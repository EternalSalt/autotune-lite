#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>
#include "AutoTuneCore.h"

class AutoTuneLiteProcessor : public juce::AudioProcessor
{
public:
    AutoTuneLiteProcessor();
    ~AutoTuneLiteProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "AutoTune Lite"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    //==========================================================================
    // Piano-roll editor API. Everything below is for the message (GUI) thread.

    std::shared_ptr<const atune::NoteList> getNotes() const;
    std::shared_ptr<atune::PitchTrack>     getTrack() const;

    // Publish a new note list to the audio thread. pushUndo=true makes it one undo step.
    void commitNotes (std::shared_ptr<const atune::NoteList> list, bool pushUndo);
    void pushUndoSnapshot();            // call once before a drag that publishes many intermediate lists
    bool undo();
    bool redo();

    void detectAllNotes();              // rebuild every note from the captured pitch (discards edits)
    bool refreshNotesFromCapture();     // detect only what was just captured, keep other edits
    void snapAllNotesToScale();

    int    currentKey() const;
    int    currentScaleMask() const;
    double getEditSampleRate() const { return editSampleRate.load(); }

    float liveDetectedMidi() const { return engine.detectedMidi(); }
    float liveTargetMidi() const   { return engine.targetMidi(); }

    std::atomic<bool>         captureArmed    { false };   // deliberately NOT saved with the project
    std::atomic<std::int64_t> hostPosition    { 0 };       // timeline sample at the start of the last block
    std::atomic<bool>         hostPlaying     { false };
    std::atomic<bool>         hostHasTimeline { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void applyPendingEditState();

    atune::Engine engine;
    std::atomic<float>* keyParam       = nullptr;
    std::atomic<float>* scaleParam     = nullptr;
    std::atomic<float>* retuneParam    = nullptr;
    std::atomic<float>* amountParam    = nullptr;
    std::atomic<float>* transposeParam = nullptr;
    std::atomic<float>* modeParam      = nullptr;
    std::atomic<float>* driftParam     = nullptr;

    // --- shared between GUI and audio thread ---
    mutable juce::SpinLock editLock;                                   // held only for pointer copies / swaps
    std::shared_ptr<const atune::NoteList> published = std::make_shared<const atune::NoteList>();
    std::shared_ptr<const atune::NoteList> audioNotes = published;     // audio thread's own reference
    std::shared_ptr<atune::PitchTrack>     track;                       // only replaced in prepareToPlay
    std::vector<std::shared_ptr<const atune::NoteList>> undoStack, redoStack, retired;  // keep old lists alive off the audio thread

    // --- edit data bookkeeping ---
    juce::CriticalSection stateLock;                  // prepareToPlay vs setStateInformation
    std::atomic<double>   editSampleRate { 0.0 };     // sample rate the stored notes/frames refer to
    double                currentSampleRate = 0.0;

    struct SavedEdit
    {
        bool valid = false;
        double sr = 0.0;
        int hop = 0;
        std::int64_t startFrame = 0;
        std::vector<std::int16_t> cents;              // pitch * 100, -1 = unvoiced
        std::vector<atune::Note> notes;
    } pendingEdit;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoTuneLiteProcessor)
};
