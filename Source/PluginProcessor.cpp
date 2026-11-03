#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cstring>

namespace
{
const juce::StringArray kKeyNames   { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
// Order must match atune::scaleMaskForIndex()
const juce::StringArray kScaleNames { "Chromatic", "Major", "Natural Minor", "Harmonic Minor",
                                      "Major Pentatonic", "Minor Pentatonic", "Blues", "Dorian" };
const juce::StringArray kModeNames  { "Live", "Edit" };

constexpr double kMaxCaptureSeconds = 1200.0;   // 20 minutes of pitch data (~1 MB)
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout AutoTuneLiteProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "mode", 1 }, "Mode", kModeNames, 0));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "key", 1 }, "Key", kKeyNames, 0));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "scale", 1 }, "Scale", kScaleNames, 0));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "retune", 1 }, "Retune Speed",
        juce::NormalisableRange<float> (0.0f, 400.0f, 0.1f, 0.4f), 20.0f,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "amount", 1 }, "Correction Amount",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "drift", 1 }, "Drift Correction",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "transpose", 1 }, "Transpose",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("st")));

    return { params.begin(), params.end() };
}

AutoTuneLiteProcessor::AutoTuneLiteProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    keyParam       = apvts.getRawParameterValue ("key");
    scaleParam     = apvts.getRawParameterValue ("scale");
    retuneParam    = apvts.getRawParameterValue ("retune");
    amountParam    = apvts.getRawParameterValue ("amount");
    transposeParam = apvts.getRawParameterValue ("transpose");
    modeParam      = apvts.getRawParameterValue ("mode");
    driftParam     = apvts.getRawParameterValue ("drift");
}

bool AutoTuneLiteProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

//==============================================================================
void AutoTuneLiteProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, juce::jmax (1, getTotalNumOutputChannels()));
    setLatencySamples (engine.latencySamples());   // lets FL Studio compensate on recorded tracks

    const juce::ScopedLock sl (stateLock);
    currentSampleRate = sampleRate;

    const int hop = engine.hopSamples();
    const double oldSr = editSampleRate.load();
    auto old = getTrack();

    if (old == nullptr || old->hop() != hop || (oldSr > 0.0 && oldSr != sampleRate))
    {
        auto fresh = std::make_shared<atune::PitchTrack>();
        fresh->prepare (hop, (std::int64_t) std::ceil (kMaxCaptureSeconds * sampleRate / hop));
        if (old != nullptr && oldSr > 0.0) fresh->rescaleFrom (*old, oldSr, sampleRate);

        {
            const juce::SpinLock::ScopedLockType lock (editLock);
            track = fresh;
        }

        if (oldSr > 0.0 && oldSr != sampleRate)          // project sample rate changed: re-time the notes
        {
            auto scaled = std::make_shared<atune::NoteList> (*getNotes());
            const double ratio = sampleRate / oldSr;
            for (auto& n : scaled->notes)
            {
                n.start = (std::int64_t) std::llround ((double) n.start * ratio);
                n.end   = (std::int64_t) std::llround ((double) n.end * ratio);
            }
            commitNotes (scaled, false);
        }
    }

    editSampleRate = sampleRate;
    applyPendingEditState();
}

void AutoTuneLiteProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // Pick up the newest note list if the GUI isn't swapping it right now
    {
        const juce::SpinLock::ScopedTryLockType lock (editLock);
        if (lock.isLocked() && audioNotes != published)
            audioNotes = published;
    }

    // Where is the host on its timeline?
    bool playing = false, haveTimeline = false;
    std::int64_t timeline = 0;
    if (auto* ph = getPlayHead())
    {
        const auto pos = ph->getPosition();
        if (pos)
        {
            const auto& info = *pos;
            playing = info.getIsPlaying();
            const auto ts = info.getTimeInSamples();
            const auto secs = info.getTimeInSeconds();
            if (ts)
            {
                timeline = *ts;
                haveTimeline = true;
            }
            else if (secs)
            {
                timeline = (std::int64_t) std::llround (*secs * getSampleRate());
                haveTimeline = true;
            }
        }
    }
    hostPosition    = timeline;
    hostPlaying     = playing;
    hostHasTimeline = haveTimeline;

    atune::Params p;
    p.rootNote        = (int) keyParam->load();
    p.scaleMask       = atune::scaleMaskForIndex ((int) scaleParam->load());
    p.retuneMs        = retuneParam->load();
    p.amount          = amountParam->load() * 0.01f;
    p.transposeSemis  = transposeParam->load();
    p.driftCorrection = driftParam->load() * 0.01f;
    engine.setParams (p);

    atune::EditContext ctx;
    ctx.editMode             = modeParam->load() > 0.5f;
    ctx.capture              = captureArmed.load() && ! ctx.editMode;   // never re-capture (and so overwrite your edits) while editing
    ctx.timelineValid        = haveTimeline && playing;
    ctx.timelineAtBlockStart = timeline;
    ctx.notes                = audioNotes.get();
    ctx.track                = track.get();
    engine.setEditContext (ctx);

    engine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

//==============================================================================
// Editor API

std::shared_ptr<const atune::NoteList> AutoTuneLiteProcessor::getNotes() const
{
    const juce::SpinLock::ScopedLockType lock (editLock);
    return published;
}

std::shared_ptr<atune::PitchTrack> AutoTuneLiteProcessor::getTrack() const
{
    const juce::SpinLock::ScopedLockType lock (editLock);
    return track;
}

void AutoTuneLiteProcessor::commitNotes (std::shared_ptr<const atune::NoteList> list, bool pushUndo)
{
    std::vector<std::shared_ptr<const atune::NoteList>> trash;     // destroyed AFTER the lock is released
    {
        const juce::SpinLock::ScopedLockType lock (editLock);
        if (pushUndo)
        {
            undoStack.push_back (published);
            if (undoStack.size() > 64) { trash.push_back (undoStack.front()); undoStack.erase (undoStack.begin()); }
            for (auto& r : redoStack) trash.push_back (r);
            redoStack.clear();
        }
        retired.push_back (published);                              // the audio thread may still hold it for a moment
        if (retired.size() > 16) { trash.push_back (retired.front()); retired.erase (retired.begin()); }
        published = std::move (list);
    }
}

void AutoTuneLiteProcessor::pushUndoSnapshot()
{
    std::vector<std::shared_ptr<const atune::NoteList>> trash;
    const juce::SpinLock::ScopedLockType lock (editLock);
    undoStack.push_back (published);
    if (undoStack.size() > 64) { trash.push_back (undoStack.front()); undoStack.erase (undoStack.begin()); }
    for (auto& r : redoStack) trash.push_back (r);
    redoStack.clear();
}

bool AutoTuneLiteProcessor::undo()
{
    const juce::SpinLock::ScopedLockType lock (editLock);
    if (undoStack.empty()) return false;
    redoStack.push_back (published);
    published = undoStack.back();
    undoStack.pop_back();
    return true;
}

bool AutoTuneLiteProcessor::redo()
{
    const juce::SpinLock::ScopedLockType lock (editLock);
    if (redoStack.empty()) return false;
    undoStack.push_back (published);
    published = redoStack.back();
    redoStack.pop_back();
    return true;
}

int AutoTuneLiteProcessor::currentKey() const       { return (int) keyParam->load(); }
int AutoTuneLiteProcessor::currentScaleMask() const { return atune::scaleMaskForIndex ((int) scaleParam->load()); }

void AutoTuneLiteProcessor::detectAllNotes()
{
    auto tr = getTrack();
    if (tr == nullptr) return;
    auto list = std::make_shared<atune::NoteList> (atune::detectAll (*tr, currentKey(), currentScaleMask()));
    std::int64_t lo, hi;
    tr->takeDirtyRange (lo, hi);                      // everything is up to date now
    commitNotes (list, true);
}

bool AutoTuneLiteProcessor::refreshNotesFromCapture()
{
    if (captureArmed.load() && hostPlaying.load() && modeParam->load() < 0.5f) return false;   // wait until the capture pass is over

    auto tr = getTrack();
    std::int64_t lo = 0, hi = -1;
    if (tr == nullptr || ! tr->takeDirtyRange (lo, hi)) return false;

    auto list = std::make_shared<atune::NoteList> (atune::redetectRange (*getNotes(), *tr, lo, hi, currentKey(), currentScaleMask()));
    commitNotes (list, true);
    return true;
}

void AutoTuneLiteProcessor::snapAllNotesToScale()
{
    auto list = std::make_shared<atune::NoteList> (*getNotes());
    atune::snapAllToScale (*list, currentKey(), currentScaleMask());
    commitNotes (list, true);
}

//==============================================================================
// State: the normal parameters plus the captured pitch and the notes, so edits survive saving the FL project.

void AutoTuneLiteProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    std::unique_ptr<juce::XmlElement> xml (apvts.copyState().createXml());
    if (xml == nullptr) return;

    auto tr = getTrack();
    auto notes = getNotes();
    const juce::ScopedLock sl (stateLock);

    if (tr != nullptr && editSampleRate.load() > 0.0 && (tr->maxFrame() >= 0 || ! notes->notes.empty()))
    {
        auto* edit = xml->createNewChildElement ("PITCHEDIT");
        edit->setAttribute ("sampleRate", editSampleRate.load());
        edit->setAttribute ("hop", tr->hop());

        for (const auto& n : notes->notes)
        {
            auto* e = edit->createNewChildElement ("NOTE");
            e->setAttribute ("s", juce::String (n.start));
            e->setAttribute ("e", juce::String (n.end));
            e->setAttribute ("d", (double) n.detected);
            e->setAttribute ("t", (double) n.target);
            e->setAttribute ("c", n.correct ? 1 : 0);
        }

        const std::int64_t last = tr->maxFrame();
        if (last >= 0)
        {
            std::int64_t first = 0;
            while (first < last && tr->read (first) < 0.0f) ++first;

            std::vector<std::int16_t> cents ((size_t) (last - first + 1));
            for (std::int64_t f = first; f <= last; ++f)
            {
                const float m = tr->read (f);
                cents[(size_t) (f - first)] = m < 0.0f ? (std::int16_t) -1 : (std::int16_t) juce::roundToInt (m * 100.0f);
            }
            juce::MemoryBlock mb (cents.data(), cents.size() * sizeof (std::int16_t));
            auto* fr = edit->createNewChildElement ("FRAMES");
            fr->setAttribute ("start", juce::String (first));
            fr->setAttribute ("data", mb.toBase64Encoding());
        }
    }
    copyXmlToBinary (*xml, destData);
}

void AutoTuneLiteProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr) return;

    {
        const juce::ScopedLock sl (stateLock);
        pendingEdit = {};
        if (auto* edit = xml->getChildByName ("PITCHEDIT"))
        {
            pendingEdit.valid = true;
            pendingEdit.sr    = edit->getDoubleAttribute ("sampleRate", 0.0);
            pendingEdit.hop   = edit->getIntAttribute ("hop", 0);

            for (auto* e : edit->getChildWithTagNameIterator ("NOTE"))
            {
                atune::Note n;
                n.start    = e->getStringAttribute ("s").getLargeIntValue();
                n.end      = e->getStringAttribute ("e").getLargeIntValue();
                n.detected = (float) e->getDoubleAttribute ("d");
                n.target   = (float) e->getDoubleAttribute ("t");
                n.correct  = e->getIntAttribute ("c", 1) != 0;
                pendingEdit.notes.push_back (n);
            }

            if (auto* fr = edit->getChildByName ("FRAMES"))
            {
                pendingEdit.startFrame = fr->getStringAttribute ("start").getLargeIntValue();
                juce::MemoryBlock mb;
                if (mb.fromBase64Encoding (fr->getStringAttribute ("data")))
                {
                    pendingEdit.cents.resize (mb.getSize() / sizeof (std::int16_t));
                    std::memcpy (pendingEdit.cents.data(), mb.getData(), pendingEdit.cents.size() * sizeof (std::int16_t));
                }
            }
            if (pendingEdit.sr <= 0.0 || pendingEdit.hop <= 0) pendingEdit = {};
            xml->removeChildElement (edit, true);     // keep it out of the parameter tree
        }
    }

    if (xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));

    applyPendingEditState();     // does nothing until prepareToPlay has told us the sample rate
}

// Turn the loaded edit data into live data, converting to the current sample rate if needed.
void AutoTuneLiteProcessor::applyPendingEditState()
{
    const juce::ScopedLock sl (stateLock);
    auto tr = getTrack();
    if (! pendingEdit.valid || currentSampleRate <= 0.0 || tr == nullptr) return;

    const double sr = currentSampleRate;
    const int    hop = tr->hop();

    tr->clear();
    if (! pendingEdit.cents.empty())
    {
        const double secLo = (double) pendingEdit.startFrame * pendingEdit.hop / pendingEdit.sr;
        const double secHi = (double) (pendingEdit.startFrame + (std::int64_t) pendingEdit.cents.size()) * pendingEdit.hop / pendingEdit.sr;
        const std::int64_t fLo = (std::int64_t) std::floor (secLo * sr / hop);
        const std::int64_t fHi = (std::int64_t) std::ceil  (secHi * sr / hop);

        for (std::int64_t f = juce::jmax<std::int64_t> (0, fLo); f < fHi && f < tr->capacity(); ++f)
        {
            const double sec = (double) f * hop / sr;
            const std::int64_t si = (std::int64_t) std::floor (sec * pendingEdit.sr / pendingEdit.hop) - pendingEdit.startFrame;
            if (si >= 0 && si < (std::int64_t) pendingEdit.cents.size())
            {
                const std::int16_t c = pendingEdit.cents[(size_t) si];
                tr->write (f, c < 0 ? -1.0f : (float) c * 0.01f);
            }
        }
        tr->resetDirty();       // loaded data must not be mistaken for a fresh capture
    }

    auto list = std::make_shared<atune::NoteList>();
    const double ratio = sr / pendingEdit.sr;
    for (auto n : pendingEdit.notes)
    {
        n.start = (std::int64_t) std::llround ((double) n.start * ratio);
        n.end   = (std::int64_t) std::llround ((double) n.end * ratio);
        list->notes.push_back (n);
    }
    commitNotes (list, false);
    {
        std::vector<std::shared_ptr<const atune::NoteList>> trash;
        const juce::SpinLock::ScopedLockType lock (editLock);
        trash.swap (undoStack);                       // a freshly loaded project starts with a clean undo history
        for (auto& r : redoStack) trash.push_back (r);
        redoStack.clear();
    }

    editSampleRate = sr;
    pendingEdit = {};
}

//==============================================================================
juce::AudioProcessorEditor* AutoTuneLiteProcessor::createEditor()
{
    return new AutoTuneLiteEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AutoTuneLiteProcessor();
}
