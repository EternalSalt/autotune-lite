#include "PluginEditor.h"

namespace
{
void fillCombo (juce::ComboBox& box, AutoTuneLiteProcessor& p, const juce::String& paramId)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter (paramId)))
        box.addItemList (choice->choices, 1);
}
}

AutoTuneLiteEditor::AutoTuneLiteEditor (AutoTuneLiteProcessor& p)
    : juce::AudioProcessorEditor (&p), processor (p), roll (p)
{
    auto label = [this] (juce::Label& l, const juce::String& text)
    {
        addAndMakeVisible (l);
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centredRight);
    };
    auto slider = [this] (juce::Slider& s, const juce::String& suffix)
    {
        addAndMakeVisible (s);
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
        s.setTextValueSuffix (suffix);
    };

    // --- row 1: mode / key / scale / snap / status
    label (modeLabel, "Mode");   addAndMakeVisible (modeBox);   fillCombo (modeBox, p, "mode");
    label (keyLabel, "Key");     addAndMakeVisible (keyBox);    fillCombo (keyBox, p, "key");
    label (scaleLabel, "Scale"); addAndMakeVisible (scaleBox);  fillCombo (scaleBox, p, "scale");

    addAndMakeVisible (snapBox);
    snapBox.addItemList ({ "Drag snaps: Semitone", "Drag snaps: Scale", "Drag snaps: Free" }, 1);
    snapBox.setSelectedId (1);
    snapBox.onChange = [this]
    {
        roll.setSnapMode (static_cast<PianoRollComponent::Snap> (juce::jmax (0, snapBox.getSelectedId() - 1)));
    };

    addAndMakeVisible (statusLabel);
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffbfe9ff));

    // --- row 2: buttons
    addAndMakeVisible (captureButton);
    captureButton.setClickingTogglesState (true);
    captureButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd23a3a));
    captureButton.onClick = [this] { processor.captureArmed = captureButton.getToggleState(); };

    addAndMakeVisible (detectButton);
    detectButton.onClick = [this] { processor.detectAllNotes(); roll.repaint(); };

    addAndMakeVisible (snapAllButton);
    snapAllButton.onClick = [this] { processor.snapAllNotesToScale(); roll.repaint(); };

    addAndMakeVisible (undoButton);
    undoButton.onClick = [this] { processor.undo(); roll.repaint(); };

    addAndMakeVisible (redoButton);
    redoButton.onClick = [this] { processor.redo(); roll.repaint(); };

    addAndMakeVisible (fitButton);
    fitButton.onClick = [this] { roll.zoomToFit(); };

    addAndMakeVisible (followButton);
    followButton.setToggleState (true, juce::dontSendNotification);
    followButton.onClick = [this] { roll.setFollowPlayhead (followButton.getToggleState()); };

    // --- row 3: sliders
    label (retuneLabel, "Retune");       slider (retuneSlider, " ms");
    label (amountLabel, "Amount");       slider (amountSlider, " %");
    label (driftLabel, "Drift corr.");   slider (driftSlider, " %");
    label (transposeLabel, "Transpose"); slider (transposeSlider, " st");

    modeAtt      = std::make_unique<ComboAtt>  (p.apvts, "mode", modeBox);
    keyAtt       = std::make_unique<ComboAtt>  (p.apvts, "key", keyBox);
    scaleAtt     = std::make_unique<ComboAtt>  (p.apvts, "scale", scaleBox);
    retuneAtt    = std::make_unique<SliderAtt> (p.apvts, "retune", retuneSlider);
    amountAtt    = std::make_unique<SliderAtt> (p.apvts, "amount", amountSlider);
    driftAtt     = std::make_unique<SliderAtt> (p.apvts, "drift", driftSlider);
    transposeAtt = std::make_unique<SliderAtt> (p.apvts, "transpose", transposeSlider);

    addAndMakeVisible (roll);

    setResizable (true, true);
    setResizeLimits (820, 480, 2600, 1500);
    setSize (1100, 660);
    startTimerHz (10);
}

AutoTuneLiteEditor::~AutoTuneLiteEditor()
{
    stopTimer();
}

void AutoTuneLiteEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff2b2e35));
}

void AutoTuneLiteEditor::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto row1 = r.removeFromTop (26);  r.removeFromTop (4);
    auto row2 = r.removeFromTop (26);  r.removeFromTop (4);
    auto row3 = r.removeFromTop (26);  r.removeFromTop (6);

    auto place = [] (juce::Rectangle<int>& row, juce::Component& c, int w)
    {
        c.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (6);
    };

    place (row1, modeLabel, 40);   place (row1, modeBox, 80);
    place (row1, keyLabel, 36);    place (row1, keyBox, 64);
    place (row1, scaleLabel, 46);  place (row1, scaleBox, 140);
    place (row1, snapBox, 180);
    statusLabel.setBounds (row1);

    place (row2, captureButton, 90);
    place (row2, detectButton, 110);
    place (row2, snapAllButton, 140);
    place (row2, undoButton, 60);
    place (row2, redoButton, 60);
    place (row2, fitButton, 50);
    place (row2, followButton, 140);

    const int col = row3.getWidth() / 4;
    auto pair = [&] (juce::Label& l, juce::Slider& s)
    {
        auto cell = row3.removeFromLeft (col);
        l.setBounds (cell.removeFromLeft (84));
        s.setBounds (cell);
    };
    pair (retuneLabel, retuneSlider);
    pair (amountLabel, amountSlider);
    pair (driftLabel, driftSlider);
    pair (transposeLabel, transposeSlider);

    roll.setBounds (r);
}

void AutoTuneLiteEditor::timerCallback()
{
    const float det = processor.liveDetectedMidi(), tgt = processor.liveTargetMidi();
    juce::String text = det < 0.0f ? juce::String ("Pitch: --")
                                   : "Pitch: " + atuneui::pitchLabel (det) + "   ->   " + atuneui::pitchLabel (tgt);

    const bool editMode = modeBox.getSelectedId() == 2;
    const bool capture  = processor.captureArmed.load();
    if (editMode && ! processor.hostPlaying.load())
        text += "      |  transport stopped: live tuning is active";
    else if ((editMode || capture) && ! processor.hostHasTimeline.load())
        text += "      |  host gives no timeline: capture / edit unavailable";
    else if (capture && editMode)
        text += "      |  capture paused while in Edit mode (switch to Live to re-capture)";
    else if (capture)
        text += processor.hostPlaying.load() ? "      |  capturing..." : "      |  capture armed: press play";
    statusLabel.setText (text, juce::dontSendNotification);

    captureButton.setToggleState (capture, juce::dontSendNotification);
}
