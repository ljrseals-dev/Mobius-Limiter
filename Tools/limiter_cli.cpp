// Headless renderer for sweeps: limiter_cli in.f32 out.f32 thr atk rel look truepeak(0|1) [fs]
// Files are raw little-endian float32, interleaved stereo. Prints the latency in samples.
#include "dsp/LimiterCore.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 8) { std::fprintf (stderr, "usage: %s in out thrDb atkMs relMs lookMs truePeak [fs]\n", argv[0]); return 2; }
    const double fs = argc > 8 ? std::atof (argv[8]) : 48000.0;

    std::FILE* f = std::fopen (argv[1], "rb");
    if (! f) { std::fprintf (stderr, "cannot open input\n"); return 2; }
    std::vector<float> inter;
    float tmp[4096];
    size_t got;
    while ((got = std::fread (tmp, sizeof (float), 4096, f)) > 0) inter.insert (inter.end(), tmp, tmp + got);
    std::fclose (f);

    const size_t n = inter.size() / 2;
    std::vector<float> l (n), r (n);
    for (size_t i = 0; i < n; ++i) { l[i] = inter[2 * i]; r[i] = inter[2 * i + 1]; }

    LimiterCore lim;
    lim.prepare (fs, 2);
    lim.configure ((float) std::atof (argv[6]), std::atoi (argv[7]) != 0);
    lim.setParameters ((float) std::atof (argv[3]), (float) std::atof (argv[4]), (float) std::atof (argv[5]));

    float* ch[2] = { l.data(), r.data() };
    for (size_t off = 0; off < n; off += 512)
    {
        float* p[2] = { ch[0] + off, ch[1] + off };
        lim.process (p, 2, (int) std::min<size_t> (512, n - off));
    }

    for (size_t i = 0; i < n; ++i) { inter[2 * i] = l[i]; inter[2 * i + 1] = r[i]; }
    f = std::fopen (argv[2], "wb");
    if (! f) { std::fprintf (stderr, "cannot open output\n"); return 2; }
    std::fwrite (inter.data(), sizeof (float), inter.size(), f);
    std::fclose (f);
    std::printf ("%d\n", lim.getLatencySamples());
    return 0;
}
