// Plain C++ tests (no framework). CHECK always runs, even in Release (assert would not).
#include "dsp/LimiterCore.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int g_failures = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (! (cond)) { ++g_failures; \
    std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static constexpr double kFs = 48000.0;
using Buf = std::vector<std::vector<float>>;

static Buf make (int ch, int n) { return Buf ((size_t) ch, std::vector<float> ((size_t) n, 0.0f)); }

static void run (LimiterCore& lim, Buf& b, int blockSize)
{
    const int n = (int) b[0].size();
    std::vector<float*> p (b.size());
    for (int off = 0; off < n; off += blockSize)
    {
        const int len = std::min (blockSize, n - off);
        for (size_t c = 0; c < b.size(); ++c) p[c] = b[c].data() + off;
        lim.process (p.data(), (int) b.size(), len);
    }
}

static float dbToLin (float d) { return std::pow (10.0f, d / 20.0f); }

// 1. coefficient math
static void testCoefficients()
{
    CHECK (LimiterCore::timeToCoeff (0.0f, kFs) == 0.0f);
    CHECK (std::fabs (LimiterCore::timeToCoeff (1.0f, 1000.0) - std::exp (-1.0f)) < 1e-6f);   // tau = 1 sample
    // step response of the smoother reaches 1 - 1/e after tau samples
    const float c = LimiterCore::timeToCoeff (1.0f, kFs);          // tau = 48 samples
    float y = 0.0f;
    for (int i = 0; i < 48; ++i) y = c * y + (1.0f - c) * 1.0f;
    CHECK (std::fabs (y - (1.0f - std::exp (-1.0f))) < 1e-4f);
}

// 2. delay-line read/write: impulse emerges exactly `latency` samples later, any block size
static void testDelayLine()
{
    for (bool tp : { false, true })
        for (int block : { 1, 7, 64, 480, 4096 })
        {
            LimiterCore lim; lim.prepare (kFs, 2);
            lim.configure (1.5f, tp);
            lim.setParameters (0.0f, 1.0f, 50.0f);
            const int D = lim.getLatencySamples();
            CHECK (D == (int) std::lround (1.5e-3 * kFs) + (tp ? LimiterCore::kTruePeakLatency : 0));
            auto b = make (2, 2000);
            b[0][100] = 0.5f; b[1][101] = -0.25f;
            run (lim, b, block);
            for (int i = 0; i < 2000; ++i)
            {
                CHECK (b[0][i] == (i == 100 + D ? 0.5f : 0.0f));
                CHECK (b[1][i] == (i == 101 + D ? -0.25f : 0.0f));
            }
        }
}

// 3. null test: threshold 0 dBFS, input below it -> bit-exact delayed copy
static void testNull()
{
    for (bool tp : { false, true })
    {
        const int n = 20000;
        auto in = make (2, n);
        for (int i = 0; i < n; ++i)
        {
            in[0][i] = 0.5f * std::sin (6.2831853f * 440.0f * (float) i / (float) kFs);
            in[1][i] = 0.3f * std::sin (6.2831853f * 1234.0f * (float) i / (float) kFs);
        }
        auto out = in;
        LimiterCore lim; lim.prepare (kFs, 2);
        lim.configure (2.0f, tp);
        lim.setParameters (0.0f, 1.0f, 50.0f);
        run (lim, out, 333);
        const int D = lim.getLatencySamples();
        for (int ch = 0; ch < 2; ++ch)
            for (int i = D; i < n; ++i)
                CHECK (out[(size_t) ch][(size_t) i] == in[(size_t) ch][(size_t) (i - D)]);
    }
}

// 4. gain law: steady DC above threshold settles to thr/peak
static void testGainLaw()
{
    LimiterCore lim; lim.prepare (kFs, 2);
    lim.configure (1.5f, false);
    lim.setParameters (-6.0f, 0.1f, 50.0f);
    auto b = make (2, 48000);
    for (auto& ch : b) for (auto& s : ch) s = 1.0f;
    run (lim, b, 512);
    CHECK (std::fabs (b[0].back() - dbToLin (-6.0f)) < 1e-4f);
    CHECK (std::fabs (lim.getGainReductionDb() - (-6.0f)) < 0.01f);
    // below threshold: unity
    LimiterCore lim2; lim2.prepare (kFs, 2); lim2.configure (1.5f, false);
    lim2.setParameters (-6.0f, 1.0f, 50.0f);
    auto q = make (2, 4800);
    for (auto& ch : q) for (auto& s : ch) s = 0.25f;
    run (lim2, q, 512);
    CHECK (q[0].back() == 0.25f);
}

// 5. ceiling holds for a hard transient even with a deliberately slow attack
static void testCeilingTransient()
{
    for (bool tp : { false, true })
        for (float atk : { 0.1f, 1.0f, 100.0f })
        {
            LimiterCore lim; lim.prepare (kFs, 2);
            lim.configure (1.5f, tp);
            lim.setParameters (-6.0f, atk, 50.0f);
            auto b = make (2, 24000);
            for (int i = 0; i < 24000; ++i) { b[0][i] = (i >= 9000 && i < 9100) ? 1.0f : 0.05f; b[1][i] = b[0][i] * -0.8f; }
            run (lim, b, 128);
            const float ceil = dbToLin (-6.0f) * 1.00001f;
            float mx = 0.0f;
            for (auto& ch : b) for (float s : ch) mx = std::max (mx, std::fabs (s));
            CHECK (mx <= ceil);
        }
}

// 6. inter-sample peak: fs/4 sine at 45 deg phase has sample peaks 0.707 of true peak
static void testTruePeak()
{
    const int n = 48000;
    auto b = make (1, n);
    for (int i = 0; i < n; ++i)
        b[0][i] = 1.0f * std::sin (3.14159265f * 0.5f * (float) i + 0.7853982f);   // samples are +/-0.7071
    auto sampleOnly = b, tpOn = b;
    LimiterCore a; a.prepare (kFs, 1); a.configure (1.5f, false); a.setParameters (-3.0f, 0.1f, 50.0f);
    LimiterCore t; t.prepare (kFs, 1); t.configure (1.5f, true);  t.setParameters (-3.0f, 0.1f, 50.0f);
    run (a, sampleOnly, 512); run (t, tpOn, 512);
    // true peak of the input is 1.0 (> -3 dBFS = 0.708); sample peak is 0.7071 (~ at threshold)
    float ra = 0.0f, rt = 0.0f;
    for (int i = 40000; i < n; ++i) { ra = std::max (ra, std::fabs (sampleOnly[0][i])); rt = std::max (rt, std::fabs (tpOn[0][i])); }
    CHECK (ra > 0.70f);                 // sample-peak mode leaves it essentially untouched
    CHECK (rt < ra * 0.85f);            // true-peak mode reduces it (~ -3 dB) because it sees the real peak
}

// 7. stereo link: both channels get identical gain
static void testStereoLink()
{
    LimiterCore lim; lim.prepare (kFs, 2); lim.configure (1.5f, false);
    lim.setParameters (-6.0f, 1.0f, 50.0f);
    auto b = make (2, 48000);
    for (int i = 0; i < 48000; ++i) { b[0][i] = 0.9f; b[1][i] = 0.09f; }
    run (lim, b, 512);
    for (int i = 47000; i < 48000; ++i)
        CHECK (std::fabs (b[1][i] / b[0][i] - 0.1f) < 1e-5f);
}

// 8. NaN / Inf in -> finite out, and the limiter keeps working afterwards
static void testNonFinite()
{
    LimiterCore lim; lim.prepare (kFs, 2); lim.configure (1.0f, true);
    lim.setParameters (-1.0f, 1.0f, 50.0f);
    auto b = make (2, 4000);
    for (auto& ch : b) for (auto& s : ch) s = 0.5f;
    b[0][10] = std::nanf (""); b[1][20] = INFINITY; b[0][30] = -INFINITY;
    run (lim, b, 64);
    for (auto& ch : b) for (float s : ch) CHECK (std::isfinite (s));
    CHECK (std::fabs (b[0].back() - 0.5f) < 1e-6f);
}

// 9. block-size independence
static void testBlockIndependence()
{
    const int n = 30000;
    auto ref = make (2, n);
    for (int i = 0; i < n; ++i) { ref[0][i] = 1.5f * std::sin (0.05f * (float) i); ref[1][i] = 1.2f * std::sin (0.031f * (float) i); }
    auto a = ref, c = ref;
    LimiterCore l1; l1.prepare (kFs, 2); l1.setParameters (-3.0f, 0.5f, 80.0f);
    LimiterCore l2; l2.prepare (kFs, 2); l2.setParameters (-3.0f, 0.5f, 80.0f);
    run (l1, a, n); run (l2, c, 97);
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < n; ++i) CHECK (a[(size_t) ch][(size_t) i] == c[(size_t) ch][(size_t) i]);
}

// 10. bypass releases to unity without altering latency
static void testBypass()
{
    LimiterCore lim; lim.prepare (kFs, 2); lim.configure (1.5f, false);
    lim.setParameters (-12.0f, 1.0f, 5.0f);
    const int D = lim.getLatencySamples();
    lim.setBypassed (true);
    auto b = make (2, 4000);
    for (auto& ch : b) for (auto& s : ch) s = 0.9f;
    run (lim, b, 256);
    CHECK (lim.getLatencySamples() == D);
    CHECK (b[0].back() == 0.9f);
}

int main()
{
    testCoefficients(); testDelayLine(); testNull(); testGainLaw(); testCeilingTransient();
    testTruePeak(); testStereoLink(); testNonFinite(); testBlockIndependence(); testBypass();
    std::printf ("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
