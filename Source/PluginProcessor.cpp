#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace drumgate;

namespace
{
    const juce::Identifier drumLevelId ("drumLevel"), drumLevelTypeId ("drumLevelType");
}

juce::AudioProcessorValueTreeState::ParameterLayout DrumGateProcessor::createLayout()
{
    using Range = juce::NormalisableRange<float>;
    using Float = juce::AudioParameterFloat;

    const auto unit = [] (const char* label, int decimals)
    {
        return juce::AudioParameterFloatAttributes().withLabel (label).withStringFromValueFunction (
            [label, decimals] (float v, int) { return juce::String (v, decimals) + " " + label; });
    };

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ParamID::type, 1 }, "Drum",
                                                              juce::StringArray { "Kick", "Snare", "Tom" }, 1));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ParamID::mode, 1 }, "Mode",
                                                              juce::StringArray { "Tracking", "Mix" }, 1));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::sensitivity, 1 }, "Sensitivity",
                                         Range (0.0f, 100.0f, 0.1f), 50.0f, unit ("%", 0)));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::bleed, 1 }, "Bleed",
                                         Range (-80.0f, 0.0f, 0.1f), -24.0f, unit ("dB", 1)));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::length, 1 }, "Length",
                                         Range (0.0f, 100.0f, 0.1f), 40.0f, unit ("%", 0)));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamID::listen, 1 }, "Listen Bleed", false));

    return layout;
}

DrumGateEngine::Params DrumGateProcessor::currentParams() const noexcept
{
    const int mode = juce::roundToInt (param (ParamID::mode));
    const int type = juce::jlimit (0, 2, juce::roundToInt (param (ParamID::type)));
    const float length = param (ParamID::length) * 0.01f;

    DrumGateEngine::Params p;
    p.type = (DrumType) type;
    p.sensitivity = param (ParamID::sensitivity) * 0.01f;
    p.rangeDb = param (ParamID::bleed);
    p.listenBleed = param (ParamID::listen) > 0.5f;

    // Mix mode buys the detector 10 ms to hear a hit out before committing, and lets the gate
    // open ahead of it. Tracking has no lookahead, so it opens faster to keep as much of the
    // attack as it can. (A 20 ms mode detected no better and was dropped.)
    const bool tracking = mode == 0;
    p.lookaheadMs = tracking ? 0.0f : 10.0f;
    p.attackMs = tracking ? 0.3f : 1.0f;

    // The tail that Length keeps is the drum's body; above it the gate closes sooner. The
    // split sits just over where each drum's fundamental can be: kick to 80 Hz, toms to about
    // 165 Hz, snare to 300 Hz for a high-tuned one.
    constexpr float splitHz[] { 150.0f, 450.0f, 300.0f };
    p.splitHz = splitHz[type];

    // Length is one control for how much of the drum's tail survives: it stretches the hold
    // and the release together, from a tight 15 + 30 ms to a ringing 250 + 1200 ms.
    p.holdMs = 15.0f * std::pow (250.0f / 15.0f, length);
    p.releaseMs = 30.0f * std::pow (40.0f, length);

    return p;
}

DrumGateProcessor::DrumGateProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "DrumGate", createLayout())
{
}

bool DrumGateProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && out == layouts.getMainInputChannelSet();
}

void DrumGateProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, getTotalNumInputChannels());
    engine.setParams (currentParams());
    engine.getDetector().setReferenceLevel (referenceLevel.load());

    latencyToReport = engine.getLatencySamples();
    setLatencySamples (latencyToReport);
}

void DrumGateProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    auto& detector = engine.getDetector();
    const auto params = currentParams();
    engine.setParams (params);

    // A level is measured through the drum type's own filter, so it means nothing for another.
    if (const int previous = lastType.exchange ((int) params.type); previous >= 0 && previous != (int) params.type)
        detector.setReferenceLevel (0.0f);

    if (const float pending = referenceToApply.exchange (-1.0f); pending >= 0.0f)
        detector.setReferenceLevel (pending);

    if (engine.getLatencySamples() != latencyToReport.load())
    {
        latencyToReport = engine.getLatencySamples();
        triggerAsyncUpdate();
    }

    engine.process (buffer.getArrayOfWritePointers(), getTotalNumInputChannels(), buffer.getNumSamples());
    referenceLevel.store (detector.getReferenceLevel());

    // The audio thread never waits for the display; a missed update is caught next block.
    if (detector.getEventCount() != lastHit.count)
    {
        const juce::SpinLock::ScopedTryLockType lock (lastHitLock);

        if (lock.isLocked())
        {
            lastHit.spectrum = detector.getLastSpectrum();
            lastHit.probability = detector.getLastProbability();
            lastHit.relativeDb = detector.getLastRelativeLevelDb();
            lastHit.count = detector.getEventCount();
        }
    }
}

void DrumGateProcessor::handleAsyncUpdate()
{
    setLatencySamples (latencyToReport.load());
}

DrumGateProcessor::LastHit DrumGateProcessor::getLastHit() const
{
    const juce::SpinLock::ScopedLockType lock (lastHitLock);
    return lastHit;
}

void DrumGateProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Saved with the session so that a song already played is gated correctly from bar one.
    state.setProperty (drumLevelId, (double) referenceLevel.load(), nullptr);
    state.setProperty (drumLevelTypeId, juce::roundToInt (param (ParamID::type)), nullptr);

    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void DrumGateProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    const auto state = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (state);

    const bool sameDrum = (int) state.getProperty (drumLevelTypeId, -1) == juce::roundToInt (param (ParamID::type));
    const float level = sameDrum ? (float) (double) state.getProperty (drumLevelId, 0.0) : 0.0f;
    lastType = -1;   // the restored type is not a change of type
    referenceLevel.store (level);
    referenceToApply.store (level);
}

juce::AudioProcessorEditor* DrumGateProcessor::createEditor()
{
    return new DrumGateEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrumGateProcessor();
}
