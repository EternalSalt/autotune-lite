#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <memory>
#include <set>
#include "PluginProcessor.h"

namespace atuneui
{
inline const char* pitchClassName (int midi)
{
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((midi % 12) + 12) % 12];
}

inline bool isBlackKey (int midi)
{
    switch (((midi % 12) + 12) % 12)
    {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}

// "A3", or "A3 +12c" when the pitch is not exactly on a semitone
inline juce::String pitchLabel (float midi)
{
    const int note  = (int) std::lround (midi);
    const int cents = (int) std::lround ((midi - (float) note) * 100.0f);
    juce::String s = juce::String (pitchClassName (note)) + juce::String (note / 12 - 1);
    if (cents > 0) s += " +" + juce::String (cents) + "c";
    if (cents < 0) s += " " + juce::String (cents) + "c";
    return s;
}
}

//==============================================================================
// The piano roll: shows the captured pitch curve and the detected notes, lets you drag notes to a
// new pitch, split / merge them, switch correction off per note, and see the playhead.
class PianoRollComponent : public juce::Component,
                           private juce::Timer
{
public:
    enum class Snap { Semitone = 0, Scale = 1, Free = 2 };

    explicit PianoRollComponent (AutoTuneLiteProcessor&);
    ~PianoRollComponent() override;

    void setSnapMode (Snap s)        { snap = s; }
    void setFollowPlayhead (bool f)  { follow = f; }
    void zoomToFit();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class DragMode { none, moveNotes, rubberBand };

    void timerCallback() override;

    double sampleRate() const;
    juce::Rectangle<float> rollArea() const;
    float  timeToX (double seconds) const;
    double xToTime (float x) const;
    float  pitchToY (float midi) const;
    float  yToPitch (float y) const;
    juce::Rectangle<float> noteRect (const atune::Note&) const;
    int    hitTest (const atune::NoteList&, juce::Point<float>) const;
    float  snapPitch (float midi) const;

    void showContextMenu (juce::Point<float> pos);
    void applyMenuAction (int id, std::int64_t posSamples);
    void editSelected (const std::function<void (atune::Note&)>& fn);

    AutoTuneLiteProcessor& processor;

    Snap snap = Snap::Semitone;
    bool follow = true;

    double viewStart = 0.0;        // seconds at the left edge
    double pps = 120.0;            // pixels per second
    float  viewLow = 46.0f;        // MIDI pitch at the bottom edge
    float  pxPerSemi = 16.0f;

    std::set<std::int64_t> selected;   // notes are identified by their start sample

    DragMode dragMode = DragMode::none;
    float dragStartY = 0.0f;
    std::shared_ptr<const atune::NoteList> dragBase;
    bool didPushUndo = false;
    juce::Point<float> bandStart;
    juce::Rectangle<float> bandRect;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollComponent)
};
