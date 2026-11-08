#!/bin/bash
# The adaptive check against fem_converge's uniform route on the shipped models
# (see the write-up beside this folder):
#   ./build.sh probe3 && PROBE3="$PWD/probe3" ./compare.sh > compare.txt
PROBE3="${PROBE3:-$(cd "$(dirname "$0")" && pwd)/probe3}"
cd "$(dirname "$0")/../../../qucs-s-26.1.1/examples/multiphysics"
run(){ echo "=== $*"; timeout 900 "$PROBE3" "$@" 2>&1 | grep -v "^QWARN"; }
run adaptive_corner.qfem es.W 1e-3
run adaptive_corner.qfem es.W 1e-4
run coax.qfem es.C11 1e-4 auto 9.703562695222404e-11
run microstrip.qfem es.C11 1e-3
run tsv_axisymmetric.qfem es.C11 1e-3
run resistor_heating.qfem ec.R 1e-3
run resistor_heating.qfem ht.Tmax 1e-4
run bimetal_strip.qfem solid.dmax 1e-4
run heater_transient.qfem ht.Tmax 1e-4
run drum_modes.qfem c.lambda 1e-5
run gapped_core_inductor.qfem mf.L_go 1e-3
run induction_heating.qfem mf.P 1e-3
export REFMAX=4e5; run wr90_modes.qfem emw.neff 1e-4 auto 0.9447273549638127; unset REFMAX
run quantum_wire.qfem qw.Ee1 1e-4
run heatsink_import.qfem ht.Tmax 1e-3
export REFMAX=6e5; run soi_waveguide_modes.qfem emw.neff 1e-3; unset REFMAX
# The uniform route (refine: uniform) against the same references: every element
# cut each time, its changes alternating with the cut (2026-11-08).
X=1.1326822727e-11
run adaptive_corner.qfem es.W 1e-3 uniform $X
run adaptive_corner.qfem es.W 1e-4 uniform $X
run coax.qfem es.C11 1e-4 uniform 9.703562695222404e-11
run microstrip.qfem es.C11 1e-3 uniform 1.207455664e-10
run tsv_axisymmetric.qfem es.C11 1e-3 uniform 1.23999745676e-13
run resistor_heating.qfem ec.R 1e-3 uniform 0.201306240254
run wr90_modes.qfem emw.neff 1e-4 uniform 0.9447273549638127
