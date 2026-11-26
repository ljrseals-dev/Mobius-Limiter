## Limiter mode (v0.2, experimental)

Time-domain, stereo-linked look-ahead peak limiter. Not claimed production-ready: the acceptance
criteria (clean builds on 3 OSes, PDC in two DAWs, real-time allocation check, CI) are not yet met.

**Signal path:** peak (sample or true-peak estimate) -> sliding-window max over the look-ahead ->
gain = min(1, ceiling/peak) -> one-pole attack/release -> final gain = min(smoothed, required) -> applied to
the delayed audio. The last min() makes the sample-peak ceiling hold even with a slow attack, at the cost
of hard gain edges when attack is much longer than look-ahead (keep attack <= ~look-ahead/3).

**Latency / PDC:** `latency = round(lookahead_ms * fs) + (true-peak ? 8 : 0)` samples (1.5 ms @ 48 kHz,
true-peak on = 72 + 8 = 80). Constant during playback. Look-ahead and True Peak are not host-automatable:
changing them clears limiter state (brief gap) and republishes latency from the message thread. Bypass keeps
the delay (no PDC change) and releases gain to unity.

**Parameters:** Threshold -60..0 dBFS (-0.1), Attack 0.1..100 ms (1), Release 1..1000 ms (50),
Lookahead 0.1..10 ms (1.5), Makeup 0..24 dB (0), True Peak (on), Bypass. State version 2; presets from the
Mobius spectral build are ignored (defaults load).

**Measured (this repo's tests and `Tools/sweep.py`, seed 99, 648 renders, 48 kHz):**
- Null test (0 dBFS ceiling, input below it): bit-exact delayed copy, sample-peak and true-peak modes.
- Sample-peak ceiling: never exceeded (0.0 dB over) on all four materials.
- True-peak mode is a 4x, 16-tap estimator, not ITU-R BS.1770 compliant. Worst true peak above the ceiling
  (scipy 4x measure): sine 0.006 dB, 45 Hz bass 0.03 dB, mix 0.65 dB, noise-burst "drums" 1.29 dB
  (2.48 dB with true-peak off). Use a ceiling margin (e.g. -1 dBFS) for delivery.
- 1 kHz sine THD+N: -150 dB or better across the grid (gain is constant on steady tones).

**Known limitations:** no program-dependent release (a 50 ms release will modulate sustained LF material;
raise release or look-ahead); no soft knee; makeup is manual; true-peak accuracy as above; look-ahead
changes click; `triggerAsyncUpdate()` from the audio thread is not strictly real-time safe on all platforms;
CPU target (<2% core) and DAW PDC checks not yet measured; sweep metrics rank settings but do not judge
audio quality, real-time safety or DSP correctness (human review required).
