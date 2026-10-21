# A photodiode under strong light, 6: its distortion and compression

The seventh of the nine missing pieces of a photodiode's physics (`2026-10-20-claude-photodiode-circuit-and-slices.md` lists them): its harmonics and its compression under strong modulated light.

**Before:** a step in time gave a photodiode's current under modulated light, but nothing analysed it. Its harmonics, its distortion in dBc and its compression had to be taken out of a dataset and Fourier-transformed elsewhere.

**Now:** a **Harmonics** derived value (Derived Values > Harmonics) analyses a global's time series.

| Setting | Meaning |
|---|---|
| Expression | A global in time: a photodiode's current through its load (`semi.IL_<contact>`), its contact's (`semi.I_<contact>`), any global expression |
| Fundamental f0 | The light's or the source's modulation |
| Harmonics | How many: H1 to Hn, HD2 to HDn |
| Periods | The last whole periods of the solution's times analysed, after its start's transient |

**What it gives**, over those periods:
- the mean;
- each harmonic's amplitude, from its Fourier coefficient (2/T) ∫ y e^(−j n 2π f0 t) dt, by the trapezoids of the snapshots. The window's first value is interpolated where it falls between two output times. This is exact for evenly spaced times over whole periods;
- HDn = 20 log₁₀(Hn/H1) in dBc;
- THD = √(Σ Hn²)/H1.

**Compression:** along a sweep of the light's power, compare H1 with its low-power value times the power's ratio. 1 dB of compression is H1 1 dB below that.

**Found on the way:** none.

## Checks

The tests are in `test_fem_photodiode_power`:

| Check | Result |
|---|---|
| A silicon bar (Ohm's alone), 10 mV at 1 GHz and 2 mV at 2 GHz | H1 and H2 the voltages over its resistance to 10⁻³; HD2 −13.98 dBc (20 log₁₀ 0.2); H3 and the mean nought to 10⁻⁴; THD 20 % |
| The same, its output times off the window's start (32 ps to 2.24 ns) | H1 and H2 to 5·10⁻³ |
| The snapshot selected before | Its globals its own after |
| The bias tee into 100 kΩ, 5 % at 2 GHz (round 1's) | H1 and the mean the test's own sum's to 10⁻⁹ |
| The p-i-n, 50 % modulated at 1 GHz, through 100 kΩ in series at −2 V, weakly (10 nA's worth) | HD2 −91.6 dBc, THD 0.003 % |
| The same, strongly (20 µA's worth: its drop all but the bias) | Compressed: H1 per pair 0.30 of the weak one's; HD2 −18.8 dBc, HD3 −37.0 dBc, THD 11.6 % |

The checks run in time, so on Release builds alone.

## Break checks

| Break | Caught by |
|---|---|
| The coefficients without their 2/T | `harmonicsOfABar`: H1 half |
| Each harmonic the next one's | `harmonicsOfABar`: H1 the second's |
| THD with the fundamental | `harmonicsOfABar`: 1.02 against 0.2 |
| HD in 10 log₁₀ | `harmonicsOfABar`: −6.99 against −13.98 dBc |
| The selected snapshot's globals not restored | `harmonicsOfABar` |
| The window's start not interpolated | `harmonicsOfABar`, its times off the window: H1 1.6 % low |

**Not caught at first, one:** the window's start. The tests' times all fell on it, and with the times off it the signal was near nought there. The window now opens near the current's peak, between two output times.

## Next

Two of the nine remain:
- noise (shot, thermal, an APD's excess);
- the APD's convergence wall past −22.7 V.
