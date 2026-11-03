#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "PianoRollComponent.h"

class AutoTuneLiteEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit AutoTuneLiteEditor (AutoTuneLiteProcessor&);
    ~AutoTuneLiteEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    AutoTuneLiteProcessor& processor;
    PianoRollComponent roll;

    juce::ComboBox modeBox, keyBox, scaleBox, snapBox;
    juce::Slider retuneSlider, amountSlider, driftSlider, transposeSlider;
    juce::Label modeLabel, keyLabel, scaleLabel, retuneLabel, amountLabel, driftLabel, transposeLabel, statusLabel;
    juce::TextButton captureButton { "Capture" }, detectButton { "Detect Notes" }, snapAllButton { "Snap All To Scale" },
                     undoButton { "Undo" }, redoButton { "Redo" }, fitButton { "Fit" };
    juce::ToggleButton followButton { "Follow playhead" };

    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<ComboAtt>  modeAtt, keyAtt, scaleAtt;       // declared last: destroyed before the controls
    std::unique_ptr<SliderAtt> retuneAtt, amountAtt, driftAtt, transposeAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoTuneLiteEditor)
};
