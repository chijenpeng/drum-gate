#pragma once

#include "dsp/DrumGateEngine.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace ParamID
{
    inline constexpr auto type = "type", mode = "mode", sensitivity = "sensitivity", bleed = "bleed",
                          length = "length", listen = "listen";
}

class DrumGateProcessor final : public juce::AudioProcessor,
                                private juce::AsyncUpdater
{
public:
    /** What the detector made of the most recent hit, for the display. */
    struct LastHit
    {
        drumgate::BandVector spectrum {};   // mean power per band over the hit's first 8 ms
        float probability = 0.0f;           // the classifier's "this is the drum"
        float relativeDb = 0.0f;            // level against the drum's loud hits
        int count = 0;                      // hits so far; changes when there is a new one
    };

    DrumGateProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // UI side
    juce::AudioProcessorValueTreeState apvts;

    LastHit getLastHit() const;
    /** True once the detector knows how loud this drum's hits are. */
    bool knowsDrumLevel() const noexcept { return referenceLevel.load() > 0.0f; }
    /** Forgets how loud the drum is; the detector works it out again as the drum plays. */
    void forgetDrumLevel() noexcept { referenceToApply.store (0.0f); }
    bool popTelemetry (drumgate::TelemetryFrame& frame) noexcept { return engine.getTelemetry().pop (frame); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    drumgate::DrumGateEngine::Params currentParams() const noexcept;
    void handleAsyncUpdate() override;
    float param (const char* id) const noexcept { return apvts.getRawParameterValue (id)->load(); }

    drumgate::DrumGateEngine engine;

    // The drum's level as the detector knows it: mirrored here after every block so it can be
    // saved with the session, and handed back through referenceToApply (negative = nothing to
    // apply) when a session is restored or the user asks to forget it.
    std::atomic<float> referenceLevel { 0.0f }, referenceToApply { -1.0f };
    std::atomic<int> lastType { -1 };

    mutable juce::SpinLock lastHitLock;
    LastHit lastHit;

    std::atomic<int> latencyToReport { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumGateProcessor)
};
