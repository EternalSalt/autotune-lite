// Tests for the piano-roll data model and the capture / edit playback path (no JUCE needed)
//   g++ -O2 -std=c++17 -I../Source edit_test.cpp -o edit_test && ./edit_test
#include "AutoTuneCore.h"
#include <array>
#include <cstdio>
#include <random>

using namespace atune;

static int failures = 0;
static void check (const char* name, bool ok, const char* detail = "")
{
    std::printf ("[%s] %-52s %s\n", ok ? " ok " : "FAIL", name, detail);
    if (! ok) ++failures;
}
static double midiToHz (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }
static double hzToMidi (double f) { return 69.0 + 12.0 * std::log2 (f / 440.0); }

//------------------------------------------------------------------------------
// Part 1: note detection on synthetic pitch frames
static void testDetection()
{
    std::printf ("\n--- note detection / edit operations ---\n");
    std::mt19937 rng (5);
    std::normal_distribution<float> noise (0.0f, 0.05f);

    PitchTrack t;
    t.prepare (256, 4000);

    auto fill = [&] (int a, int b, float midi, float vib)
    {
        for (int f = a; f <= b; ++f) t.write (f, midi + vib * std::sin (2.0f * 3.14159f * (float) f / 30.0f) + noise (rng));
    };
    fill (100, 199, 60.0f, 0.4f);            // C4 with +-40 cent vibrato
    t.write (150, -1.0f);                     // one dropped frame inside the note (must be bridged)
    fill (208, 299, 64.3f, 0.1f);            // E4, 30 cents sharp, after an 8-frame gap
    fill (300, 349, 67.0f, 0.1f);            // G4 immediately after (legato jump)
    fill (360, 362, 72.0f, 0.0f);            // 3-frame blip -> ignored
    fill (400, 404, 55.0f, 0.0f);            // 5-frame blip -> ignored

    const int cMajor = scaleMaskForIndex (1);
    auto list = detectAll (t, 0, cMajor);

    char buf[200];
    std::snprintf (buf, sizeof buf, "found %zu notes", list.notes.size());
    check ("three notes found (blips ignored, dropout bridged)", list.notes.size() == 3, buf);
    if (list.notes.size() != 3) return;

    const float exp[3] = { 60.0f, 64.3f, 67.0f };
    const float tgt[3] = { 60.0f, 64.0f, 67.0f };
    bool okPitch = true, okTarget = true;
    for (int i = 0; i < 3; ++i)
    {
        okPitch  = okPitch  && std::abs (list.notes[(size_t) i].detected - exp[i]) < 0.1f;
        okTarget = okTarget && std::abs (list.notes[(size_t) i].target   - tgt[i]) < 0.001f;
    }
    check ("detected pitch = median of the sung note", okPitch);
    check ("initial targets snapped to C major", okTarget);

    std::snprintf (buf, sizeof buf, "note 1 spans %lld..%lld (expected 25600..51200)", (long long) list.notes[0].start, (long long) list.notes[0].end);
    check ("note boundaries land on the right frames", list.notes[0].start == 100 * 256 && list.notes[0].end == 200 * 256, buf);
    check ("legato jump E4 -> G4 is split exactly", list.notes[1].end == 300 * 256 && list.notes[2].start == 300 * 256);
    check ("lookup by timeline position", list.indexAt (120 * 256) == 0 && list.indexAt (204 * 256) == -1 && list.indexAt (250 * 256) == 1);

    // split / merge
    NoteList edited = list;
    const bool split = splitNoteAt (edited, 150 * 256 + 37, t);
    check ("split: makes two notes on a frame boundary", split && edited.notes.size() == 4 && edited.notes[0].end == 150 * 256 && edited.notes[1].start == 150 * 256);
    check ("split: refuses to cut too close to an edge", ! splitNoteAt (edited, 102 * 256, t));
    const bool merged = mergeWithNext (edited, 0, t);
    check ("merge: back to three notes with the original span", merged && edited.notes.size() == 3 && edited.notes[0].start == 100 * 256 && edited.notes[0].end == 200 * 256);

    // re-detect only a changed region, keep edits elsewhere
    list.notes[0].target = 62.0f;                          // user edit on note 1
    list.notes[2].correct = false;                         // user bypassed note 3
    for (int f = 208; f <= 299; ++f) t.write (f, 65.0f);   // pretend we re-captured the E4 region as F4
    t.resetDirty();
    auto redone = redetectRange (list, t, 250, 260, 0, cMajor);
    check ("re-detect keeps edits outside the changed region",
           redone.notes.size() == 3 && redone.notes[0].target == 62.0f && ! redone.notes[2].correct);
    check ("re-detect replaces the changed note", std::abs (redone.notes[1].detected - 65.0f) < 0.01f && redone.notes[1].target == 65.0f);

    snapAllToScale (redone, 0, scaleMaskForIndex (1));
    check ("snap-all resets every target to the scale", redone.notes[0].target == 60.0f && redone.notes[1].target == 65.0f);
}

//------------------------------------------------------------------------------
// Part 2: capture -> detect -> edit -> playback through the real engine
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
            y += (1.0 / k + 2.0 * std::exp (-std::pow ((fk - 700.0) / 300.0, 2.0)) / k) * std::sin (k * phase + 0.3 * k);
        }
        return (float) (0.12 * y);
    }
};

static double measureMidi (const std::vector<float>& x, size_t start, size_t len, double sr, double expectMidi)
{
    const double expect = midiToHz (expectMidi);
    const int lo = (int) (sr / expect / 1.15), hi = (int) (sr / expect * 1.15);
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
    return hzToMidi (sr / ((double) best + frac));
}

static std::vector<float> runEngine (double sr, std::vector<float> audio, const Params& p, EditContext ctx, std::int64_t hostStart, int block = 256)
{
    Engine e;
    e.prepare (sr, 1);
    e.setParams (p);
    for (size_t i = 0; i < audio.size(); i += (size_t) block)
    {
        ctx.timelineAtBlockStart = hostStart + (std::int64_t) i;
        e.setEditContext (ctx);
        float* ptr = audio.data() + i;
        e.process (&ptr, 1, (int) std::min<size_t> ((size_t) block, audio.size() - i));
    }
    return audio;
}

static void testEndToEnd()
{
    std::printf ("\n--- capture -> detect -> edit -> playback (engine) ---\n");
    const double sr = 44100.0;
    const std::int64_t hostStart = 100000;             // the clip does not start at timeline zero
    const double lead = 0.5, noteLen = 1.2;
    const double sung[3] = { 60.35, 63.70, 67.40 };    // C4 +35c, E4 -30c, G4 +40c   (all "out of tune")

    // The vocal: 0.5 s silence, then three notes with +-20 cent vibrato
    std::vector<float> vocal ((size_t) (sr * (lead + 3 * noteLen + 0.4)), 0.0f);
    {
        Voice v (sr);
        for (size_t i = (size_t) (sr * lead); i < (size_t) (sr * (lead + 3 * noteLen)); ++i)
        {
            const double t = (double) i / sr - lead;
            const int k = (int) (t / noteLen);
            const double m = sung[k] + 0.2 * std::sin (2.0 * kPi * 5.0 * t);
            vocal[i] = v.next (midiToHz (m));
        }
    }
    const int cMajor = scaleMaskForIndex (1);

    // --- pass 1: LIVE mode with Capture armed (what happens while you sing / play the clip once)
    Engine probe; probe.prepare (sr, 1);
    PitchTrack track;
    track.prepare (probe.hopSamples(), (std::int64_t) (60.0 * sr / probe.hopSamples()));
    Params live; live.retuneMs = 0.0f; live.scaleMask = cMajor;
    EditContext capCtx; capCtx.capture = true; capCtx.timelineValid = true; capCtx.track = &track;
    auto liveOut = runEngine (sr, vocal, live, capCtx, hostStart);

    const int L = probe.latencySamples();
    auto noteMid = [&] (int k) { return (size_t) (sr * (lead + (k + 0.5) * noteLen)); };
    char buf[200];

    // live path still works while capturing
    double worst = 0;
    const double tgtMidi[3] = { 60.0, 64.0, 67.0 };
    for (int k = 0; k < 3; ++k) worst = std::max (worst, std::abs (measureMidi (liveOut, noteMid (k) + (size_t) L, 4096, sr, tgtMidi[k]) - tgtMidi[k]));
    std::snprintf (buf, sizeof buf, "worst error %.1f cents", worst * 100.0);
    check ("live mode still tunes while capturing", worst < 0.06, buf);

    // --- detect
    auto list = std::make_shared<NoteList> (detectAll (track, 0, cMajor));
    std::snprintf (buf, sizeof buf, "found %zu notes", list->notes.size());
    check ("capture produced 3 notes at the right place", list->notes.size() == 3 && list->notes[0].start > hostStart, buf);
    if (list->notes.size() != 3) return;
    const double startErr = std::abs ((double) list->notes[0].start - ((double) hostStart + sr * lead)) / sr * 1000.0;
    std::snprintf (buf, sizeof buf, "note 1 starts %.1f ms from where it was sung", startErr);
    check ("notes are keyed to the host timeline", startErr < 40.0, buf);
    bool detOk = true;
    for (int k = 0; k < 3; ++k) detOk = detOk && std::abs (list->notes[(size_t) k].detected - sung[k]) < 0.1;
    check ("detected pitches match what was sung", detOk);

    // --- pass 2: EDIT mode, fresh engine, same audio at the same timeline positions
    Params edit; edit.retuneMs = 0.0f; edit.scaleMask = cMajor;   // note: scale is irrelevant inside notes
    EditContext editCtx; editCtx.editMode = true; editCtx.timelineValid = true; editCtx.notes = list.get();

    auto out1 = runEngine (sr, vocal, edit, editCtx, hostStart);
    worst = 0;
    for (int k = 0; k < 3; ++k) worst = std::max (worst, std::abs (measureMidi (out1, noteMid (k) + (size_t) L, 4096, sr, tgtMidi[k]) - tgtMidi[k]));
    std::snprintf (buf, sizeof buf, "worst error %.1f cents", worst * 100.0);
    check ("edit playback lands every note on its target", worst < 0.06, buf);

    // user drags note 2 up two semitones (E4 -> F#4) and bypasses note 3
    auto edited = std::make_shared<NoteList> (*list);
    edited->notes[1].target = 66.0f;
    edited->notes[2].correct = false;
    editCtx.notes = edited.get();
    auto out2 = runEngine (sr, vocal, edit, editCtx, hostStart);

    const double n1 = measureMidi (out2, noteMid (0) + (size_t) L, 4096, sr, 60.0);
    const double n2 = measureMidi (out2, noteMid (1) + (size_t) L, 4096, sr, 66.0);
    const double n3 = measureMidi (out2, noteMid (2) + (size_t) L, 4096, sr, 67.4);
    const double n3ref = measureMidi (vocal, noteMid (2), 4096, sr, 67.4);     // same window, unprocessed input
    std::snprintf (buf, sizeof buf, "note2 -> %+.1f c from F#4", (n2 - 66.0) * 100.0);
    check ("dragging a note moves the output to the new pitch", std::abs (n2 - 66.0) < 0.06, buf);
    std::snprintf (buf, sizeof buf, "note3 output %+.2f c vs the raw input", (n3 - n3ref) * 100.0);
    check ("a bypassed note is left exactly as sung", std::abs (n3 - n3ref) < 0.03, buf);
    check ("untouched notes are unaffected by edits elsewhere", std::abs (n1 - 60.0) < 0.06);

    // drift correction: 100% flattens the +-20c vibrato, 0% keeps it but still moves the note
    auto spread = [&] (const std::vector<float>& x, int k, double centre)
    {
        double lo = 1e9, hi = -1e9, mean = 0; int cnt = 0;
        for (double off = -0.35; off <= 0.35; off += 0.05)      // slide a 46 ms window across the middle of the note
        {
            const size_t at = noteMid (k) + (size_t) L - 1024 + (size_t) (long) std::llround (sr * (off + 0.35));
            const double m = measureMidi (x, at - (size_t) (sr * 0.35), 2048, sr, centre);
            lo = std::min (lo, m); hi = std::max (hi, m); mean += m; ++cnt;
        }
        return std::array<double, 2> { (hi - lo) * 100.0, mean / cnt };
    };
    Params keep = edit; keep.driftCorrection = 0.0f;
    editCtx.notes = list.get();
    auto outKeep = runEngine (sr, vocal, keep, editCtx, hostStart);
    auto sFlat = spread (out1, 0, 60.0), sKeep = spread (outKeep, 0, 60.0);
    std::snprintf (buf, sizeof buf, "vibrato spread: drift 100%% -> %.1f c,  drift 0%% -> %.1f c (mean %.2f)", sFlat[0], sKeep[0], sKeep[1]);
    check ("drift 100% flattens vibrato, 0% keeps it", sFlat[0] < 8.0 && sKeep[0] > 20.0 && std::abs (sKeep[1] - 60.0) < 0.08, buf);

    // --- fallbacks keep the plugin usable live
    EditContext emptyCtx; emptyCtx.editMode = true; emptyCtx.timelineValid = true;
    NoteList empty; emptyCtx.notes = &empty;
    auto out3 = runEngine (sr, vocal, edit, emptyCtx, hostStart);
    worst = 0;
    for (int k = 0; k < 3; ++k) worst = std::max (worst, std::abs (measureMidi (out3, noteMid (k) + (size_t) L, 4096, sr, tgtMidi[k]) - tgtMidi[k]));
    std::snprintf (buf, sizeof buf, "worst error %.1f cents", worst * 100.0);
    check ("edit mode, no notes here -> falls back to live tuning", worst < 0.06, buf);

    EditContext stopped = editCtx; stopped.timelineValid = false;      // transport stopped / no host position
    auto out4 = runEngine (sr, vocal, edit, stopped, hostStart);
    worst = 0;
    for (int k = 0; k < 3; ++k) worst = std::max (worst, std::abs (measureMidi (out4, noteMid (k) + (size_t) L, 4096, sr, tgtMidi[k]) - tgtMidi[k]));
    check ("edit mode, transport stopped -> falls back to live tuning", worst < 0.06);

    // block-size independence of the timeline mapping
    auto a = runEngine (sr, vocal, edit, editCtx, hostStart, 64), b = runEngine (sr, vocal, edit, editCtx, hostStart, 1000);
    double md = 0; for (size_t i = 0; i < a.size(); ++i) md = std::max (md, (double) std::abs (a[i] - b[i]));
    std::snprintf (buf, sizeof buf, "max difference %.2e", md);
    check ("edit playback independent of host block size", md < 1e-5, buf);
}

int main()
{
    testDetection();
    testEndToEnd();
    std::printf ("\n%s (%d failure%s)\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
