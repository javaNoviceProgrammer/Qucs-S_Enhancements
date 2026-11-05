#!/bin/bash
# usage: canary.sh <compiler> <source> <flags-set>...: one column of each flags
# set per case: the sanitizer's word for it where caught, "-" where it ran on.
cxx=$1; src=$2; shift 2
dir=$(mktemp -d)
i=0; sets=("$@")
for f in "${sets[@]}"; do
  $cxx -std=c++20 -fsanitize=address,undefined -fno-omit-frame-pointer $f -w "$src" -o "$dir/c$i" || exit 1
  i=$((i+1))
done
printf '%-30s' case; for f in "${sets[@]}"; do printf ' %-26s' "$f"; done; echo
for name in $("$dir/c0" --list); do
  printf '%-30s' "$name"
  i=0
  for f in "${sets[@]}"; do
    out=$(ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=0 "$dir/c$i" "$name" 2>&1); rc=$?
    word=$(printf '%s\n' "$out" | grep -m1 -oE 'AddressSanitizer: (attempting )?[a-zA-Z-]+|runtime error: [^ ]+( [^ ]+)?( [^ ]+)?' | sed -E 's/AddressSanitizer: (attempting )?//; s/runtime error: /ub: /' | cut -c1-24)
    if [ -z "$word" ]; then
      if [ "$rc" -ne 0 ]; then word="rc$rc"; else word="-"; fi
    fi
    printf ' %-26s' "$word"
    i=$((i+1))
  done
  echo
done
rm -rf "$dir"
