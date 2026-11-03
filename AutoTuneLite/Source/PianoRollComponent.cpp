#include "PianoRollComponent.h"

namespace
{
constexpr float kKeyW   = 58.0f;
constexpr float kRulerH = 22.0f;

const juce::Colour kBackground  (0xff1b1d22);
const juce::Colour kRowWhite    (0xff25282e);
const juce::Colour kRowBlack    (0xff202328);
const juce::Colour kRowScale    (0xff2b3340);
const juce::Colour kGrid        (0xff15171b);
const juce::Colour kNoteOn      (0xff2fb5a3);
const juce::Colour kNoteSel     (0xffffa94d);
const juce::Colour kNoteOff     (0xff6b7079);
const juce::Colour kCurve       (0xff8fd6ff);
}

PianoRollComponent::PianoRollComponent (AutoTuneLiteProcessor& p) : processor (p)
{
    setWantsKeyboardFocus (true);
    startTimerHz (30);
}

PianoRollComponent::~PianoRollComponent()
{
    stopTimer();
}

//==============================================================================
// geometry

double PianoRollComponent::sampleRate() const
{
    const double sr = processor.getEditSampleRate();
    return sr > 0.0 ? sr : 44100.0;
}

juce::Rectangle<float> PianoRollComponent::rollArea() const
{
    return { kKeyW, kRulerH, juce::jmax (1.0f, (float) getWidth() - kKeyW), juce::jmax (1.0f, (float) getHeight() - kRulerH) };
}

float  PianoRollComponent::timeToX (double seconds) const { return kKeyW + (float) ((seconds - viewStart) * pps); }
double PianoRollComponent::xToTime (float x) const        { return viewStart + (double) (x - kKeyW) / pps; }
float  PianoRollComponent::pitchToY (float midi) const    { return rollArea().getBottom() - (midi - viewLow) * pxPerSemi; }
float  PianoRollComponent::yToPitch (float y) const       { return viewLow + (rollArea().getBottom() - y) / pxPerSemi; }

juce::Rectangle<float> PianoRollComponent::noteRect (const atune::Note& n) const
{
    const double sr = sampleRate();
    const float x1 = timeToX ((double) n.start / sr);
    const float x2 = timeToX ((double) n.end / sr);
    const float y1 = pitchToY (n.target + 0.4f);
    const float y2 = pitchToY (n.target - 0.4f);
    return { x1, y1, juce::jmax (3.0f, x2 - x1), y2 - y1 };
}

int PianoRollComponent::hitTest (const atune::NoteList& list, juce::Point<float> p) const
{
    for (int i = (int) list.notes.size() - 1; i >= 0; --i)
        if (noteRect (list.notes[(size_t) i]).expanded (0.0f, 2.0f).contains (p))
            return i;
    return -1;
}

float PianoRollComponent::snapPitch (float midi) const
{
    switch (snap)
    {
        case Snap::Semitone: return std::round (midi);
        case Snap::Scale:    return atune::quantiseToScale (midi, processor.currentKey(), processor.currentScaleMask());
        case Snap::Free:     break;
    }
    return midi;
}

void PianoRollComponent::zoomToFit()
{
    auto notes = processor.getNotes();
    if (notes->notes.empty()) return;

    const double sr = sampleRate();
    float lo = 127.0f, hi = 0.0f;
    std::int64_t t0 = notes->notes.front().start, t1 = notes->notes.back().end;
    for (const auto& n : notes->notes)
    {
        lo = std::min ({ lo, n.target, n.detected });
        hi = std::max ({ hi, n.target, n.detected });
        t0 = std::min (t0, n.start);
        t1 = std::max (t1, n.end);
    }
    const auto area = rollArea();
    pxPerSemi = juce::jlimit (8.0f, 40.0f, area.getHeight() / (hi - lo + 5.0f));
    viewLow   = juce::jmax (0.0f, lo - 2.0f);
    pps       = juce::jlimit (5.0, 2000.0, (double) area.getWidth() / juce::jmax (0.5, (double) (t1 - t0) / sr + 0.4));
    viewStart = juce::jmax (0.0, (double) t0 / sr - 0.2);
    repaint();
}

//==============================================================================
void PianoRollComponent::timerCallback()
{
    processor.refreshNotesFromCapture();      // auto-detect notes once a capture pass is over

    if (follow && dragMode == DragMode::none && processor.hostPlaying.load() && processor.hostHasTimeline.load())
    {
        const double t = (double) processor.hostPosition.load() / sampleRate();
        const double visible = (double) rollArea().getWidth() / pps;
        if (t < viewStart || t > viewStart + visible * 0.9)
            viewStart = juce::jmax (0.0, t - visible * 0.1);
    }
    repaint();
}

//==============================================================================
void PianoRollComponent::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    const auto area = rollArea();
    const double sr = sampleRate();
    auto track = processor.getTrack();
    auto notes = processor.getNotes();
    const int key = processor.currentKey(), mask = processor.currentScaleMask();

    const int lowRow  = (int) std::floor (viewLow) - 1;
    const int highRow = (int) std::ceil (yToPitch (area.getY())) + 1;

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (area.toNearestInt());

        // --- pitch rows (scale notes are tinted)
        for (int m = lowRow; m <= highRow; ++m)
        {
            const float yTop = pitchToY ((float) m + 0.5f), yBot = pitchToY ((float) m - 0.5f);
            const int degree = (((m - key) % 12) + 12) % 12;
            g.setColour (((mask >> degree) & 1) ? kRowScale : (atuneui::isBlackKey (m) ? kRowBlack : kRowWhite));
            g.fillRect (area.getX(), yTop, area.getWidth(), yBot - yTop);
            g.setColour (kGrid);
            g.drawHorizontalLine ((int) yBot, area.getX(), area.getRight());
        }

        // --- time grid
        static const double steps[] = { 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 };
        double step = 300.0;
        for (double s : steps) if (s * pps >= 70.0) { step = s; break; }
        for (double t = std::floor (viewStart / step) * step;; t += step)
        {
            const float x = timeToX (t);
            if (x > area.getRight()) break;
            if (x < area.getX()) continue;
            g.setColour (juce::Colour (0x30ffffff));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        }

        // --- notes: block at the TARGET pitch, faint outline where it was actually sung
        for (const auto& n : notes->notes)
        {
            const auto r = noteRect (n);
            if (r.getRight() < area.getX() || r.getX() > area.getRight()) continue;

            const bool sel = selected.count (n.start) != 0;
            const juce::Colour base = ! n.correct ? kNoteOff : (sel ? kNoteSel : kNoteOn);

            if (n.correct && std::abs (n.target - n.detected) > 0.05f)
            {
                auto ghost = r;
                ghost.setY (pitchToY (n.detected + 0.4f));
                g.setColour (base.withAlpha (0.45f));
                g.drawRect (ghost, 1.0f);
            }

            g.setColour (base.withAlpha (0.85f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (base.brighter (0.5f));
            g.drawRoundedRectangle (r, 3.0f, 1.0f);

            if (r.getWidth() > 36.0f && r.getHeight() > 9.0f)
            {
                g.setColour (juce::Colours::white);
                g.setFont (11.0f);
                g.drawText (atuneui::pitchLabel (n.target) + (n.correct ? "" : "  (off)"), r.reduced (4.0f, 0.0f).toNearestInt(),
                            juce::Justification::centredLeft, true);
            }
        }

        // --- captured pitch curve on top
        if (track != nullptr && track->maxFrame() >= 0)
        {
            const int hop = track->hop();
            const std::int64_t f0 = juce::jmax<std::int64_t> (0, (std::int64_t) std::floor (xToTime (area.getX()) * sr / hop) - 1);
            const std::int64_t f1 = juce::jmin<std::int64_t> (track->maxFrame(), (std::int64_t) std::ceil (xToTime (area.getRight()) * sr / hop) + 1);
            const double pxPerFrame = pps * hop / sr;
            const std::int64_t stride = juce::jmax<std::int64_t> (1, (std::int64_t) (1.0 / pxPerFrame));

            juce::Path curve;
            bool pen = false;
            for (std::int64_t f = f0; f <= f1; f += stride)
            {
                const float v = track->read (f);
                if (v < 0.0f) { pen = false; continue; }
                const float x = timeToX ((double) f * hop / sr), y = pitchToY (v);
                if (! pen) { curve.startNewSubPath (x, y); pen = true; }
                else         curve.lineTo (x, y);
            }
            g.setColour (kCurve.withAlpha (0.8f));
            g.strokePath (curve, juce::PathStrokeType (1.4f));
        }

        // --- rubber band
        if (dragMode == DragMode::rubberBand)
        {
            g.setColour (juce::Colour (0x40ffffff));
            g.fillRect (bandRect);
            g.setColour (juce::Colours::white.withAlpha (0.7f));
            g.drawRect (bandRect, 1.0f);
        }

        // --- playhead
        if (processor.hostHasTimeline.load())
        {
            const float x = timeToX ((double) processor.hostPosition.load() / sr);
            if (x >= area.getX() && x <= area.getRight())
            {
                g.setColour (juce::Colour (0xffff5d5d));
                g.drawLine (x, area.getY(), x, area.getBottom(), 1.5f);
            }
        }

        // --- hint when there is nothing yet
        if (notes->notes.empty() && (track == nullptr || track->maxFrame() < 0))
        {
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.setFont (14.0f);
            g.drawText ("Arm Capture, then press play in the host while the vocal is running (live or recorded). "
                        "Notes appear here when the pass ends.",
                        area.reduced (30.0f).toNearestInt(), juce::Justification::centred, true);
        }
    }

    // --- keyboard column
    for (int m = lowRow; m <= highRow; ++m)
    {
        const float yTop = pitchToY ((float) m + 0.5f), yBot = pitchToY ((float) m - 0.5f);
        const juce::Rectangle<float> r (0.0f, yTop, kKeyW, yBot - yTop);
        if (r.getBottom() < area.getY() || r.getY() > area.getBottom()) continue;

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (juce::Rectangle<int> (0, (int) area.getY(), (int) kKeyW, (int) area.getHeight()));

        g.setColour (atuneui::isBlackKey (m) ? juce::Colour (0xff2a2a2e) : juce::Colour (0xffe6e6e6));
        g.fillRect (r.withTrimmedBottom (1.0f));
        if (r.getHeight() >= 11.0f && (m % 12 == 0 || r.getHeight() >= 15.0f))
        {
            g.setColour (atuneui::isBlackKey (m) ? juce::Colours::white.withAlpha (0.7f) : juce::Colours::black.withAlpha (0.75f));
            g.setFont (10.0f);
            g.drawText (juce::String (atuneui::pitchClassName (m)) + juce::String (m / 12 - 1), r.toNearestInt().reduced (4, 0),
                        juce::Justification::centredLeft, false);
        }
    }

    // live pitch marker on the keyboard
    const float live = processor.liveDetectedMidi();
    if (live >= 0.0f)
    {
        const float y = pitchToY (live);
        if (y > area.getY() && y < area.getBottom())
        {
            g.setColour (juce::Colour (0xffff5d5d));
            g.fillEllipse (kKeyW - 11.0f, y - 4.5f, 9.0f, 9.0f);
        }
    }

    // --- ruler
    g.setColour (juce::Colour (0xff2b2e35));
    g.fillRect (0.0f, 0.0f, (float) getWidth(), kRulerH);
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (juce::Rectangle<int> ((int) kKeyW, 0, getWidth() - (int) kKeyW, (int) kRulerH));
        static const double steps[] = { 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 };
        double step = 300.0;
        for (double s : steps) if (s * pps >= 70.0) { step = s; break; }
        g.setFont (11.0f);
        for (double t = std::floor (viewStart / step) * step;; t += step)
        {
            const float x = timeToX (t);
            if (x > (float) getWidth()) break;
            if (x < kKeyW - 60.0f) continue;
            const int minutes = (int) (t / 60.0);
            const double secs = t - minutes * 60.0;
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawVerticalLine ((int) x, kRulerH - 6.0f, kRulerH);
            g.setColour (juce::Colours::white.withAlpha (0.75f));
            g.drawText (juce::String (minutes) + ":" + (secs < 10.0 ? "0" : "") + juce::String (secs, step < 1.0 ? 2 : 0),
                        (int) x + 3, 0, 60, (int) kRulerH, juce::Justification::centredLeft, false);
        }
    }
}

//==============================================================================
void PianoRollComponent::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragMode = DragMode::none;
    if (! rollArea().contains (e.position)) return;

    auto notes = processor.getNotes();
    const int hit = hitTest (*notes, e.position);

    if (e.mods.isPopupMenu())
    {
        if (hit >= 0)
        {
            if (selected.count (notes->notes[(size_t) hit].start) == 0)
            {
                selected.clear();
                selected.insert (notes->notes[(size_t) hit].start);
            }
            showContextMenu (e.position);
        }
        return;
    }

    const bool additive = e.mods.isShiftDown() || e.mods.isCommandDown() || e.mods.isCtrlDown();

    if (hit >= 0)
    {
        const auto start = notes->notes[(size_t) hit].start;
        if (additive)
        {
            if (selected.count (start) != 0) selected.erase (start);
            else                             selected.insert (start);
        }
        else if (selected.count (start) == 0)
        {
            selected.clear();
            selected.insert (start);
        }
        dragMode    = DragMode::moveNotes;
        dragStartY  = e.position.y;
        dragBase    = notes;
        didPushUndo = false;
    }
    else
    {
        if (! additive) selected.clear();
        dragMode  = DragMode::rubberBand;
        bandStart = e.position;
        bandRect  = {};
    }
    repaint();
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (dragMode == DragMode::moveNotes && dragBase != nullptr)
    {
        if (! didPushUndo && std::abs (e.position.y - dragStartY) < 3.0f) return;    // a click, not a drag
        if (! didPushUndo)
        {
            processor.pushUndoSnapshot();        // the whole drag is one undo step
            didPushUndo = true;
        }

        const float semis = (dragStartY - e.position.y) / pxPerSemi;
        auto list = std::make_shared<atune::NoteList> (*dragBase);
        for (auto& n : list->notes)
            if (selected.count (n.start) != 0)
                n.target = juce::jlimit (0.0f, 127.0f, snapPitch (n.target + semis));

        processor.commitNotes (list, false);     // published immediately, so you hear it while dragging
        repaint();
    }
    else if (dragMode == DragMode::rubberBand)
    {
        bandRect = juce::Rectangle<float> (bandStart, e.position);
        repaint();
    }
}

void PianoRollComponent::mouseUp (const juce::MouseEvent&)
{
    if (dragMode == DragMode::rubberBand)
    {
        auto notes = processor.getNotes();
        for (const auto& n : notes->notes)
            if (noteRect (n).intersects (bandRect))
                selected.insert (n.start);
    }
    dragMode = DragMode::none;
    dragBase.reset();
    bandRect = {};
    repaint();
}

void PianoRollComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! rollArea().contains (e.position)) return;
    auto notes = processor.getNotes();
    const int hit = hitTest (*notes, e.position);
    if (hit < 0) return;

    auto list = std::make_shared<atune::NoteList> (*notes);
    auto& n = list->notes[(size_t) hit];
    n.correct = ! n.correct;
    processor.commitNotes (list, true);
    repaint();
}

void PianoRollComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const float dy = w.deltaY != 0.0f ? w.deltaY : w.deltaX;

    if (e.mods.isCommandDown() || e.mods.isCtrlDown())           // zoom time around the cursor
    {
        const double tAt = xToTime (e.position.x);
        pps = juce::jlimit (5.0, 3000.0, pps * std::exp ((double) dy * 1.5));
        viewStart = tAt - (double) (e.position.x - kKeyW) / pps;
    }
    else if (e.mods.isAltDown())                                 // zoom pitch
    {
        pxPerSemi = juce::jlimit (6.0f, 48.0f, pxPerSemi * std::exp (dy * 1.0f));
    }
    else if (e.mods.isShiftDown())                               // scroll pitch
    {
        viewLow = juce::jlimit (0.0f, 110.0f, viewLow + dy * 8.0f);
    }
    else                                                         // scroll time
    {
        viewStart -= (double) dy * 250.0 / pps;
    }
    viewStart = juce::jmax (0.0, viewStart);
    repaint();
}

//==============================================================================
void PianoRollComponent::editSelected (const std::function<void (atune::Note&)>& fn)
{
    if (selected.empty()) return;
    auto list = std::make_shared<atune::NoteList> (*processor.getNotes());
    bool changed = false;
    for (auto& n : list->notes)
        if (selected.count (n.start) != 0) { fn (n); changed = true; }
    if (changed) processor.commitNotes (list, true);
}

bool PianoRollComponent::keyPressed (const juce::KeyPress& key)
{
    const int code = key.getKeyCode();
    const auto mods = key.getModifiers();

    if (mods.isCommandDown() && (code == 'Z' || code == 'z'))
    {
        if (mods.isShiftDown()) processor.redo(); else processor.undo();
        repaint();
        return true;
    }
    if (mods.isCommandDown() && (code == 'Y' || code == 'y')) { processor.redo(); repaint(); return true; }

    if (mods.isCommandDown() && (code == 'A' || code == 'a'))
    {
        selected.clear();
        for (const auto& n : processor.getNotes()->notes) selected.insert (n.start);
        repaint();
        return true;
    }

    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        const float delta = (code == juce::KeyPress::upKey ? 1.0f : -1.0f) * (mods.isShiftDown() ? 0.1f : 1.0f);
        editSelected ([delta] (atune::Note& n) { n.target = juce::jlimit (0.0f, 127.0f, n.target + delta); });
        repaint();
        return true;
    }

    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        editSelected ([] (atune::Note& n) { n.correct = ! n.correct; });
        repaint();
        return true;
    }
    return false;
}

//==============================================================================
void PianoRollComponent::showContextMenu (juce::Point<float> pos)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Snap target to scale");
    menu.addItem (2, "Reset to automatic target");
    menu.addItem (3, "Correction on / off");
    menu.addSeparator();
    menu.addItem (4, "Split here");
    menu.addItem (5, "Merge with next note");

    const auto posSamples = (std::int64_t) std::llround (xToTime (pos.x) * sampleRate());
    juce::Component::SafePointer<PianoRollComponent> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options(), [safeThis, posSamples] (int result)
    {
        if (safeThis != nullptr && result != 0)
            safeThis->applyMenuAction (result, posSamples);
    });
}

void PianoRollComponent::applyMenuAction (int id, std::int64_t posSamples)
{
    auto list = std::make_shared<atune::NoteList> (*processor.getNotes());
    auto track = processor.getTrack();
    const int key = processor.currentKey(), mask = processor.currentScaleMask();
    bool changed = false;

    switch (id)
    {
        case 1:
            for (auto& n : list->notes)
                if (selected.count (n.start) != 0) { n.target = atune::quantiseToScale (n.target, key, mask); changed = true; }
            break;

        case 2:
            for (auto& n : list->notes)
                if (selected.count (n.start) != 0) { n.target = atune::quantiseToScale (n.detected, key, mask); n.correct = true; changed = true; }
            break;

        case 3:
            for (auto& n : list->notes)
                if (selected.count (n.start) != 0) { n.correct = ! n.correct; changed = true; }
            break;

        case 4:
            if (track != nullptr) changed = atune::splitNoteAt (*list, posSamples, *track);
            break;

        case 5:
            if (track != nullptr)
                for (int i = 0; i < (int) list->notes.size(); ++i)
                    if (selected.count (list->notes[(size_t) i].start) != 0)
                    {
                        changed = atune::mergeWithNext (*list, i, *track);
                        break;
                    }
            break;

        default:
            break;
    }

    if (changed) processor.commitNotes (list, true);
    repaint();
}
