#include "LimiterCore.h"

namespace
{
    inline int nextPow2 (int v) { int p = 1; while (p < v) p <<= 1; return p; }
    constexpr double kPi = 3.14159265358979323846;
    inline double sinc (double x) { return std::fabs (x) < 1e-12 ? 1.0 : std::sin (kPi * x) / (kPi * x); }
}

void LimiterCore::prepare (double sampleRate, int numChannels)
{
    fs    = sampleRate > 0.0 ? sampleRate : 48000.0;
    numCh = std::max (1, numChannels);

    maxLookSamples = (int) std::ceil (kMaxLookaheadMs * 0.001 * fs);

    const int delaySize = nextPow2 (maxLookSamples + kTruePeakLatency + kTruePeakTaps + 4);
    delayMask = delaySize - 1;
    delayBuf.assign ((size_t) numCh, std::vector<float> ((size_t) delaySize, 0.0f));

    const int dqSize = nextPow2 (maxLookSamples + 4);
    dqMask = dqSize - 1;
    dqVal.assign ((size_t) dqSize, 0.0f);
    dqIdx.assign ((size_t) dqSize, 0);

    // Windowed-sinc fractional-delay taps. Output point = m + f, inputs x[m-(H-1) .. m+H].
    for (int ph = 1; ph < kOversample; ++ph)
    {
        const double f = (double) ph / kOversample;
        double h[kTruePeakTaps], sum = 0.0;
        for (int t = 0; t < kTruePeakTaps; ++t)
        {
            const double d = f - (double) (t - (kTruePeakHalf - 1));
            h[t] = sinc (d) * 0.5 * (1.0 + std::cos (kPi * d / (kTruePeakHalf + 0.0001)));   // Hann over +/-H
            sum += h[t];
        }
        for (int t = 0; t < kTruePeakTaps; ++t)
            tpTaps[ph - 1][t] = (float) (h[t] / sum);                       // unity DC gain
    }

    lookSamples = 0; tpLatency = 0; delaySamples = 0;
    configure (1.5f, true);
    setParameters (-0.1f, 1.0f, 50.0f);
}

void LimiterCore::reset() noexcept
{
    for (auto& b : delayBuf) std::fill (b.begin(), b.end(), 0.0f);
    std::fill (dqVal.begin(), dqVal.end(), 0.0f);
    std::fill (dqIdx.begin(), dqIdx.end(), (std::int64_t) 0);
    dqHead = dqTail = sampleCounter = 0;
    writePos   = 0;
    smoothGain = 1.0f;
    lastGrDb   = 0.0f;
}

bool LimiterCore::configure (float lookaheadMs, bool truePeak) noexcept
{
    int L = (int) std::lround ((double) lookaheadMs * 0.001 * fs);
    L = std::max (1, std::min (L, maxLookSamples));
    const int T = truePeak ? kTruePeakLatency : 0;

    if (delaySamples != 0 && L == lookSamples && T == tpLatency)
        return false;

    lookSamples  = L;
    tpLatency    = T;
    delaySamples = L + T;
    truePeakOn   = truePeak;
    reset();
    return true;
}

float LimiterCore::timeToCoeff (float ms, double sampleRate) noexcept
{
    if (ms <= 0.0f) return 0.0f;
    return (float) std::exp (-1.0 / ((double) ms * 0.001 * sampleRate));
}

void LimiterCore::setParameters (float thresholdDb, float attackMs, float releaseMs) noexcept
{
    const float lin = std::pow (10.0f, thresholdDb / 20.0f);
    thresholdLin = std::max (1.0e-6f, std::min (lin, 1.0f));
    attackCoeff  = timeToCoeff (attackMs,  fs);
    releaseCoeff = timeToCoeff (releaseMs, fs);
}

void LimiterCore::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    if (delayBuf.empty() || numSamples <= 0) return;
    const int nCh = std::min (numChannels, numCh);
    if (nCh <= 0) return;

    const int  mask = delayMask;
    const int  D    = delaySamples;
    const int  L    = lookSamples;
    const int  T    = tpLatency;
    const bool tp   = truePeakOn;
    const bool byp  = bypassed;
    float minGain   = 1.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const int w = writePos;

        for (int ch = 0; ch < nCh; ++ch)
        {
            float s = channels[ch][i];
            if (! std::isfinite (s)) s = 0.0f;
            delayBuf[(size_t) ch][(size_t) w] = s;
        }

        // Detector peak for sample m = n - T (taps cover x[m-(H-1) .. m+H] = x[n-(2H-1) .. n] when T = H).
        float peak = 0.0f;
        for (int ch = 0; ch < nCh; ++ch)
        {
            const float* b = delayBuf[(size_t) ch].data();
            peak = std::max (peak, std::fabs (b[(w - T) & mask]));
            if (tp)
            {
                float x[kTruePeakTaps];
                for (int t = 0; t < kTruePeakTaps; ++t) x[t] = b[(w - (kTruePeakTaps - 1) + t) & mask];
                for (int ph = 0; ph < kOversample - 1; ++ph)
                {
                    float acc = 0.0f;
                    for (int t = 0; t < kTruePeakTaps; ++t) acc += tpTaps[ph][t] * x[t];
                    peak = std::max (peak, std::fabs (acc));
                }
            }
        }

        // Sliding-window max over detector times [n-L, n].
        while (dqTail > dqHead && dqVal[(size_t) ((dqTail - 1) & dqMask)] <= peak) --dqTail;
        dqVal[(size_t) (dqTail & dqMask)] = peak;
        dqIdx[(size_t) (dqTail & dqMask)] = sampleCounter;
        ++dqTail;
        while (dqIdx[(size_t) (dqHead & dqMask)] < sampleCounter - L) ++dqHead;
        const float winMax = dqVal[(size_t) (dqHead & dqMask)];
        ++sampleCounter;

        float required = 1.0f;
        if (! byp && winMax > thresholdLin) required = thresholdLin / winMax;

        const float c = (required < smoothGain) ? attackCoeff : releaseCoeff;
        smoothGain = c * smoothGain + (1.0f - c) * required;
        if (! std::isfinite (smoothGain)) smoothGain = 1.0f;
        smoothGain = std::min (smoothGain, 1.0f);
        const float g = std::min (smoothGain, required);
        minGain = std::min (minGain, g);

        for (int ch = 0; ch < nCh; ++ch)
            channels[ch][i] = delayBuf[(size_t) ch][(size_t) ((w - D) & mask)] * g;

        writePos = (w + 1) & mask;
    }

    lastGrDb = 20.0f * std::log10 (std::max (minGain, 1.0e-6f));
}
