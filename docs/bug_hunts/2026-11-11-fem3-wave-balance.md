# test_fem3_wave's junction 8e-4 off now and then under load (2026-11-11)

**Seen.** A clean-checkout run of main 65867f9b (ctest -j 8, load average some 35) failed once at test_fem3_wave.cpp:373,
`aJunctionAgreesWithTheExpansion`: balance 1.00003266457 and 1.0008314972 against 1 within 1e-5. Rerun alone, it passed.

**Reproduced.** The junction alone, eight copies at once, five rounds: 1 run of 40 gave balance 1.000032664577 and
1.000831497188 - the failure's digits exactly, a discrete other outcome, not noise -, the rest within 4e-10 of 1.

**Cause.** The ports' cuts' mode analysis (`fem_waves.cpp`, the shift-and-invert search for λ = -β²) leaves out A's null space
(Ez alone, λ nought) by a bound of 1e-8 k0². Those eigenvalues are a cluster of many between the propagating and the evanescent
modes, and the search finds them only to its accuracy about its shift: some 1e-9 of σ = -(k0 n)², 12 k0² here, by rounding
that differs from run to run with the threaded factorizations (K's own assembly was the same each time: its checksum
identical). Now and then one came out at |λ| 1.35e-8 k0², past the bound: port 1 had a mode of n_eff 4.27e-5 - 1.08e-4j
(β² complex in a lossless metal window: no mode), its field normalized to 1 W some 150 times too large, absorbed as one of
the port's. Every S moved by some 1e-3 (|S21|² 0.900926 for 0.899756), the balance by 8e-4. The same cluster's members, dropped,
make the port's modes 16 to 18 of the 20 asked, the evanescent ones in an order that varies - harmless: those runs agree
within 1e-10.

**Fix.** The null space's bound scaled with the shift: |λ| < 1e-6 |σ| (the coupled formulation's alike, |β| < 1e-3 |shift|),
a thousand times the error seen. A real mode that near its cutoff - |n_eff| under a thousandth of the index searched about -
is left out too; at 1 W its field is as large, and no port can take it.

**Shown.**
- With the fix, under the same load: 120 runs of the junction, then 80 more, every one |S21|² 0.899756, balance within 3.7e-10
  of 1. In the first 120 the new bound caught one eigenvalue at 1.07e-8 k0², which the old one would have let through.
- The whole test_fem3_wave, eight copies at once twice: 16 of 16.
- Every test_fem* and test_multiphysics* suite (43): passed.
