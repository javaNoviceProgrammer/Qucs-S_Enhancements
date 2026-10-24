#!/usr/bin/env python3
"""A schematic written in pieces, as an editor or a git checkout writes one:
the whole of it, then truncated at a fraction, for a tool call to meet."""
import sys
whole = open(sys.argv[1], encoding="utf-8").read()
open(sys.argv[2], "w", encoding="utf-8").write(whole[:int(len(whole) * float(sys.argv[3]))])
