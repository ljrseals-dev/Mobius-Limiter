#!/usr/bin/env python3
"""Headless parameter sweep that drives the REAL compiled LimiterCore via limiter_cli.

  python3 Tools/sweep.py --cli build/limiter_cli --seed 1234 --out sweep.csv

Needs numpy + scipy. Material is generated from --seed, so runs are reproducible.
Metrics are measured on the output after removing the reported latency:
  lufs_in/lufs_out  BS.1770-4 integrated loudness (K-weighting, 400 ms blocks, -70 / -10 LU gates)
  true_peak_dbfs    4x polyphase oversampled peak (scipy resample_poly; approximates BS.1770 Annex 2)
  sample_peak_dbfs, peak_change_db, thdn_db (sine material only)
These are objective numbers for ranking. They do not judge pumping, distortion character or
real-time safety: those need listening and code review.
"""
import argparse, csv, itertools, os, subprocess, tempfile
import numpy as np
from scipy.signal import resample_poly, lfilter

FS = 48000

def k_weight(x):
    # BS.1770 K-weighting biquads at 48 kHz (published coefficients)
    b1 = [1.53512485958697, -2.69169618940638, 1.19839281085285]
    a1 = [1.0, -1.69065929318241, 0.73248077421585]
    b2 = [1.0, -2.0, 1.0]
    a2 = [1.0, -1.99004745483398, 0.99007225036621]
    return lfilter(b2, a2, lfilter(b1, a1, x, axis=-1), axis=-1)

def lufs(x):
    if x.shape[-1] < int(0.4 * FS):
        return float("nan")
    z = k_weight(x) ** 2
    step, blk = int(0.1 * FS), int(0.4 * FS)
    ms = np.array([z[:, i:i + blk].mean(axis=1).sum() for i in range(0, z.shape[1] - blk + 1, step)])
    l = -0.691 + 10 * np.log10(np.maximum(ms, 1e-12))
    g = ms[l > -70]
    if g.size == 0:
        return float("-inf")
    rel = -0.691 + 10 * np.log10(g.mean()) - 10
    g = ms[(l > -70) & (l > rel)]
    return float(-0.691 + 10 * np.log10(g.mean())) if g.size else float("-inf")

def peak_db(x):
    return float(20 * np.log10(np.max(np.abs(x)) + 1e-12))

def true_peak_db(x, guard=2400):
    # resample_poly rings at the file edges, so ignore `guard` input samples at each end.
    up = resample_poly(x, 4, 1, axis=-1)
    return peak_db(up[:, 4 * guard: up.shape[1] - 4 * guard])

def thdn_db(x, f0=1000.0, guard=2400):
    # Trim to a whole number of cycles (f0 = 1 kHz at 48 kHz -> 48 samples) so the FFT has no leakage.
    m = x.mean(axis=0)[guard:-guard]
    cyc = int(round(FS / f0))
    m = m[: (m.size // cyc) * cyc]
    s = np.abs(np.fft.rfft(m)) ** 2
    k = int(round(f0 * m.size / FS))
    sig = s[max(0, k - 1):k + 2].sum()
    return float(10 * np.log10(max(s.sum() - sig, 1e-20) / max(sig, 1e-20)))

def pink(rng, n):
    w = rng.standard_normal(n)
    f = np.fft.rfft(w); f[1:] /= np.sqrt(np.arange(1, f.size)); f[0] = 0
    p = np.fft.irfft(f, n)
    return p / np.max(np.abs(p))

def materials(seed, dur):
    rng = np.random.default_rng(seed)
    n = int(FS * dur); t = np.arange(n) / FS
    sine = 0.89 * np.sin(2 * np.pi * 1000 * t)
    drums = np.zeros(n)
    for k in range(0, n, FS // 4):
        L = int(0.03 * FS); env = np.exp(-np.arange(L) / (0.006 * FS))
        drums[k:k + L] += (rng.standard_normal(L) * env)[: max(0, min(L, n - k))] if k + L <= n else 0
    drums = 0.9 * drums / np.max(np.abs(drums))
    bass = 0.95 * np.sin(2 * np.pi * 45 * t) + 0.05 * pink(rng, n)
    mix = 0.6 * pink(rng, n) + 0.4 * drums + 0.4 * np.sin(2 * np.pi * 80 * t)
    out = {"sine1k": sine, "drums": drums, "bass45": bass, "mix": mix / np.max(np.abs(mix)) * 0.98}
    return {k: np.stack([v, v * (0.9 + 0.1 * rng.random())]) for k, v in out.items()}

def render(cli, x, thr, atk, rel, look, tp):
    with tempfile.TemporaryDirectory() as d:
        fi, fo = os.path.join(d, "i.f32"), os.path.join(d, "o.f32")
        x.T.astype("<f4").tofile(fi)
        r = subprocess.run([cli, fi, fo, str(thr), str(atk), str(rel), str(look), str(int(tp)), str(FS)],
                           capture_output=True, text=True, check=True)
        lat = int(r.stdout.strip())
        y = np.fromfile(fo, dtype="<f4").reshape(-1, 2).T.astype(np.float64)
    return y, lat

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", default="build/limiter_cli")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--dur", type=float, default=6.0)
    ap.add_argument("--out", default="sweep.csv")
    ap.add_argument("--quick", action="store_true", help="smaller grid")
    a = ap.parse_args()

    grid = dict(
        threshold=[-0.1, -3.0, -6.0] if not a.quick else [-1.0, -6.0],
        attack=[0.1, 1.0, 5.0] if not a.quick else [1.0],
        release=[20.0, 100.0, 400.0] if not a.quick else [100.0],
        lookahead=[1.0, 3.0, 6.0] if not a.quick else [3.0],
        truepeak=[0, 1],
    )
    mats = materials(a.seed, a.dur)
    keys = list(grid)
    rows = []
    for name, x in mats.items():
        lin = lufs(x)
        for combo in itertools.product(*(grid[k] for k in keys)):
            p = dict(zip(keys, combo))
            y, lat = render(a.cli, x, p["threshold"], p["attack"], p["release"], p["lookahead"], p["truepeak"])
            ya, xa = y[:, lat:], x[:, : y.shape[1] - lat]
            row = dict(seed=a.seed, material=name, latency=lat, **p,
                       lufs_in=lufs(xa), lufs_out=lufs(ya),
                       sample_peak_dbfs=peak_db(ya), true_peak_dbfs=true_peak_db(ya),
                       peak_change_db=float(20 * np.log10((np.max(np.abs(ya)) + 1e-12) / (np.max(np.abs(xa)) + 1e-12))),
                       thdn_db=thdn_db(ya) if name == "sine1k" else float("nan"))
            row["ceiling_violation_db"] = row["true_peak_dbfs"] - p["threshold"]
            rows.append(row)
    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
    print(f"{len(rows)} rows -> {a.out} (seed {a.seed})")

if __name__ == "__main__":
    main()
