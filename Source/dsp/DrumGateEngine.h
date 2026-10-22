#pragma once

#include "DrumDetector.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace drumgate
{

/** One display point: what the (delayed) audio did and what gain was applied to it. */
struct TelemetryFrame
{
    float inputPeak = 0.0f;
    float gain = 1.0f;
    float activity = 0.0f;
};

/** Single-producer single-consumer ring, audio thread -> UI. Drops frames when full. */
class TelemetryFifo
{
public:
    bool push (const TelemetryFrame& f) noexcept
    {
        const int w = writeIndex.load (std::memory_order_relaxed);
        const int next = (w + 1) % kSize;
        if (next == readIndex.load (std::memory_order_acquire))
            return false;
        frames[(size_t) w] = f;
        writeIndex.store (next, std::memory_order_release);
        return true;
    }

    bool pop (TelemetryFrame& f) noexcept
    {
        const int r = readIndex.load (std::memory_order_relaxed);
        if (r == writeIndex.load (std::memory_order_acquire))
            return false;
        f = frames[(size_t) r];
        readIndex.store ((r + 1) % kSize, std::memory_order_release);
        return true;
    }

private:
    static constexpr int kSize = 2048;
    std::array<TelemetryFrame, kSize> frames;
    std::atomic<int> writeIndex { 0 }, readIndex { 0 };
};

/**
    Detector + lookahead delay + gain stage.

    The gain is never a mute: it moves between 0 dB and `rangeDb` according to the detector's
    activity, so `rangeDb` is the "how much bleed stays" control.

    A drum's transient is broadband but its sustain is little more than the fundamental. So a
    hit opens the whole spectrum, and afterwards the part above the fundamental closes quickly
    while the body is left to ring. The body's release is the time set, or sooner if the drum
    has already sunk into the bleed: see the tail gain in process().
*/
class DrumGateEngine
{
public:
    static constexpr float kMaxLookaheadMs = 10.0f;
    static constexpr float kTelemetryHz = 400.0f;
    static constexpr float kBodyLevelReleaseMs = 30.0f, kBleedLevelMs = 400.0f, kTailSmoothMs = 5.0f;
    static constexpr float kTailMargin = 1.5f;   // the tail is gone once the body is within 3.5 dB of the bleed
    static constexpr float kRetractMs = 3.0f;    // closing time after a provisional opening is rejected

    struct Params
    {
        DrumType type = DrumType::snare;
        float sensitivity = 0.5f;   // 0..1
        float floorDb = -70.0f;
        float lookaheadMs = 10.0f;  // 0 = tracking mode, no added latency
        float attackMs = 1.0f;
        float holdMs = 40.0f;
        float releaseMs = 120.0f;
        float splitHz = 450.0f;     // just above the drum's fundamental: below is body, above is attack
        float hfRelease = 0.3f;     // release of the part above the split, as a fraction of releaseMs
        float rangeDb = -24.0f;     // gain applied to everything that is not a hit
        bool listenBleed = false;   // output what is being removed instead
    };

    void prepare (double sampleRate, int numChannels);
    void reset() noexcept;
    void setParams (const Params& p) noexcept;

    /** Processes in place. If gainTrace is non-null it receives the body gain per sample. */
    void process (float* const* channels, int numChannels, int numSamples, float* gainTrace = nullptr) noexcept;

    int getLatencySamples() const noexcept { return delaySamples; }
    /** What Sensitivity means to the detector, for each drum. */
    static DrumDetector::Settings detectorSettings (DrumType type, float sensitivity) noexcept;

    DrumDetector& getDetector() noexcept { return detector; }
    TelemetryFifo& getTelemetry() noexcept { return telemetry; }

private:
    struct Envelope
    {
        float value = 0.0f, attackStep = 1.0f, releaseStep = 1.0f;
        int holdSamples = 0, holdCounter = 0;
        bool releasing = false;   // past the hold and on the way down

        /** `handover` is a level the gate has already reached by other means (the provisional
            opening), so a rising target can start from there instead of ramping again. */
        float process (float target, float handover) noexcept
        {
            if (target > value)
                value = std::max (value, std::min (target, handover));

            releasing = false;

            if (target >= value)
            {
                value = std::min (target, value + attackStep);
                holdCounter = holdSamples;
            }
            else if (holdCounter > 0)
                --holdCounter;
            else
            {
                value = std::max (target, value - releaseStep);
                releasing = true;
            }

            return value;
        }
    };

    double sr = 48000.0;
    Params params;
    int appliedType = -1;
    DrumDetector detector;
    Envelope bodyEnv, highEnv;
    float provisional = 0.0f, retractStep = 1.0f;
    float rangeLog = 0.0f, rangeGain = 1.0f, splitCoef = 0.0f;

    // Tail: level of the body band, its level while the gate is closed (the bleed), and the
    // gain that follows from the two.
    float bodyLevel = 0.0f, bleedLevel = 0.0f, tailGain = 1.0f;
    float bodyLevelDecay = 0.0f, bleedLevelCoef = 0.0f, tailSmoothCoef = 0.0f;

    std::vector<std::vector<float>> delay;
    std::vector<float> splitState, dryScratch;
    int delaySize = 1, delaySamples = 0, writePos = 0;

    TelemetryFifo telemetry;
    TelemetryFrame pending;
    int telemetryInterval = 120, telemetryCounter = 0;
};

} // namespace drumgate
