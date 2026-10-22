#pragma once

#include "Biquad.h"

#include <algorithm>
#include <array>

namespace drumgate
{

constexpr int kNumBands = 11;
using BandVector = std::array<float, kNumBands>;

/** Band edges in Hz; band b spans kBandEdges[b] .. kBandEdges[b + 1]. */
constexpr std::array<float, kNumBands + 1> kBandEdges { 40.0f, 80.0f, 150.0f, 250.0f, 350.0f, 500.0f, 800.0f,
                                                        1500.0f, 3000.0f, 6000.0f, 10000.0f, 16000.0f };

enum class DrumType { kick = 0, snare, tom };

/** What the classifier sees of a hit: see DrumDetector::computeFeatures for the layout. */
constexpr int kNumFeatures = 38;
using FeatureVector = std::array<float, kNumFeatures>;

/** A logistic regression over standardised features: P(this is the drum) = sigmoid (w . z + bias),
    z = (x - mean) / scale. Trained offline by tools/train_classifier.py. (A 16-unit network was
    tried and did no better on kits it had not seen.) */
struct ClassifierWeights
{
    FeatureVector mean {}, scale {}, weight {};
    float bias = 0.0f;
};

/** The weights shipped with the plug-in for a close mic on the given drum. */
const ClassifierWeights& defaultClassifier (DrumType type) noexcept;

/** Band weights for measuring how loud a hit is: the sidechain filter an engineer would set for
    a gate on this drum. */
BandVector levelFocus (DrumType type) noexcept;

/**
    Causal drum-hit detector for a close-mic track.

    Is it a hit? An 11-band filterbank feeds fast power envelopes. Each band keeps a lagging
    background level, and the onset novelty is the mean rise over that background in dB.

    Is it this drum? Two rules, because recordings show two different situations.
    Loud hits: anything near the level of this drum's own recent loud hits is the drum, as with
    any gate, except that a cymbal is still turned away.
    Quieter hits: a soft stroke and another drum's bleed arrive at the same level, so level
    cannot decide. A classifier looks at the first 8 ms instead (the spectrum's shape, how far
    each band rose, how the energy is spread in time) and the hit opens the gate if it is
    probably this drum.

    The output is a 0..1 activity value meant to drive a gain envelope.
*/
class DrumDetector
{
public:
    struct Settings
    {
        float loudZoneDb = 12.0f;     // within this of the drum's loud hits, level alone is enough
        float minProbability = 0.5f;  // below that, how sure the classifier must be; above 1 = never
        float depthDb = 40.0f;        // hits further than this below the loud ones never open
        float floorDb = -70.0f;       // absolute level below which nothing triggers
        float confirmMs = 0.0f;       // wait this long before opening at all (needs lookahead)
    };

    /** How much of a hit the classifier sees. */
    static constexpr float kEvidenceMs = 8.0f;

    void prepare (double sampleRate);
    void reset() noexcept;

    void setSettings (const Settings& s) noexcept;
    void setDrumType (DrumType type) noexcept;
    void setClassifier (const ClassifierWeights& weights) noexcept { classifier = weights; }

    /** The level of this drum's recent loud hits, which the detector works out as it goes and
        compares every hit against. Until the drum has been hit it has none (zero), and bleed
        can only be judged by the classifier. A host can save it and hand it back, so that a
        song played once is gated correctly from its first bar the next time. */
    float getReferenceLevel() const noexcept { return loudReference; }
    void setReferenceLevel (float level) noexcept { loudReference = std::max (0.0f, level); }

    /** Feeds one sample and returns the current activity (0..1). */
    float processSample (float x) noexcept;

    /** Early estimate for a hit that is still being examined; zero once it is settled.
        Meant to open the gate immediately, without committing it to a hold. */
    float getProvisionalActivity() const noexcept { return provisionalActivity; }

    // About the most recent hit
    int getEventCount() const noexcept { return eventCount; }               // settled hits so far
    float getLastProbability() const noexcept { return lastProbability; }
    float getLastRelativeLevelDb() const noexcept { return lastRelativeDb; } // 0 with no reference yet
    const FeatureVector& getLastFeatures() const noexcept { return lastFeatures; }
    const BandVector& getLastSpectrum() const noexcept { return lastSpectrum; } // mean power per band

    float getNoveltyDb() const noexcept { return novelty; }
    int getTickInterval() const noexcept { return tickInterval; }

private:
    void controlTick() noexcept;
    void startEvent (const BandVector& bandBackground) noexcept;
    void settleEvent() noexcept;
    void finishEvent() noexcept;
    void computeFeatures (FeatureVector& out) const noexcept;
    float decide() const noexcept;
    float notCymbal() const noexcept;

    double sr = 48000.0;
    int tickInterval = 16, tickCounter = 0;
    float tickMs = 0.33f;

    std::array<Biquad, kNumBands> highPass, lowPass;
    BandVector envCoef {}, power {}, bgLog {};
    float bgUpCoef = 0.0f, bgDownCoef = 0.0f;

    Settings settings;
    float floorPower = 1.0e-7f;
    BandVector focus {};
    ClassifierWeights classifier;

    // Current hit
    static constexpr int kNumRegions = 3, kNumQuarters = 4;
    bool eventActive = false, pulseActive = false, settled = false;
    float ageMs = 0.0f, rawPeak = 0.0f, eventActivity = 0.0f, levelPeak = 0.0f;
    float noveltyPeak = 0.0f, noveltyTrough = 0.0f;
    int gatheredTicks = 0;
    BandVector powerSum {}, preBandBackground {};
    std::array<std::array<float, kNumQuarters>, kNumRegions> timing {};

    // Level of this drum's recent loud hits, through the focus weights.
    float loudReference = 0.0f, referenceDecay = 1.0f;

    float novelty = 0.0f, activity = 0.0f, provisionalActivity = 0.0f;
    float lastProbability = 0.0f, lastRelativeDb = 0.0f;
    FeatureVector lastFeatures {};
    BandVector lastSpectrum {};
    int eventCount = 0;
};

} // namespace drumgate
