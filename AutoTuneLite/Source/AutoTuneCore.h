// AutoTuneCore.h  --  header-only, no JUCE dependency.
//
// Real-time monophonic pitch correction:
//   1. PitchDetector : YIN on a decimated copy, refined at full sample rate
//   2. Scale quantiser: snaps the detected note to the nearest note of a key/scale
//   3. PSOLA shifter : pitch-synchronous grains, so formants (the "vowel" colour)
//                      stay put and the voice does not turn into a chipmunk
//
// Latency is fixed at ~3 periods of the lowest pitch we track (see kMinPitchHz).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "AutoTuneEdit.h"

namespace atune
{
// Lowest / highest fundamental we try to track. Lower kMinPitchHz = can follow
// deeper voices, but costs more latency (latency ~= 3 / kMinPitchHz seconds).
constexpr double kMinPitchHz = 85.0;
constexpr double kMaxPitchHz = 1000.0;
constexpr double kPi = 3.14159265358979323846;

//------------------------------------------------------------------------------
struct Params
{
    int   rootNote       = 0;      // 0 = C ... 11 = B
    int   scaleMask      = 0xFFF;  // bit i set => (root + i) semitones is an allowed note
    float retuneMs       = 20.0f;  // 0 = instant "hard tune" effect, larger = more natural
    float amount         = 1.0f;   // 0..1 how much of the correction is applied
    float transposeSemis = 0.0f;   // extra shift after correction
    float driftCorrection = 1.0f;  // Edit mode: 1 = flatten pitch drift inside a note, 0 = keep natural drift / vibrato
};

// Per-block information the wrapper hands to the engine for the piano-roll editor.
struct EditContext
{
    bool editMode      = false;   // use the note list instead of live scale snapping
    bool capture       = false;   // record the detected pitch into `track`
    bool timelineValid = false;   // host is playing and told us where we are
    std::int64_t timelineAtBlockStart = 0;   // host position (samples) of the first sample of this block
    const NoteList* notes = nullptr;
    PitchTrack*     track = nullptr;
};

inline int maskFromDegrees (std::initializer_list<int> degrees)
{
    int m = 0;
    for (int d : degrees) m |= (1 << d);
    return m;
}

// Order must match the "Scale" choice list in PluginProcessor.cpp
inline int scaleMaskForIndex (int index)
{
    switch (index)
    {
        case 0:  return 0xFFF;                                   // Chromatic
        case 1:  return maskFromDegrees ({ 0, 2, 4, 5, 7, 9, 11 }); // Major
        case 2:  return maskFromDegrees ({ 0, 2, 3, 5, 7, 8, 10 }); // Natural minor
        case 3:  return maskFromDegrees ({ 0, 2, 3, 5, 7, 8, 11 }); // Harmonic minor
        case 4:  return maskFromDegrees ({ 0, 2, 4, 7, 9 });        // Major pentatonic
        case 5:  return maskFromDegrees ({ 0, 3, 5, 7, 10 });       // Minor pentatonic
        case 6:  return maskFromDegrees ({ 0, 3, 5, 6, 7, 10 });    // Blues
        case 7:  return maskFromDegrees ({ 0, 2, 3, 5, 7, 9, 10 }); // Dorian
        default: return 0xFFF;
    }
}

//------------------------------------------------------------------------------
class PitchDetector
{
public:
    struct Result
    {
        bool  voiced = false;
        float period = 0.0f;  // in samples at the host sample rate
    };

    void prepare (double sampleRate)
    {
        sr     = sampleRate;
        decim  = std::max (1, (int) std::lround (sr / 11025.0));
        const double dsr = sr / decim;

        minLag = std::max (2, (int) std::floor (dsr / kMaxPitchHz));
        maxLag = (int) std::ceil (dsr / kMinPitchHz);
        W      = std::max (256, 2 * maxLag);
        N      = W + maxLag;

        fullW      = std::max (1024, (int) std::lround (sr * 0.023));
        maxFullLag = maxLag * decim + decim + 2;

        dec.assign ((size_t) N, 0.0f);
        d.assign ((size_t) maxLag + 2, 0.0f);
        cm.assign ((size_t) maxLag + 2, 0.0f);
        dv.assign ((size_t) (2 * decim + 3), 0.0);
    }

    int fullWindow() const { return fullW; }

    // `ring` is a power-of-two circular buffer, `n` = number of samples written
    // so far (newest sample is ring[(n-1) & mask]).
    Result analyse (const float* ring, std::uint64_t mask, std::int64_t n)
    {
        auto at = [&] (std::int64_t i) { return ring[(std::uint64_t) i & mask]; };

        // 1. decimate (box filter) the most recent N*decim samples
        const std::int64_t base = n - (std::int64_t) N * decim;
        for (int i = 0; i < N; ++i)
        {
            float s = 0.0f;
            for (int k = 0; k < decim; ++k) s += at (base + (std::int64_t) i * decim + k);
            dec[(size_t) i] = s / (float) decim;
        }

        // 2. level gate
        double e = 0.0;
        for (int i = N - W; i < N; ++i) e += (double) dec[(size_t) i] * dec[(size_t) i];
        if (std::sqrt (e / W) < kGate) return {};

        // 3. YIN difference function + cumulative mean normalisation
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            double sum = 0.0;
            for (int j = 0; j < W; ++j)
            {
                const double diff = (double) dec[(size_t) j] - dec[(size_t) (j + tau)];
                sum += diff * diff;
            }
            d[(size_t) tau] = (float) sum;
        }
        cm[0] = 1.0f;
        double run = 0.0;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            run += d[(size_t) tau];
            cm[(size_t) tau] = run > 0.0 ? (float) (d[(size_t) tau] * tau / run) : 1.0f;
        }

        // 4. first dip under threshold (avoids octave-down errors)
        int tau = -1;
        for (int t = minLag; t < maxLag; ++t)
        {
            if (cm[(size_t) t] < 0.15f)
            {
                while (t + 1 < maxLag && cm[(size_t) t + 1] < cm[(size_t) t]) ++t;
                tau = t;
                break;
            }
        }
        if (tau < 0)
        {
            int best = minLag;
            for (int t = minLag; t < maxLag; ++t)
                if (cm[(size_t) t] < cm[(size_t) best]) best = t;
            if (cm[(size_t) best] > 0.25f) return {};
            tau = best;
        }

        const float coarse = ((float) tau + parabolic (cm[(size_t) tau - 1], cm[(size_t) tau], cm[(size_t) tau + 1])) * (float) decim;

        // 5. refine at full sample rate around the coarse lag
        int lo = std::max (4, (int) std::lround (coarse) - decim - 1);
        int hi = std::min (maxFullLag, (int) std::lround (coarse) + decim + 1);
        if (hi - lo < 2) return {};

        int bestIdx = 0;
        for (int t = lo; t <= hi; ++t)
        {
            double sum = 0.0;
            for (int j = 0; j < fullW; ++j)
            {
                const double a = at (n - fullW + j);
                const double b = at (n - fullW + j - t);
                sum += (a - b) * (a - b);
            }
            dv[(size_t) (t - lo)] = sum;
            if (sum < dv[(size_t) bestIdx]) bestIdx = t - lo;
        }

        float frac = 0.0f;
        if (bestIdx > 0 && bestIdx < hi - lo)
            frac = parabolic ((float) dv[(size_t) bestIdx - 1], (float) dv[(size_t) bestIdx], (float) dv[(size_t) bestIdx + 1]);

        Result r;
        r.voiced = true;
        r.period = (float) (lo + bestIdx) + frac;
        return r;
    }

private:
    static float parabolic (float a, float b, float c)
    {
        const float den = a - 2.0f * b + c;
        if (std::abs (den) < 1.0e-12f) return 0.0f;
        return std::clamp (0.5f * (a - c) / den, -1.0f, 1.0f);
    }

    static constexpr double kGate = 0.002;  // ~ -54 dBFS RMS

    double sr = 44100.0;
    int decim = 4, minLag = 11, maxLag = 130, W = 260, N = 390, fullW = 1024, maxFullLag = 530;
    std::vector<float>  dec, d, cm;
    std::vector<double> dv;
};

//------------------------------------------------------------------------------
class Engine
{
public:
    static constexpr int  kMaxChannels = 2;
    static constexpr std::int64_t kRingSize    = 16384;                 // power of two
    static constexpr std::uint64_t kMask = (std::uint64_t) kRingSize - 1;
    static constexpr std::int64_t kStart       = 1L << 20;              // keeps indices positive

    void prepare (double sampleRate, int numChannels)
    {
        sr  = sampleRate;
        nCh = std::clamp (numChannels, 1, kMaxChannels);
        detector.prepare (sr);

        Tmax    = sr / kMinPitchHz;
        Tmin    = sr / kMaxPitchHz;
        latency = (int) std::ceil (3.0 * Tmax) + 16;
        hop     = std::max (32, (int) std::lround (sr * 0.006));

        mixCoef = 1.0f - std::exp (-1.0f / (0.008f * (float) sr));
        for (int c = 0; c < kMaxChannels; ++c)
        {
            inBuf[c].assign ((size_t) kRingSize, 0.0f);
            outBuf[c].assign ((size_t) kRingSize, 0.0f);
        }
        mono.assign ((size_t) kRingSize, 0.0f);
        scores.assign ((size_t) (2 * (int) (Tmax / 10.0) + 16), 0.0);
        reset();
        setParams (params);
    }

    void reset()
    {
        for (int c = 0; c < kMaxChannels; ++c)
        {
            std::fill (inBuf[c].begin(), inBuf[c].end(), 0.0f);
            std::fill (outBuf[c].begin(), outBuf[c].end(), 0.0f);
        }
        std::fill (mono.begin(), mono.end(), 0.0f);
        n = kStart;
        lastC = (double) kStart;
        tNext = (double) kStart;
        hopCounter = 0;
        Tin = Tdet = sr / 200.0;
        wasVoiced = false;
        unvoicedHops = 0;
        sCur = sTarget = 0.0f;
        wetMix = wetTarget = 0.0f;
        blockN0 = kStart;
        lastCapFrame = -1;
    }

    void setParams (const Params& p)
    {
        params = p;
        mask12 = p.scaleMask & 0xFFF;
        if (mask12 == 0) mask12 = 0xFFF;
        sCoef = p.retuneMs <= 0.5f ? 1.0f : 1.0f - std::exp (-1.0f / (0.001f * p.retuneMs * (float) sr));
    }

    int latencySamples() const { return latency; }
    int hopSamples() const     { return hop; }

    void setEditContext (const EditContext& c) { ctx = c; }

    // For metering in a GUI: MIDI note numbers (-1 = nothing detected)
    float detectedMidi() const { return detMidi.load(); }
    float targetMidi()   const { return tgtMidi.load(); }

    void process (float* const* ch, int numChannels, int numSamples)
    {
        const int chn = std::min (numChannels, nCh);
        const float inv = 1.0f / (float) chn;
        blockN0 = n;

        for (int i = 0; i < numSamples; ++i)
        {
            const std::uint64_t idx = (std::uint64_t) n & kMask;
            float m = 0.0f;
            for (int c = 0; c < chn; ++c)
            {
                const float x = ch[c][i];
                inBuf[c][idx] = x;
                m += x;
            }
            mono[idx] = m * inv;
            ++n;

            if (++hopCounter >= hop) { hopCounter = 0; updatePitch(); }

            sCur   += sCoef   * (sTarget   - sCur);
            wetMix += mixCoef * (wetTarget - wetMix);

            schedule();

            const std::uint64_t oi = (std::uint64_t) (n - 1 - latency) & kMask;
            for (int c = 0; c < chn; ++c)
            {
                const float wet = outBuf[c][oi];
                outBuf[c][oi] = 0.0f;
                const float dry = inBuf[c][oi];
                ch[c][i] = dry + wetMix * (wet - dry);
            }
        }
    }

private:
    //--------------------------------------------------------------------------
    // Host-timeline position (samples) of input sample index `idx`.
    std::int64_t timelineOf (std::int64_t idx) const { return ctx.timelineAtBlockStart + (idx - blockN0); }

    void captureFrame (std::int64_t tl, float midiOrNeg)
    {
        if (tl < 0 || ctx.track == nullptr) return;
        const std::int64_t f = tl / ctx.track->hop();
        if (lastCapFrame >= 0 && f > lastCapFrame + 1 && f - lastCapFrame <= 4)
            for (std::int64_t g = lastCapFrame + 1; g < f; ++g) ctx.track->write (g, midiOrNeg);   // fill rounding gaps
        ctx.track->write (f, midiOrNeg);
        lastCapFrame = f;
    }

    void updatePitch()
    {
        const auto r = detector.analyse (mono.data(), kMask, n);

        float midi = -1.0f;
        if (r.voiced)
        {
            const double T = std::clamp ((double) r.period, Tmin, Tmax);
            if (! wasVoiced || std::abs (T - Tdet) / Tdet > 0.15) Tdet = T;
            else                                                  Tdet += 0.5 * (T - Tdet);
            wasVoiced = true;
            unvoicedHops = 0;
            Tin = Tdet;
            midi = 69.0f + 12.0f * std::log2 ((float) (sr / Tdet) / 440.0f);
        }

        // Where on the host timeline does this pitch estimate belong? (the analysis window is centred in the past)
        const std::int64_t tl = timelineOf (n - (detector.fullWindow() + (int) Tdet) / 2);

        if (ctx.capture && ctx.timelineValid)
            captureFrame (tl, r.voiced ? midi : -1.0f);

        if (r.voiced)
        {
            const Note* note = (ctx.editMode && ctx.timelineValid && ctx.notes != nullptr) ? ctx.notes->find (tl) : nullptr;

            float shift, shownTarget;
            bool  active;
            if (note != nullptr)
            {
                // EDIT: move the whole note to its target, and optionally also flatten drift inside it
                const float offset = note->correct ? note->target - note->detected : 0.0f;
                const float drift  = note->correct ? (note->detected - midi) * params.driftCorrection : 0.0f;
                shift       = params.amount * (offset + drift) + params.transposeSemis;
                shownTarget = note->target;
                active      = (note->correct && params.amount > 0.0f) || params.transposeSemis != 0.0f;
            }
            else
            {
                // LIVE: snap the pitch we hear right now to the key / scale
                shownTarget = quantiseToScale (midi, params.rootNote, mask12);
                shift       = params.amount * (shownTarget - midi) + params.transposeSemis;
                active      = params.amount > 0.0f || params.transposeSemis != 0.0f;
            }

            sTarget   = std::clamp (shift, -12.0f, 12.0f);
            wetTarget = active ? 1.0f : 0.0f;
            detMidi.store (midi);
            tgtMidi.store (shownTarget);
        }
        else if (++unvoicedHops > 2)    // ride out one or two bad frames
        {
            wasVoiced = false;
            wetTarget = 0.0f;
            detMidi.store (-1.0f);
            tgtMidi.store (-1.0f);
        }
    }

    // Cross-correlate the candidate source position against the previous grain's
    // source position and return the best lag (in samples, sub-sample precision)
    // between them. Successive grains are therefore an exact number of true
    // pitch periods apart, which is what keeps the output free of roughness.
    double refineLag (std::int64_t ref, std::int64_t cand, int Ti, int delta)
    {
        const int H = std::max (4, Ti / 2);
        auto at = [&] (std::int64_t i) { return mono[(size_t) ((std::uint64_t) i & kMask)]; };

        const int count = 2 * delta + 1;
        int bestIdx = 0;
        for (int o = -delta; o <= delta; ++o)
        {
            double xy = 0.0, yy = 1.0e-9;
            for (int k = -H; k <= H; ++k)
            {
                const double a = at (ref + k);
                const double b = at (cand + o + k);
                xy += a * b;
                yy += b * b;
            }
            scores[(size_t) (o + delta)] = xy / std::sqrt (yy);
            if (scores[(size_t) (o + delta)] > scores[(size_t) bestIdx]) bestIdx = o + delta;
        }

        double frac = 0.0;
        if (bestIdx > 0 && bestIdx < count - 1)
        {
            const double a = scores[(size_t) bestIdx - 1], b = scores[(size_t) bestIdx], c = scores[(size_t) bestIdx + 1];
            const double den = a - 2.0 * b + c;
            if (std::abs (den) > 1.0e-15) frac = std::clamp (0.5 * (a - c) / den, -1.0, 1.0);
        }
        return (double) (cand - ref) + (double) (bestIdx - delta) + frac;
    }

    static float hermite (float xm1, float x0, float x1, float x2, float t)
    {
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    // Copy a Hann-windowed, 2-period grain from source position `c` (fractional)
    // to output position `tcExact` (fractional), with sub-sample accuracy on both.
    void addGrain (double c, int Ti, double tcExact, float gain)
    {
        const std::int64_t   firstWritable = n - 1 - latency;   // anything older has already been output
        const std::int64_t   tc = (std::int64_t) std::floor (tcExact);
        const double fo = tcExact - (double) tc;

        for (int k = -Ti; k <= Ti + 1; ++k)
        {
            if (tc + k <= firstWritable) continue;
            const double x = (double) k - fo;            // offset from grain centre
            if (std::abs (x) >= (double) Ti) continue;

            const double p  = c + x;
            const std::int64_t   i0 = (std::int64_t) std::floor (p);
            const float  u  = (float) (p - (double) i0);
            const float  w  = gain * 0.5f * (1.0f + std::cos ((float) (kPi * x / (double) Ti)));
            const std::uint64_t di = (std::uint64_t) (tc + k) & kMask;

            for (int ch = 0; ch < nCh; ++ch)
            {
                const float* src = inBuf[ch].data();
                const float v = hermite (src[(std::uint64_t) (i0 - 1) & kMask], src[(std::uint64_t) i0 & kMask],
                                         src[(std::uint64_t) (i0 + 1) & kMask], src[(std::uint64_t) (i0 + 2) & kMask], u);
                outBuf[ch][di] += v * w;
            }
        }
    }

    void schedule()
    {
        const double T      = std::clamp (Tin, Tmin, Tmax);
        const int    Ti     = (int) std::lround (T);
        const int    delta  = std::max (1, Ti / 10);
        const double ratio  = std::exp2 ((double) sCur / 12.0);
        const std::int64_t   newest = n - 1;

        for (;;)
        {
            double m = std::round ((tNext - lastC) / T);
            if (m < 0.0) m = 0.0;

            const double candExact = lastC + m * T;
            const std::int64_t   cand      = std::llround (candExact);
            if (cand + delta + Ti + 4 > newest) break;     // source audio not here yet

            double c = lastC;
            if (m >= 1.0)
            {
                const std::int64_t ref = std::llround (lastC);
                c = lastC + refineLag (ref, ref + std::llround (m * T), Ti, delta);
            }

            addGrain (c, Ti, tNext, (float) (1.0 / ratio));
            lastC = c;
            tNext += T / ratio;
        }
    }

    //--------------------------------------------------------------------------
    double sr = 44100.0;
    int    nCh = 2;
    Params params;
    int    mask12 = 0xFFF;

    PitchDetector detector;
    std::vector<float> inBuf[kMaxChannels], outBuf[kMaxChannels], mono;
    std::vector<double> scores;

    std::int64_t   n = kStart;            // samples pushed so far
    double lastC = (double) kStart;  // source centre of the previous grain (fractional)
    double tNext = (double) kStart;  // output centre of the next grain
    int    latency = 1500, hop = 256, hopCounter = 0, unvoicedHops = 0;
    double Tmin = 44.0, Tmax = 520.0, Tin = 220.0, Tdet = 220.0;
    bool   wasVoiced = false;

    float sCur = 0.0f, sTarget = 0.0f, sCoef = 1.0f;
    float wetMix = 0.0f, wetTarget = 0.0f, mixCoef = 0.01f;

    EditContext  ctx;
    std::int64_t blockN0 = kStart, lastCapFrame = -1;

    std::atomic<float> detMidi { -1.0f }, tgtMidi { -1.0f };
};

} // namespace atune
