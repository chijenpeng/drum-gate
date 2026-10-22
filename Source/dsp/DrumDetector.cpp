#include "DrumDetector.h"
#include "ClassifierWeights.h"

#include <algorithm>

namespace drumgate
{

namespace
{
    constexpr float kTriggerDb = 6.0f;      // novelty that counts as an onset
    constexpr float kKneeDb = 6.0f;         // range over which a level confidence goes 0 -> 1
    constexpr float kHysteresisDb = 3.0f;   // novelty must drop this far below the trigger to re-arm
    constexpr float kRetriggerRiseDb = 6.0f;   // novelty climbing this far out of a dip is a new onset
    constexpr float kBgRiseMs = 80.0f;      // background lags a rising level: this is the onset memory
    constexpr float kBgFallMs = 6.0f;       // ...but follows a decay closely, so a hit in a tail still stands out
    constexpr float kLevelWindowMs = 20.0f; // how long after settling the level rule may still be met
    constexpr float kReferenceDecaySec = 120.0f;   // the loud-hit reference forgets slowly
    constexpr float kReferenceAttack = 0.5f;       // and moves halfway to each louder hit
    constexpr float kCymbalShare = 0.45f, kCymbalShareKnee = 0.3f;   // share of energy above 3 kHz that marks a cymbal
    constexpr size_t kHighShareFeature = 25;
    constexpr float kProbabilityKnee = 0.2f;
    constexpr float kTiny = 1.0e-10f;

    // Feature regions, as band index ranges [first, last).
    struct Region { int first, last; };
    constexpr Region kLow { 0, 4 };    // under 350 Hz
    constexpr Region kBody { 2, 4 };   // 150-350 Hz
    constexpr Region kMid { 4, 8 };    // 350 Hz - 3 kHz
    constexpr Region kHigh { 8, kNumBands };

    float smoothstep (float x) noexcept
    {
        x = std::clamp (x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }

    float onePoleCoef (float timeMs, double rate) noexcept
    {
        return 1.0f - std::exp (-1000.0f / (timeMs * (float) rate));
    }

    /** Envelope smoothing time for a band: roughly half a period of its centre, so the squared
        signal's ripple does not read as novelty; never under 1 ms or the high bands get noisy. */
    float envelopeMs (int band) noexcept
    {
        const float centre = std::sqrt (kBandEdges[(size_t) band] * kBandEdges[(size_t) band + 1]);
        return std::clamp (500.0f / centre, 1.0f, 10.0f);
    }

    float toDb (float power) noexcept { return 10.0f * std::log10 (power + kTiny); }
}

const ClassifierWeights& defaultClassifier (DrumType type) noexcept
{
    return trainedClassifiers[(size_t) type];
}

BandVector levelFocus (DrumType type) noexcept
{
    //                                   40-80  -150   -250   -350   -500   -800   -1k5   -3k    -6k    -10k   -16k
    switch (type)
    {
        case DrumType::kick:  return { 1.00f, 1.00f, 0.25f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f };
        case DrumType::snare: return { 0.00f, 0.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 0.25f, 0.00f };
        case DrumType::tom:   return { 0.25f, 1.00f, 1.00f, 1.00f, 0.50f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f };
    }

    return {};
}

void DrumDetector::prepare (double sampleRate)
{
    sr = sampleRate;
    tickInterval = std::max (1, (int) std::lround (sampleRate / 3000.0));
    tickMs = 1000.0f * (float) tickInterval / (float) sampleRate;

    const double controlRate = sampleRate / tickInterval;
    bgUpCoef = onePoleCoef (kBgRiseMs, controlRate);
    bgDownCoef = onePoleCoef (kBgFallMs, controlRate);
    referenceDecay = std::exp (-1.0f / (kReferenceDecaySec * (float) controlRate));

    for (int b = 0; b < kNumBands; ++b)
    {
        const auto i = (size_t) b;
        highPass[i].setHighPass (sampleRate, kBandEdges[i]);
        lowPass[i].setLowPass (sampleRate, kBandEdges[i + 1]);
        envCoef[i] = onePoleCoef (envelopeMs (b), sampleRate);
    }

    if (*std::max_element (focus.begin(), focus.end()) <= 0.0f)
        setDrumType (DrumType::snare);

    setSettings (settings);
    reset();
}

void DrumDetector::reset() noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        highPass[(size_t) b].reset();
        lowPass[(size_t) b].reset();
    }

    power.fill (0.0f);
    bgLog.fill (std::log (floorPower));
    tickCounter = 0;
    eventActive = pulseActive = settled = false;
    eventActivity = activity = provisionalActivity = 0.0f;
    novelty = 0.0f;
}

void DrumDetector::setSettings (const Settings& s) noexcept
{
    settings = s;
    floorPower = std::pow (10.0f, s.floorDb * 0.1f);
}

void DrumDetector::setDrumType (DrumType type) noexcept
{
    focus = levelFocus (type);
    classifier = defaultClassifier (type);
}

float DrumDetector::processSample (float x) noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto i = (size_t) b;
        const float y = lowPass[i].process (highPass[i].process (x));
        power[i] += envCoef[i] * (y * y - power[i]);
    }

    if (++tickCounter >= tickInterval)
    {
        tickCounter = 0;
        controlTick();
    }

    return activity;
}

void DrumDetector::startEvent (const BandVector& bandBackground) noexcept
{
    preBandBackground = bandBackground;
    eventActive = pulseActive = true;
    settled = false;
    ageMs = 0.0f;
    rawPeak = eventActivity = levelPeak = 0.0f;
    noveltyPeak = noveltyTrough = novelty;
    gatheredTicks = 0;
    powerSum.fill (0.0f);
    for (auto& region : timing)
        region.fill (0.0f);
}

void DrumDetector::computeFeatures (FeatureVector& out) const noexcept
{
    // Layout: [0, 11) spectrum shape in dB below the strongest band; [11, 22) each band's rise
    // over its level before the hit, in dB; [22, 26) share of the energy under 350 Hz, in
    // 150-350 Hz, in 350 Hz - 3 kHz and above; [26, 38) for the low, mid and high regions, the
    // share of the region's energy in each quarter of the window.
    const float ticks = (float) std::max (1, gatheredTicks);
    BandVector mean;
    float total = kTiny, strongest = -1.0e9f;

    for (int b = 0; b < kNumBands; ++b)
    {
        mean[(size_t) b] = powerSum[(size_t) b] / ticks;
        total += mean[(size_t) b];
        strongest = std::max (strongest, toDb (mean[(size_t) b]));
    }

    size_t f = 0;
    for (int b = 0; b < kNumBands; ++b)
        out[f++] = toDb (mean[(size_t) b]) - strongest;

    for (int b = 0; b < kNumBands; ++b)
        out[f++] = 10.0f * std::log10 ((mean[(size_t) b] + kTiny) / (preBandBackground[(size_t) b] + kTiny));

    for (const auto region : { kLow, kBody, kMid, kHigh })
    {
        float sum = 0.0f;
        for (int b = region.first; b < region.last; ++b)
            sum += mean[(size_t) b];
        out[f++] = sum / total;
    }

    for (const auto& region : timing)
    {
        float sum = kTiny;
        for (auto q : region)
            sum += q;
        for (auto q : region)
            out[f++] = q / sum;
    }
}

void DrumDetector::settleEvent() noexcept
{
    computeFeatures (lastFeatures);

    float logit = classifier.bias;
    for (size_t i = 0; i < (size_t) kNumFeatures; ++i)
        logit += classifier.weight[i] * (lastFeatures[i] - classifier.mean[i]) / std::max (classifier.scale[i], 1.0e-6f);

    lastProbability = 1.0f / (1.0f + std::exp (-logit));

    const float ticks = (float) std::max (1, gatheredTicks);
    for (int b = 0; b < kNumBands; ++b)
        lastSpectrum[(size_t) b] = powerSum[(size_t) b] / ticks;

    settled = true;
    ++eventCount;
}

float DrumDetector::notCymbal() const noexcept
{
    // A cymbal heard through a drum mic has nearly all of its energy above 3 kHz (a median of
    // 91% on the recordings measured, against 10-20% for drums), which makes this one number a
    // safer veto than the classifier's opinion of a kit it has never heard.
    return 1.0f - smoothstep ((lastFeatures[kHighShareFeature] - kCymbalShare) / kCymbalShareKnee);
}

float DrumDetector::decide() const noexcept
{
    // Near the drum's own loud hits, level is enough. Further down a soft stroke and another
    // drum's bleed arrive at the same level, so the classifier has to be convinced. With no
    // reference yet there are no "loud hits" to be near, and every hit is treated as quiet.
    const bool hasReference = loudReference > 0.0f;
    const float quietness = hasReference ? smoothstep ((-settings.loudZoneDb - lastRelativeDb) / kKneeDb) : 1.0f;
    const float byClassifier = smoothstep ((lastProbability - settings.minProbability) / kProbabilityKnee + 0.5f);
    const float byLevel = hasReference ? smoothstep ((lastRelativeDb + settings.depthDb) / kKneeDb) : 1.0f;

    return notCymbal() * byLevel * (1.0f + quietness * (byClassifier - 1.0f));
}

void DrumDetector::controlTick() noexcept
{
    float focusLevel = 0.0f;
    BandVector bandBackground;
    novelty = 0.0f;

    for (int b = 0; b < kNumBands; ++b)
    {
        const auto i = (size_t) b;
        const float p = power[i];
        const float bg = std::exp (bgLog[i]);

        bandBackground[i] = bg;
        focusLevel += focus[i] * p;

        // Each band is judged against its own background, so a click stands out even while
        // the drum's low end is still ringing from the hit before.
        novelty += std::max (0.0f, 10.0f * std::log10 ((p + floorPower) / (bg + floorPower)));

        const float logP = std::log (p + 1.0e-12f);
        bgLog[i] += (logP > bgLog[i] ? bgUpCoef : bgDownCoef) * (logP - bgLog[i]);
    }

    novelty /= (float) kNumBands;
    loudReference *= referenceDecay;

    const bool gathering = eventActive && ageMs < kEvidenceMs;

    // An onset is the novelty crossing the threshold, or, while it is still above it (a roll,
    // or bleed landing on a ringing drum), dipping and climbing again.
    const bool fresh = ! pulseActive || novelty > noveltyTrough + kRetriggerRiseDb;

    if (novelty > kTriggerDb && fresh && ! gathering)
    {
        if (eventActive)
            finishEvent();
        startEvent (bandBackground);
    }
    else
    {
        if (novelty > noveltyPeak)
            noveltyPeak = noveltyTrough = novelty;
        else
            noveltyTrough = std::min (noveltyTrough, novelty);

        if (novelty < kTriggerDb - kHysteresisDb)
            pulseActive = false;
    }

    provisionalActivity = 0.0f;

    if (! eventActive)
    {
        activity = 0.0f;
        return;
    }

    const bool stillGathering = ageMs < kEvidenceMs;
    const float ageBefore = ageMs;
    ageMs += tickMs;
    levelPeak = std::max (levelPeak, focusLevel);

    if (stillGathering)
    {
        rawPeak = std::max (rawPeak, smoothstep ((novelty - kTriggerDb) / kKneeDb));
        ++gatheredTicks;

        const int quarter = std::min (kNumQuarters - 1, (int) (ageBefore / kEvidenceMs * (float) kNumQuarters));
        size_t index = 0;

        for (const auto region : { kLow, kMid, kHigh })
        {
            for (int b = region.first; b < region.last; ++b)
                timing[index][(size_t) quarter] += power[(size_t) b];
            ++index;
        }

        for (int b = 0; b < kNumBands; ++b)
            powerSum[(size_t) b] += power[(size_t) b];

        if (ageMs >= kEvidenceMs)
        {
            settleEvent();
        }
        else if (ageMs >= settings.confirmMs)
        {
            // Not enough lookahead to wait for the verdict: open on the raw onset. If the hit
            // turns out to be bleed, the gate shuts again a few ms later, and a short tick of
            // bleed is better than a clipped attack.
            provisionalActivity = rawPeak;
        }
    }

    if (settled)
    {
        // The level rule may be met a little after the onset, which is when a kick's low end
        // arrives; the verdict can only go up during that time.
        lastRelativeDb = loudReference > 0.0f ? 10.0f * std::log10 (levelPeak / loudReference + 1.0e-12f) : 0.0f;
        eventActivity = std::max (eventActivity, decide());
    }

    if (ageMs >= kEvidenceMs + kLevelWindowMs)
        finishEvent();

    // A settled hit is announced for the length of the level window; how long the gate then
    // stays open is the envelope's business (hold and release), which keeps the length
    // predictable on a ringing drum.
    activity = (eventActive && settled) ? eventActivity : 0.0f;
}

void DrumDetector::finishEvent() noexcept
{
    eventActive = false;

    if (! settled || notCymbal() < 0.5f)
        return;

    // The reference follows the loudest thing that could be this drum, accepted or not, so it
    // can never get stuck above what the drum actually plays.
    if (levelPeak > loudReference)
        loudReference += (loudReference > 0.0f ? kReferenceAttack : 1.0f) * (levelPeak - loudReference);
}

} // namespace drumgate
