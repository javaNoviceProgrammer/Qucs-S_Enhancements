#!/usr/bin/env python3
"""Where a CI job's unit-test time goes, from its log (gh run view --job <id> --log > job.log).

    times.py LABEL=job.log [LABEL=job.log ...]

For each log: every suite's seconds, the sum, and how many tests ran at once in each ten-minute window
(4.00 means the runner's four lanes were full). With two or more logs, a table of the slowest suites side by
side and each log's test-seconds against the first's on the suites both finished.
"""
import collections
import datetime
import re
import sys

DONE = re.compile(r'Test +#\d+: (\S+) \.+.*?([\d.]+) sec')
START = re.compile(r'^\s*Start +\d+: (\S+)')


def stamp(s):
    return datetime.datetime.fromisoformat(s.lstrip('﻿')[:26])


def read(path):
    seconds, started, ended = {}, {}, {}
    for line in open(path, errors='replace'):
        parts = line.rstrip('\n').split('\t', 2)
        if len(parts) < 3:
            continue
        text = parts[2].lstrip('﻿')
        when, body = text[:28], text[29:]
        m = DONE.search(body)
        if m:
            seconds[m.group(1)] = float(m.group(2))
            ended[m.group(1)] = stamp(when)
            continue
        m = START.match(body)
        if m:
            started[m.group(1)] = stamp(when)
    return seconds, started, ended


def lanes(started, ended):
    if not started:
        return
    t0 = min(started.values())
    end = max(list(ended.values()) + list(started.values()))
    window = datetime.timedelta(seconds=600)
    a = t0
    while a < end:
        z = a + window
        busy = sum(max(0.0, (min(ended.get(n, end), z) - max(s, a)).total_seconds()) for n, s in started.items())
        print(f'  {int((a - t0).total_seconds() // 60):4d} min  {busy / 600:4.2f} at once')
        a = z
    running = [n for n in started if n not in ended]
    if running:
        print('  still running at the end:', ', '.join(running))


def main():
    runs = collections.OrderedDict(arg.split('=', 1) for arg in sys.argv[1:])
    data = {}
    for label, path in runs.items():
        seconds, started, ended = read(path)
        data[label] = seconds
        print(f'== {label}: {len(seconds)} suites finished, {sum(seconds.values()):.0f} test-seconds')
        lanes(started, ended)
    if len(runs) < 2:
        return
    labels = list(runs)
    first = data[labels[0]]
    for label in labels[1:]:
        both = [n for n in first if n in data[label]]
        ratio = sum(data[label][n] for n in both) / sum(first[n] for n in both)
        print(f'{label} / {labels[0]} on {len(both)} suites both finished: {ratio:.3f}')
    names = set().union(*data.values())
    print(f'{"suite":34s}' + ''.join(f'{l:>10s}' for l in labels))
    for n in sorted(names, key=lambda n: -max(d.get(n, 0) for d in data.values()))[:30]:
        print(f'{n:34s}' + ''.join(f'{data[l][n]:10.0f}' if n in data[l] else f'{"-":>10s}' for l in labels))


if __name__ == '__main__':
    main()
