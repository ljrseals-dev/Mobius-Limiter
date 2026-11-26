#pragma once
// LimiterCore: stereo-linked look-ahead peak limiter. No JUCE dependency.
//
// Signal path (per sample):
//   input -> sanitise (NaN/Inf -> 0) -> write to delay line
//   detector peak (sample peak, or 4x true-peak estimate, aligned to the delay line)
//   sliding-window MAX of the peak over the look-ahead window
//   required gain = min(1, thr / windowMax)
//   one-pole smoothing (attack when reducing, release when recovering)
//   final gain = min(smoothed, required)   <- guarantees the ceiling even if attack is slow
//   output = delayed audio * final gain
//
// Latency = lookaheadSamples + (truePeak ? kTruePeakLatency : 0). Constant while processing.
// Everything is allocated in prepare(); process()/configure()/setParameters() never allocate.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class LimiterCore
{
public:
    static constexpr double kMaxLookaheadMs  = 10.0;
    static constexpr int    kTruePeakHalf    = 8;   // taps on each side of the interpolation point
    static constexpr int    kTruePeakLatency = kTruePeakHalf;   // detector look-ahead in samples
    static constexpr int    kTruePeakTaps    = 2 * kTruePeakHalf;   // 16-tap polyphase
    static constexpr int    kOversample      = 4;

    LimiterCore() = default;
    LimiterCore (const LimiterCore&) = delete;
    LimiterCore& operator= (const LimiterCore&) = delete;

    // Not real-time safe (allocates).
    void prepare (double sampleRate, int numChannels);

    // Real-time safe. Clears state.
    void reset() noexcept;

    // Real-time safe. Changing look-ahead or true-peak mode changes latency and clears state.
    // Returns true if the latency changed.
    bool configure (float lookaheadMs, bool truePeak) noexcept;

    // Real-time safe, any time.
    void setParameters (float thresholdDb, float attackMs, float releaseMs) noexcept;
    void setBypassed (bool shouldBypass) noexcept { bypassed = shouldBypass; }

    // In place. channels[ch][0..numSamples). Channels beyond the prepared count are ignored.
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    int   getLatencySamples()  const noexcept { return delaySamples; }
    float getGainReductionDb() const noexcept { return lastGrDb; }   // meter only: benign race

    // One-pole coefficient for time constant `ms` at sample rate `fs`. 0 ms -> 0 (instant).
    static float timeToCoeff (float ms, double fs) noexcept;

private:
    double fs = 48000.0;
    int    numCh = 0;

    std::vector<std::vector<float>> delayBuf;   // power-of-two ring per channel
    int delayMask = 0;
    int writePos  = 0;

    std::vector<float>        dqVal;            // monotonic deque: sliding-window max
    std::vector<std::int64_t> dqIdx;
    int          dqMask = 0;
    std::int64_t dqHead = 0, dqTail = 0, sampleCounter = 0;

    float tpTaps[kOversample - 1][kTruePeakTaps] {};

    int  maxLookSamples = 0;
    int  lookSamples    = 0;   // L
    int  tpLatency      = 0;   // T (0 or kTruePeakLatency)
    int  delaySamples   = 0;   // D = L + T
    bool truePeakOn     = false;

    float thresholdLin = 1.0f, attackCoeff = 0.0f, releaseCoeff = 0.0f;
    float smoothGain   = 1.0f;
    float lastGrDb     = 0.0f;
    bool  bypassed     = false;
};
