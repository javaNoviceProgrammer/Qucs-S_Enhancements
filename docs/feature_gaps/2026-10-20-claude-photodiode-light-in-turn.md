# A photodiode under strong light, 3: the light and the carriers in turn

The sixth of the nine missing pieces of a photodiode's physics (`2026-10-20-claude-photodiode-circuit-and-slices.md` lists them).

**Before:** a study lit a semiconductor in one pass.
1. A stationary step solved the device dark.
2. A Mode Analysis solved the light in that dark device: Franz-Keldysh absorption in the dark field, no free carriers, the absorber at its temperature then.
3. A stationary step solved the device lit by that mode.

Under strong light the lit device is not the dark one. Its bias falls through a load, its space charge screens the field, its carriers absorb and bleach, and it warms. The light was never solved again in it.

**Now:** a Stationary step has **Light and carriers in turn**:
- After a Mode Analysis whose light a semiconductor absorbs, that mode step and this one are solved again, in turn.
- They stop when the pairs made, `semi.Gop`, change from one pass to the next by less than the **Light tolerance** (10⁻⁴ by default), or after **Most passes** (20), with a warning.
- The log says how many passes, the pairs made and the last change.

What the mode sees as the device is lit:
- Franz-Keldysh absorption in its own field (the Wave Equation's `Fea`, `semi.normE`);
- free carriers' index and absorption, and band filling (their `Ncarriers`, `Pcarriers`);
- the absorber's temperature (`Tea`, a heat physics' `ht.T` solved in the same stationary step).

A sliced device's mode sees the first slice's field and carriers, at the input end. That is where they change most under strong light.

**Refused, with the reason:** the option without a Mode Analysis step before it, or without a semiconductor that one lights.

## Checks

The test is `test_fem_photodiode_power`:

| Check | Result |
|---|---|
| Germanium on silicon, Franz-Keldysh in its own field, 5 mW through 100 Ω (its bias −1 V falling to −0.40) | One pass: α 2114 /cm (the dark field's). In turn: 1981 /cm, 6 % less (the photon above the edge, a weaker field's oscillation). Settled to 8·10⁻⁸ in three passes; one pass more changes the pairs made by 3·10⁻¹⁰ |
| The same, the pairs made | 0.09 % fewer: its 15 µm absorb the light nearly whole either way. A shorter device's responsivity follows its absorption more |
| No Mode Analysis before it; no Optical Generation | Refused, each with its reason |

The light's passes take some 50 s each under the sanitizers, so that check runs on Release builds alone, like the other strong-light walks.

## Break checks

| Break | Caught by |
|---|---|
| The option ignored | `theLightInTurn`: no passes said |
| The stationary step alone again, not the mode | `theLightInTurn`: one pass more changes the pairs 8·10⁻⁴ |
| One pass alone | `theLightInTurn`: one pass more changes the pairs 1.5·10⁻⁵ |

## Next

Five of the nine remain:
- thermionic emission and tunnelling across heterointerfaces;
- hot electrons (energy balance, velocity overshoot, the transferred electrons' negative mobility);
- distortion (harmonics, compression);
- noise (shot, thermal, an APD's excess);
- the APD's convergence wall past −22.7 V.
