// Offline sanity tests for AutoTuneCore.h  (no JUCE needed)
//   g++ -O2 -std=c++17 -I../Source offline_test.cpp -o offline_test && ./offline_test
#include "AutoTuneCore.h"
#include <cstdio>
#include <functional>
#include <random>

using namespace atune;

static double cents (double f, double ref) { return 1200.0 * std::log2 (f / ref); }
static double midiToHz (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }

// Voice-like source: harmonic series with a formant bump near 700 Hz.
struct Voice
{
    double sr, phase = 0.0;
    explicit Voice (double s) : sr (s) {}
    float next (double f0)
    {
        phase += 2.0 * kPi * f0 / sr;
        if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        double y = 0.0;
        for (int k = 1; k * f0 < 0.45 * sr && k <= 40; ++k)
        {
            const double fk = k * f0;
            const double a  = 1.0 / k + 2.0 * std::exp (-std::pow ((fk - 700.0) / 300.0, 2.0)) / k;
            y += a * std::sin (k * phase + 0.3 * k);
        }
        return (float) (0.12 * y);
    }
};

// Independent pitch measurement (plain autocorrelation) around an expected frequency.
static double measureHz (const std::vector<float>& x, size_t start, size_t len, double sr, double expect)
{
    const int lo = (int) (sr / expect / 1.12), hi = (int) (sr / expect * 1.12);
    std::vector<double> r ((size_t) hi + 2, 0.0);
    for (int t = lo - 1; t <= hi + 1; ++t)
    {
        double xy = 0, xx = 1e-12, yy = 1e-12;
        for (size_t i = start; i + (size_t) t < start + len; ++i)
        {
            xy += (double) x[i] * x[i + (size_t) t];
            xx += (double) x[i] * x[i];
            yy += (double) x[i + (size_t) t] * x[i + (size_t) t];
        }
        r[(size_t) t] = xy / std::sqrt (xx * yy);
    }
    int best = lo;
    for (int t = lo; t <= hi; ++t) if (r[(size_t) t] > r[(size_t) best]) best = t;
    const double a = r[(size_t) best - 1], b = r[(size_t) best], c = r[(size_t) best + 1];
    const double den = a - 2 * b + c;
    const double frac = std::abs (den) > 1e-12 ? 0.5 * (a - c) / den : 0.0;
    return sr / ((double) best + frac);
}

static std::vector<float> run (double sr, std::vector<float> in, const Params& p, int block = 256)
{
    Engine e;
    e.prepare (sr, 1);
    e.setParams (p);
    for (size_t i = 0; i < in.size(); i += (size_t) block)
    {
        float* ptr = in.data() + i;
        e.process (&ptr, 1, (int) std::min<size_t> ((size_t) block, in.size() - i));
    }
    return in;
}

static int failures = 0;
static void check (const char* name, bool ok, const char* detail)
{
    std::printf ("[%s] %-46s %s\n", ok ? " ok " : "FAIL", name, detail);
    if (! ok) ++failures;
}

int main()
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        std::printf ("\n=== sample rate %.0f ===\n", sr);
        char buf[200];

        // 1. 30 cents sharp of A3 -> chromatic, instant retune -> should land on 220 Hz
        {
            Voice v (sr);
            std::vector<float> in ((size_t) (sr * 2.0));
            const double f = 220.0 * std::pow (2.0, 30.0 / 1200.0);
            for (auto& s : in) s = v.next (f);
            Engine probe; probe.prepare (sr, 1);
            Params p; p.retuneMs = 0.0f;
            auto out = run (sr, in, p);
            const double hzIn  = measureHz (in,  (size_t) (sr * 1.0), 8192, sr, 220.0);
            const double hzOut = measureHz (out, (size_t) (sr * 1.0), 8192, sr, 220.0);
            std::snprintf (buf, sizeof buf, "in %+.1f c  ->  out %+.1f c  (latency %d smp = %.1f ms)",
                           cents (hzIn, 220.0), cents (hzOut, 220.0), probe.latencySamples(), 1000.0 * probe.latencySamples() / sr);
            check ("30c sharp A3 snaps to A3", std::abs (cents (hzOut, 220.0)) < 3.0, buf);
        }

        // 2. 40 cents flat of E4
        {
            Voice v (sr);
            std::vector<float> in ((size_t) (sr * 2.0));
            const double f = midiToHz (64.0) * std::pow (2.0, -40.0 / 1200.0);
            for (auto& s : in) s = v.next (f);
            Params p; p.retuneMs = 0.0f;
            auto out = run (sr, in, p);
            const double hzOut = measureHz (out, (size_t) (sr * 1.0), 8192, sr, midiToHz (64.0));
            std::snprintf (buf, sizeof buf, "out %+.1f c from E4", cents (hzOut, midiToHz (64.0)));
            check ("40c flat E4 snaps to E4", std::abs (cents (hzOut, midiToHz (64.0))) < 3.0, buf);
        }

        // 3. C major: 303 Hz (between D4 and D#4, nearer D4) must go to D4, not D#4
        {
            Voice v (sr);
            std::vector<float> in ((size_t) (sr * 2.0));
            for (auto& s : in) s = v.next (303.0);
            Params p; p.retuneMs = 0.0f; p.rootNote = 0; p.scaleMask = scaleMaskForIndex (1);
            auto out = run (sr, in, p);
            const double hzOut = measureHz (out, (size_t) (sr * 1.0), 8192, sr, midiToHz (62.0));
            std::snprintf (buf, sizeof buf, "out %+.1f c from D4", cents (hzOut, midiToHz (62.0)));
            check ("C major keeps notes in scale (-> D4)", std::abs (cents (hzOut, midiToHz (62.0))) < 3.0, buf);
        }

        // 4. Vibrato +-35 cents at 5.5 Hz should be flattened by instant retune
        {
            Voice v (sr);
            std::vector<float> in ((size_t) (sr * 3.0));
            for (size_t i = 0; i < in.size(); ++i)
                in[i] = v.next (220.0 * std::pow (2.0, 35.0 / 1200.0 * std::sin (2 * kPi * 5.5 * (double) i / sr)));
            Params p; p.retuneMs = 0.0f;
            auto out = run (sr, in, p);
            double worstIn = 0, worstOut = 0;
            for (double t = 1.0; t < 2.6; t += 0.1)
            {
                const size_t s = (size_t) (sr * t);
                worstIn  = std::max (worstIn,  std::abs (cents (measureHz (in,  s, 2048, sr, 220.0), 220.0)));
                worstOut = std::max (worstOut, std::abs (cents (measureHz (out, s, 2048, sr, 220.0), 220.0)));
            }
            std::snprintf (buf, sizeof buf, "worst deviation in %.1f c -> out %.1f c", worstIn, worstOut);
            check ("vibrato is flattened", worstOut < worstIn * 0.5 && worstOut < 15.0, buf);
        }

        // 5. Already in tune + amount=1: output waveform should match the input
        //    (PSOLA may shift the waveform phase by a fraction of a period, so we
        //    compare after finding the best alignment within +-1 period)
        {
            Voice v (sr);
            std::vector<float> in ((size_t) (sr * 2.0));
            for (auto& s : in) s = v.next (220.0);
            Engine probe; probe.prepare (sr, 1);
            Params p; p.retuneMs = 0.0f;
            auto out = run (sr, in, p);
            const int L = probe.latencySamples();
            const int T = (int) (sr / 220.0);
            double bestNcc = -1.0;
            for (int s = -T; s <= T; ++s)
            {
                double xy = 0, xx = 1e-12, yy = 1e-12;
                for (size_t i = (size_t) (sr * 1.0); i < (size_t) (sr * 1.5); ++i)
                {
                    const double a = out[i], b = in[(size_t) ((long) i - L + s)];
                    xy += a * b; xx += a * a; yy += b * b;
                }
                bestNcc = std::max (bestNcc, xy / std::sqrt (xx * yy));
            }
            std::snprintf (buf, sizeof buf, "waveform correlation with dry = %.5f", bestNcc);
            check ("in-tune input passes cleanly", bestNcc > 0.995, buf);
        }

        // 6. Level preserved when shifting by a semitone-ish amount
        {
            Voice v (sr);
            std::vector<float> in ((size_t) (sr * 2.0));
            const double f = 220.0 * std::pow (2.0, 45.0 / 1200.0);
            for (auto& s : in) s = v.next (f);
            Params p; p.retuneMs = 0.0f;
            auto out = run (sr, in, p);
            double ei = 0, eo = 0;
            for (size_t i = (size_t) (sr * 1.0); i < (size_t) (sr * 1.9); ++i) { ei += in[i] * in[i]; eo += out[i] * out[i]; }
            std::snprintf (buf, sizeof buf, "level change %.2f dB", 10 * std::log10 (eo / ei));
            check ("loudness preserved", std::abs (10 * std::log10 (eo / ei)) < 1.0, buf);
        }

        // 7. No clicks: largest sample-to-sample jump of output vs input
        {
            Voice v (sr);
            std::vector<float> in ((size_t) (sr * 2.0));
            for (size_t i = 0; i < in.size(); ++i)
                in[i] = v.next (200.0 + 100.0 * (double) i / (double) in.size());   // 200 -> 300 Hz glide
            Params p; p.retuneMs = 0.0f;
            auto out = run (sr, in, p);
            double jIn = 0, jOut = 0;
            for (size_t i = (size_t) (sr * 0.5) + 1; i < in.size(); ++i)
            {
                jIn  = std::max (jIn,  (double) std::abs (in[i]  - in[i - 1]));
                jOut = std::max (jOut, (double) std::abs (out[i] - out[i - 1]));
            }
            std::snprintf (buf, sizeof buf, "max step in %.3f / out %.3f", jIn, jOut);
            check ("no clicks on a pitch glide", jOut < 2.0 * jIn, buf);
        }

        // 8. Noise (unvoiced) must pass through unchanged (delayed)
        {
            std::mt19937 rng (1);
            std::normal_distribution<float> nd (0.0f, 0.1f);
            std::vector<float> in ((size_t) (sr * 1.5));
            for (auto& s : in) s = nd (rng);
            Engine probe; probe.prepare (sr, 1);
            Params p; p.retuneMs = 0.0f;
            auto out = run (sr, in, p);
            const int L = probe.latencySamples();
            double err = 0, ref = 0;
            for (size_t i = (size_t) (sr * 0.8); i < in.size(); ++i)
            {
                const double d = (double) out[i] - in[i - (size_t) L];
                err += d * d; ref += (double) in[i - (size_t) L] * in[i - (size_t) L];
            }
            std::snprintf (buf, sizeof buf, "error vs delayed dry = %.1f dB", 10 * std::log10 (err / ref + 1e-20));
            check ("breaths / noise pass through", 10 * std::log10 (err / ref + 1e-20) < -40.0, buf);
        }

        // 9. Silence in -> silence out, host block size 37 (odd) must work
        {
            std::vector<float> in ((size_t) (sr * 0.5), 0.0f);
            Params p;
            auto out = run (sr, in, p, 37);
            float mx = 0; for (float s : out) mx = std::max (mx, std::abs (s));
            std::snprintf (buf, sizeof buf, "peak %.6f", mx);
            check ("silence stays silent, odd block size", mx == 0.0f, buf);
        }

        // 10. Block-size independence: 64 vs 1024 give the same result
        {
            Voice v (sr);
            std::vector<float> in ((size_t) (sr * 1.2));
            const double f = 220.0 * std::pow (2.0, 25.0 / 1200.0);
            for (auto& s : in) s = v.next (f);
            Params p; p.retuneMs = 5.0f;
            auto a = run (sr, in, p, 64), b = run (sr, in, p, 1024);
            double md = 0; for (size_t i = 0; i < a.size(); ++i) md = std::max (md, (double) std::abs (a[i] - b[i]));
            std::snprintf (buf, sizeof buf, "max difference %.2e", md);
            check ("output independent of block size", md < 1e-6, buf);
        }
    }

    std::printf ("\n%s (%d failure%s)\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
