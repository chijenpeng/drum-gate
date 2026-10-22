#include "DrumGateEngine.h"

#include <algorithm>

namespace drumgate
{

void DrumGateEngine::prepare (double sampleRate, int numChannels)
{
    sr = sampleRate;
    detector.prepare (sampleRate);

    delaySize = (int) (sampleRate * kMaxLookaheadMs * 0.001) + 2;
    delay.assign ((size_t) std::max (1, numChannels), std::vector<float> ((size_t) delaySize, 0.0f));
    splitState.assign (delay.size(), 0.0f);
    dryScratch.assign (delay.size(), 0.0f);

    const auto coef = [sampleRate] (float ms) { return 1.0f - std::exp (-1000.0f / (ms * (float) sampleRate)); };
    bodyLevelDecay = 1.0f - coef (kBodyLevelReleaseMs);
    bleedLevelCoef = coef (kBleedLevelMs);
    tailSmoothCoef = coef (kTailSmoothMs);

    telemetryInterval = std::max (1, (int) (sampleRate / kTelemetryHz));

    setParams (params);
    reset();
}

DrumDetector::Settings DrumGateEngine::detectorSettings (DrumType type, float sensitivity) noexcept
{
    DrumDetector::Settings s;

    if (type == DrumType::tom)
    {
        // On tom mics the classifier is no better than chance at telling a soft tom from the
        // other drums (measured on kits it had not seen), so it is not used: Sensitivity is
        // simply how far below the tom's loud hits the gate still opens.
        s.loudZoneDb = 8.0f + 16.0f * sensitivity;
        s.minProbability = 2.0f;
        s.depthDb = s.loudZoneDb + 6.0f;
    }
    else
    {
        // Kick and snare: loud hits open by level; Sensitivity lowers how sure the classifier
        // has to be about the quieter ones, and how far down it is asked at all.
        s.loudZoneDb = 12.0f;
        s.minProbability = 0.9f - 0.8f * sensitivity;
        s.depthDb = 20.0f + 30.0f * sensitivity;
    }

    return s;
}

void DrumGateEngine::reset() noexcept
{
    detector.reset();

    for (auto& d : delay)
        std::fill (d.begin(), d.end(), 0.0f);
    std::fill (splitState.begin(), splitState.end(), 0.0f);

    writePos = 0;
    bodyEnv.value = highEnv.value = provisional = 0.0f;
    bodyEnv.releasing = highEnv.releasing = false;
    bodyLevel = bleedLevel = 0.0f;
    tailGain = 1.0f;
    bodyEnv.holdCounter = highEnv.holdCounter = 0;
    telemetryCounter = 0;
    pending = {};
}

void DrumGateEngine::setParams (const Params& p) noexcept
{
    params = p;

    const float lookahead = std::clamp (p.lookaheadMs, 0.0f, kMaxLookaheadMs);
    delaySamples = std::min (delaySize - 1, (int) std::lround (lookahead * 0.001 * sr));

    // The classifier and level focus follow the drum type; set only on a change, so that a
    // caller can install its own classifier afterwards.
    if ((int) p.type != appliedType)
    {
        detector.setDrumType (p.type);
        appliedType = (int) p.type;
    }

    // Lookahead is spent on two things: the attack ramp has to finish before the transient
    // leaves the delay, and the rest goes to hearing the hit out before committing. With none
    // to spend, the detector opens provisionally and settles a few ms later.
    auto s = detectorSettings (p.type, p.sensitivity);
    s.floorDb = p.floorDb;
    s.confirmMs = std::clamp (lookahead - p.attackMs - 1.0f, 0.0f, DrumDetector::kEvidenceMs);
    detector.setSettings (s);

    const auto samples = [this] (float ms) { return std::max (1.0f, ms * 0.001f * (float) sr); };

    // The envelope runs on the detector's clock, which is `lookahead` ahead of the audio
    // leaving the delay, so the hold is extended by the same amount.
    bodyEnv.attackStep = highEnv.attackStep = 1.0f / samples (p.attackMs);
    bodyEnv.holdSamples = highEnv.holdSamples = (int) samples (p.holdMs) + delaySamples;
    bodyEnv.releaseStep = 1.0f / samples (p.releaseMs);
    highEnv.releaseStep = 1.0f / samples (p.releaseMs * std::clamp (p.hfRelease, 0.05f, 1.0f));

    retractStep = 1.0f / samples (kRetractMs);

    splitCoef = 1.0f - std::exp (-2.0f * 3.14159265f * std::clamp (p.splitHz, 40.0f, 8000.0f) / (float) sr);
    rangeLog = std::min (0.0f, p.rangeDb) * 0.11512925f; // dB -> natural log of gain
    rangeGain = std::exp (rangeLog);
}

void DrumGateEngine::process (float* const* channels, int numChannels, int numSamples, float* gainTrace) noexcept
{
    numChannels = std::min (numChannels, (int) delay.size());
    if (numChannels <= 0)
        return;

    const float monoScale = 1.0f / (float) numChannels;

    for (int n = 0; n < numSamples; ++n)
    {
        float mono = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            mono += channels[ch][n];

        const float activity = detector.processSample (mono * monoScale);

        // A hit still being classified opens the gate without arming the hold, so a hit that
        // turns out to be bleed is shut again within a few ms.
        const float early = detector.getProvisionalActivity();
        provisional = early > provisional ? std::min (early, provisional + bodyEnv.attackStep)
                                          : std::max (early, provisional - retractStep);

        int readPos = writePos - delaySamples;
        if (readPos < 0)
            readPos += delaySize;

        // Read the delayed audio and split it first: the body level is needed for the gain.
        float peak = 0.0f, bodyPeak = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto i = (size_t) ch;
            delay[i][(size_t) writePos] = channels[ch][n];
            dryScratch[i] = delay[i][(size_t) readPos];

            // First-order complementary split: body + high == dry exactly, with no latency.
            splitState[i] += splitCoef * (dryScratch[i] - splitState[i]);
            peak = std::max (peak, std::abs (dryScratch[i]));
            bodyPeak = std::max (bodyPeak, std::abs (splitState[i]));
        }

        // Envelope position is linear in dB, so a linear release is an exponential fade.
        const float bodyPosition = std::max (provisional, bodyEnv.process (activity, provisional));
        const float highGain = std::exp (rangeLog * (1.0f - std::max (provisional, highEnv.process (activity, provisional))));
        float bodyGain = std::exp (rangeLog * (1.0f - bodyPosition));

        // The tail. What a limiter set just above the bleed would leave, subtracted from the
        // track, is the drum's level minus the bleed's: a gain of 1 - bleed / level. Used here
        // only while the gate is releasing, it lets the tail fade as the drum itself sinks
        // into the bleed, however long the release is set, and leaves the attack alone.
        bodyLevel = std::max (bodyPeak, bodyLevel * bodyLevelDecay);

        if (bodyPosition <= 0.0f)
            bleedLevel += bleedLevelCoef * (bodyLevel - bleedLevel);   // learnt while closed

        const float target = bodyEnv.releasing ? std::clamp (1.0f - kTailMargin * bleedLevel / (bodyLevel + 1.0e-9f), 0.0f, 1.0f) : 1.0f;
        tailGain += tailSmoothCoef * (target - tailGain);
        bodyGain = std::max (rangeGain, std::min (bodyGain, tailGain));

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto i = (size_t) ch;
            const float wet = splitState[i] * bodyGain + (dryScratch[i] - splitState[i]) * highGain;
            channels[ch][n] = params.listenBleed ? dryScratch[i] - wet : wet;
        }

        if (++writePos >= delaySize)
            writePos = 0;

        if (gainTrace != nullptr)
            gainTrace[n] = bodyGain;

        pending.inputPeak = std::max (pending.inputPeak, peak);
        pending.gain = std::min (pending.gain, bodyGain);
        pending.activity = std::max (pending.activity, activity);

        if (++telemetryCounter >= telemetryInterval)
        {
            telemetryCounter = 0;
            telemetry.push (pending);
            pending = {};
        }
    }
}

} // namespace drumgate
