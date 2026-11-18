#!/usr/bin/env bash
#
# A step's memory, watched while it runs (Linux only, /proc): CI's sanitized
# Debug build lost its runner seven times in eleven runs, out of memory as
# far as anything said, and a runner lost keeps only the part of its log it
# had uploaded - some 2 MB of it, a quarter of an hour into the build
# So the watch says each
# new high-water mark as it is reached, with the biggest processes then -
# the compiler with its source, the linker with what it writes -, and at the
# end the mark and each process's own peak, in the log and the job's summary.
#
# Usage:
#   memory-watch.sh start <dir>    sample every 2 s in the background
#   memory-watch.sh stop <dir>     end it and say what it saw
#
# <dir> holds the samples (peaks.txt: a process's peak RSS in KiB and its
# label; mark.txt: the system's mark) and the watcher's pid.
set -euo pipefail

cmd="${1:?usage: memory-watch.sh start|stop <dir>}"
dir="${2:?usage: memory-watch.sh start|stop <dir>}"

# The label of a process from its command line: a compiler's source file, a
# linker's output, else its program.
label() {
    awk '{
        prog = $1; sub(".*/", "", prog); src = ""; base = ""; out = ""
        for (i = 2; i <= NF; i++) {
            if ($i == "-o" && i < NF) out = $(i + 1)
            else if ($i == "-dumpbase" && i < NF) base = $(i + 1)
            else if ($i ~ /[^\/]\.(c|cc|cpp|cxx)$/ && $(i - 1) !~ /^-(o|dumpbase|dumpbase-ext)$/) src = $i
        }
        if (base != "") src = base
        if (prog ~ /^(cc1|cc1plus)$/ && src != "") { sub(".*/", "", src); print prog " " src }
        else if (prog ~ /^(ld|ld\.bfd|ld\.gold|ld\.lld|collect2|mold)$/ && out != "") { sub(".*/", "", out); print prog " " out }
        else print prog
    }'
}

gib() { awk -v k="$1" 'BEGIN { printf "%.1f GiB", k / 1048576 }'; }

sample() {
    # (A sample that fails is skipped, not the watch ended.)
    set +e +o pipefail
    local mark=0 said=0
    while :; do
        # Used: total less available; swap: total less free (KiB).
        local used swap now
        read -r used swap < <(awk '/^MemTotal:/{t=$2} /^MemAvailable:/{a=$2} /^SwapTotal:/{st=$2} /^SwapFree:/{sf=$2} END{print t-a, st-sf}' /proc/meminfo)
        now=$(( used + swap ))
        # Every process past 200 MB, its label and RSS.
        ps -eo rss=,args= --sort=-rss | awk '$1 > 204800' | while read -r rss args; do
            printf '%s\t%s\n' "$rss" "$(printf '%s\n' "$args" | label)"
        done > "$dir/now.txt"
        if [ -s "$dir/now.txt" ]; then
            # Keep each label's peak.
            sort -t$'\t' -k2,2 -k1,1nr "$dir/now.txt" "$dir/peaks.txt" 2>/dev/null \
                | awk -F'\t' '!seen[$2]++' > "$dir/peaks.new"
            mv "$dir/peaks.new" "$dir/peaks.txt"
        fi
        if [ "$now" -gt "$mark" ]; then
            mark=$now
            local at biggest
            at=$(date -u +%H:%M:%S)
            biggest=$(head -4 "$dir/now.txt" | awk -F'\t' '{printf "%s%s %.1f GiB", (NR > 1 ? ", " : ""), $2, $1 / 1048576}')
            printf '%s %s %s %s\n' "$at" "$used" "$swap" "$biggest" > "$dir/mark.txt"
            # Said when it has grown by half a GiB since it was last said.
            if [ $(( mark - said )) -ge 524288 ]; then
                said=$mark
                echo "[memory] $at high-water $(gib "$mark"): $(gib "$used") in use, $(gib "$swap") swapped; biggest: $biggest"
            fi
        fi
        # (Its sleep holds none of the step's output open once it is ended.)
        sleep 2 > /dev/null 2>&1
    done
}

case "$cmd" in
start)
    mkdir -p "$dir"
    : > "$dir/peaks.txt"
    free -h
    sample &
    echo $! > "$dir/pid"
    ;;
stop)
    if [ -f "$dir/pid" ]; then
        kill "$(cat "$dir/pid")" 2>/dev/null || true
        rm -f "$dir/pid"
    fi
    {
        if [ -f "$dir/mark.txt" ]; then
            read -r at used swap biggest < "$dir/mark.txt"
            echo "Memory's high-water mark: $(gib $(( used + swap ))) at $at UTC - $(gib "$used") in use, $(gib "$swap") swapped."
            echo "Biggest then: $biggest"
        fi
        echo "Each process's peak:"
        sort -t$'\t' -k1,1nr "$dir/peaks.txt" | head -20 | awk -F'\t' '{printf "  %6.2f GiB  %s\n", $1 / 1048576, $2}'
    } > "$dir/report.txt"
    cat "$dir/report.txt"
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
        { echo '```'; cat "$dir/report.txt"; echo '```'; } >> "$GITHUB_STEP_SUMMARY"
    fi
    free -h
    ;;
*)
    echo "usage: memory-watch.sh start|stop <dir>" >&2
    exit 2
    ;;
esac
