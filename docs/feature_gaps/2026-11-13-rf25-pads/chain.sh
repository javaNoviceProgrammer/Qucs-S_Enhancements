#!/bin/zsh
# Each Palace run alone: waits until no build or test run is going (ninja, ctest), then runs it on 8 ranks.
# PALACE: the Palace launcher to use. Run from the folder holding the runs' .json and .msh files.
# usage: PALACE=<launcher> ./chain.sh name [name...]   (name.json each; name.log written)
: ${PALACE:?set PALACE to the Palace launcher}
for name in "$@"; do
  while pgrep -x ninja >/dev/null || pgrep -x ctest >/dev/null; do sleep 30; done
  echo "$(date +%T) $name start (load $(sysctl -n vm.loadavg))"
  OPENBLAS_NUM_THREADS=1 "$PALACE" -np 8 $name.json > $name.log 2>&1
  echo "$(date +%T) $name exit $? $(grep -m1 'offline phase elapsed' $name.log)"
done
