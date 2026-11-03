// AutoTuneEdit.h  --  data model behind the piano-roll editor. No JUCE dependency.
//
//   PitchTrack : the detected pitch of the vocal, one value per "frame" (~6 ms),
//                indexed by HOST TIMELINE position. Written by the audio thread
//                while Capture is armed and the host is playing; read by the GUI.
//   Note       : a segment of that pitch curve (start/end in timeline samples),
//                with the pitch that was sung (`detected`) and the pitch it should
//                end up at (`target`) -- this is what you drag around in the editor.
//   NoteList   : immutable-by-convention, sorted, non-overlapping list of notes.
//                The GUI builds a new list for every edit and hands it to the audio
//                thread by swapping a shared_ptr.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace atune
{
//------------------------------------------------------------------------------
// Snap a (fractional) MIDI note to the nearest allowed note of a key/scale.
inline float quantiseToScale (float midi, int root, int mask12)
{
    mask12 &= 0xFFF;
    if (mask12 == 0) mask12 = 0xFFF;

    const int base = (int) std::floor (midi);
    float best = midi, bestDist = 1.0e9f;
    for (int cand = base - 6; cand <= base + 7; ++cand)
    {
        const int degree = (((cand - root) % 12) + 12) % 12;
        if (! ((mask12 >> degree) & 1)) continue;
        const float dist = std::abs ((float) cand - midi);
        if (dist < bestDist) { bestDist = dist; best = (float) cand; }
    }
    return best;
}

//------------------------------------------------------------------------------
struct Note
{
    std::int64_t start = 0;      // timeline sample, inclusive
    std::int64_t end   = 0;      // timeline sample, exclusive
    float detected     = 60.0f;  // median pitch that was actually sung (MIDI, fractional)
    float target       = 60.0f;  // where the note should land (MIDI, fractional)
    bool  correct      = true;   // false = leave this note untouched
};

struct NoteList
{
    std::vector<Note> notes;     // sorted by start, never overlapping

    int indexAt (std::int64_t pos) const
    {
        int lo = 0, hi = (int) notes.size() - 1, found = -1;
        while (lo <= hi)
        {
            const int mid = (lo + hi) / 2;
            if (notes[(size_t) mid].start <= pos) { found = mid; lo = mid + 1; }
            else                                  { hi = mid - 1; }
        }
        if (found >= 0 && pos < notes[(size_t) found].end) return found;
        return -1;
    }

    const Note* find (std::int64_t pos) const
    {
        const int i = indexAt (pos);
        return i >= 0 ? &notes[(size_t) i] : nullptr;
    }
};

//------------------------------------------------------------------------------
class PitchTrack
{
public:
    void prepare (int hopSamples, std::int64_t capacityFrames)
    {
        hop_      = hopSamples;
        capacity_ = capacityFrames;
        frames_.reset (new std::atomic<float>[(size_t) capacityFrames]);
        clear();
    }

    bool         prepared() const { return frames_ != nullptr; }
    int          hop() const      { return hop_; }
    std::int64_t capacity() const { return capacity_; }

    void clear()
    {
        for (std::int64_t i = 0; i < capacity_; ++i)
            frames_[(size_t) i].store (-1.0f, std::memory_order_relaxed);
        maxFrame_.store (-1);
        resetDirty();
    }

    // midi < 0 means "no pitch" (silence, breath, consonant)
    void write (std::int64_t f, float midi)
    {
        if (f < 0 || f >= capacity_) return;
        frames_[(size_t) f].store (midi, std::memory_order_relaxed);
        touch (f);
    }

    float read (std::int64_t f) const
    {
        if (f < 0 || f >= capacity_ || frames_ == nullptr) return -1.0f;
        return frames_[(size_t) f].load (std::memory_order_relaxed);
    }

    std::int64_t maxFrame() const { return maxFrame_.load(); }

    // Frames written since the last call (used to re-detect only what was just captured).
    bool takeDirtyRange (std::int64_t& lo, std::int64_t& hi)
    {
        lo = dirtyLo_.exchange (std::numeric_limits<std::int64_t>::max());
        hi = dirtyHi_.exchange (-1);
        return hi >= 0 && lo <= hi;
    }

    void resetDirty()
    {
        dirtyLo_.store (std::numeric_limits<std::int64_t>::max());
        dirtyHi_.store (-1);
    }

    // Re-time another track recorded at a different sample rate / hop into this one.
    void rescaleFrom (const PitchTrack& src, double srcSr, double dstSr)
    {
        if (! src.prepared() || ! prepared()) return;
        for (std::int64_t f = 0; f < capacity_; ++f)
        {
            const double seconds = (double) f * hop_ / dstSr;
            const auto sf = (std::int64_t) std::floor (seconds * srcSr / src.hop_);
            frames_[(size_t) f].store (src.read (sf), std::memory_order_relaxed);
        }
        maxFrame_.store (src.maxFrame() < 0 ? -1
                         : (std::int64_t) std::ceil ((double) src.maxFrame() * src.hop_ / srcSr * dstSr / hop_));
        resetDirty();
    }

private:
    void touch (std::int64_t f)
    {
        auto lo = dirtyLo_.load (std::memory_order_relaxed);
        while (f < lo && ! dirtyLo_.compare_exchange_weak (lo, f, std::memory_order_relaxed)) {}
        auto hi = dirtyHi_.load (std::memory_order_relaxed);
        while (f > hi && ! dirtyHi_.compare_exchange_weak (hi, f, std::memory_order_relaxed)) {}
        auto mx = maxFrame_.load (std::memory_order_relaxed);
        while (f > mx && ! maxFrame_.compare_exchange_weak (mx, f, std::memory_order_relaxed)) {}
    }

    int hop_ = 256;
    std::int64_t capacity_ = 0;
    std::unique_ptr<std::atomic<float>[]> frames_;
    std::atomic<std::int64_t> dirtyLo_ { std::numeric_limits<std::int64_t>::max() };
    std::atomic<std::int64_t> dirtyHi_ { -1 };
    std::atomic<std::int64_t> maxFrame_ { -1 };
};

//------------------------------------------------------------------------------
// Note detection
constexpr int   kMinNoteFrames = 6;      // shorter blips are ignored (~36 ms)
constexpr int   kMaxGapFrames  = 2;      // brief pitch dropouts inside a note are bridged
constexpr int   kSplitFrames   = 5;      // pitch must stay somewhere new this long to start a new note
constexpr float kSplitSemis    = 0.8f;   // ...and be at least this far from the current note
constexpr int   kMedianWindow  = 30;     // frames used to judge "what pitch is this note at"

namespace detail
{
inline float medianOf (std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    const auto mid = v.begin() + (std::ptrdiff_t) (v.size() / 2);
    std::nth_element (v.begin(), mid, v.end());
    return *mid;
}

inline float recentMedian (const std::vector<float>& v)
{
    const size_t n = std::min<size_t> (v.size(), (size_t) kMedianWindow);
    return medianOf (std::vector<float> (v.end() - (std::ptrdiff_t) n, v.end()));
}
} // namespace detail

// Median of the captured pitch inside [startSample, endSample); `fallback` if none.
inline float medianPitch (const PitchTrack& t, std::int64_t startSample, std::int64_t endSample, float fallback)
{
    std::vector<float> v;
    const std::int64_t hop = t.hop();
    for (std::int64_t f = startSample / hop; f * hop < endSample; ++f)
    {
        const float m = t.read (f);
        if (m >= 0.0f) v.push_back (m);
    }
    return v.empty() ? fallback : detail::medianOf (std::move (v));
}

// Cut the frames [lo, hi] into notes.
inline std::vector<Note> detectNotesInRange (const PitchTrack& track, std::int64_t lo, std::int64_t hi, int root, int mask12)
{
    std::vector<Note> out;
    const std::int64_t hop = track.hop();
    lo = std::max<std::int64_t> (lo, 0);
    hi = std::min<std::int64_t> (hi, track.capacity() - 1);

    auto emit = [&] (std::int64_t first, std::int64_t last, const std::vector<float>& vals)
    {
        if (last - first + 1 < kMinNoteFrames || vals.empty()) return;
        Note n;
        n.start    = first * hop;
        n.end      = (last + 1) * hop;
        n.detected = detail::medianOf (vals);
        n.target   = quantiseToScale (n.detected, root, mask12);
        n.correct  = true;
        out.push_back (n);
    };

    auto splitRun = [&] (std::int64_t first, std::int64_t last)
    {
        std::vector<float> cur;
        std::vector<std::pair<std::int64_t, float>> pending;
        std::int64_t curStart = first;

        for (std::int64_t f = first; f <= last; ++f)
        {
            const float v = track.read (f);
            if (v < 0.0f) continue;                       // bridged dropout

            if (cur.empty()) { cur.push_back (v); continue; }

            if (std::abs (v - detail::recentMedian (cur)) > kSplitSemis)
            {
                pending.emplace_back (f, v);
                if ((int) pending.size() >= kSplitFrames)
                {
                    float pmin = pending.front().second, pmax = pmin;
                    for (auto& p : pending) { pmin = std::min (pmin, p.second); pmax = std::max (pmax, p.second); }

                    if (pmax - pmin < 1.0f)               // settled on a new pitch -> new note
                    {
                        emit (curStart, pending.front().first - 1, cur);
                        curStart = pending.front().first;
                        cur.clear();
                        for (auto& p : pending) cur.push_back (p.second);
                        pending.clear();
                    }
                    else                                  // still gliding -> belongs to the current note
                    {
                        cur.push_back (pending.front().second);
                        pending.erase (pending.begin());
                    }
                }
            }
            else
            {
                for (auto& p : pending) cur.push_back (p.second);
                pending.clear();
                cur.push_back (v);
            }
        }
        for (auto& p : pending) cur.push_back (p.second);
        emit (curStart, last, cur);
    };

    std::int64_t f = lo;
    while (f <= hi)
    {
        if (track.read (f) < 0.0f) { ++f; continue; }

        std::int64_t runLast = f;
        int gap = 0;
        for (std::int64_t g = f + 1; g <= hi; ++g)
        {
            if (track.read (g) >= 0.0f) { runLast = g; gap = 0; }
            else if (++gap > kMaxGapFrames) break;
        }
        splitRun (f, runLast);
        f = runLast + 1;
    }
    return out;
}

// Re-detect only the part of the track that changed (frames lo..hi), keeping every
// note you edited elsewhere. Notes touching the range are replaced as a whole.
inline NoteList redetectRange (const NoteList& old, const PitchTrack& track, std::int64_t lo, std::int64_t hi, int root, int mask12)
{
    const std::int64_t hop = track.hop(), cap = track.capacity();
    auto voiced = [&] (std::int64_t f) { return track.read (f) >= 0.0f; };

    lo = std::max<std::int64_t> (lo, 0);
    hi = std::min<std::int64_t> (hi, cap - 1);

    // Grow the range until it covers (a) every old note it touches and (b) the voiced run
    // that continues past its edges -- but never into frames owned by a note we are not
    // replacing, so a legato neighbour keeps its edits.
    auto owned = [&] (std::int64_t f) { return old.indexAt (f * hop) >= 0; };

    for (int pass = 0; pass < 8; ++pass)
    {
        bool changed = false;

        for (const auto& n : old.notes)
        {
            if (n.end > lo * hop && n.start < (hi + 1) * hop)
            {
                const std::int64_t nl = n.start / hop, nh = (n.end - 1) / hop;
                if (nl < lo) { lo = nl; changed = true; }
                if (nh > hi) { hi = nh; changed = true; }
            }
        }
        while (lo > 0 && ! owned (lo - 1) && (voiced (lo - 1) || (lo > 1 && voiced (lo - 2)) || (lo > 2 && voiced (lo - 3)))) { --lo; changed = true; }
        while (hi + 3 < cap && ! owned (hi + 1) && (voiced (hi + 1) || voiced (hi + 2) || voiced (hi + 3))) { ++hi; changed = true; }

        if (! changed) break;
    }

    NoteList result;
    for (const auto& n : old.notes)
        if (! (n.end > lo * hop && n.start < (hi + 1) * hop))
            result.notes.push_back (n);

    for (const auto& n : detectNotesInRange (track, lo, hi, root, mask12))
        result.notes.push_back (n);

    std::sort (result.notes.begin(), result.notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    return result;
}

inline NoteList detectAll (const PitchTrack& track, int root, int mask12)
{
    NoteList result;
    if (track.maxFrame() >= 0)
        result.notes = detectNotesInRange (track, 0, track.maxFrame() + 3, root, mask12);
    return result;
}

//------------------------------------------------------------------------------
// Edit operations (all work on a copy of the list that the GUI then publishes)

inline void snapAllToScale (NoteList& list, int root, int mask12)
{
    for (auto& n : list.notes) n.target = quantiseToScale (n.detected, root, mask12);
}

// Split the note under `pos` into two notes. Both halves keep the same target.
inline bool splitNoteAt (NoteList& list, std::int64_t pos, const PitchTrack& track)
{
    const std::int64_t hop = track.hop();
    const int i = list.indexAt (pos);
    if (i < 0) return false;

    pos = (pos / hop) * hop;                                   // cut on a frame boundary
    Note a = list.notes[(size_t) i], b = a;
    if (pos - a.start < kMinNoteFrames * hop || a.end - pos < kMinNoteFrames * hop) return false;

    a.end = pos;
    b.start = pos;
    a.detected = medianPitch (track, a.start, a.end, a.detected);
    b.detected = medianPitch (track, b.start, b.end, b.detected);

    list.notes[(size_t) i] = a;
    list.notes.insert (list.notes.begin() + i + 1, b);
    return true;
}

// Join note i with the note after it (the gap between them is filled).
inline bool mergeWithNext (NoteList& list, int i, const PitchTrack& track)
{
    if (i < 0 || i + 1 >= (int) list.notes.size()) return false;

    const Note& a = list.notes[(size_t) i];
    const Note& b = list.notes[(size_t) i + 1];
    Note m = a;
    m.end      = b.end;
    m.detected = medianPitch (track, m.start, m.end, a.detected);
    m.target   = (b.end - b.start > a.end - a.start) ? b.target : a.target;
    m.correct  = a.correct || b.correct;

    list.notes[(size_t) i] = m;
    list.notes.erase (list.notes.begin() + i + 1);
    return true;
}

} // namespace atune
