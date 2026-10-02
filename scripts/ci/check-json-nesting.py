#!/usr/bin/env python3
"""No list of one list written as QJsonArray{QJsonArray{...}}.

A braced list of one element of the class itself is a copy to some
compilers (Apple's newer clang, CI's macOS runner) and a nesting to others:
QJsonArray{QJsonArray{600, 300}} is [[600, 300]] on one and [600, 300] on
the other. A test of connect's 'via' passed here and failed on CI ("via[0]
is a list, not the number 600"), CI red for days. Built by append, the
list is one list: QJsonArray points; points.append(QJsonArray{600, 300}).

    python3 scripts/ci/check-json-nesting.py

Exits 1, and says where, when the sources hold one (comments aside).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "qucs-s-26.1.1")
# One inner list, nothing after it but the outer brace's end.
ONE = re.compile(r"\b(QJsonArray|QVariantList|QStringList)\s*\{\s*\1\s*\{[^{}]*\}\s*\}")


def code(line):
    """The line without a // comment (one in a string is rare enough here)."""
    at = line.find("//")
    return line if at < 0 else line[:at]


found = []
for root, _, files in os.walk(SOURCE):
    for name in files:
        if not name.endswith((".cpp", ".h")):
            continue
        path = os.path.join(root, name)
        with open(path, encoding="utf-8", errors="replace") as f:
            for number, line in enumerate(f, 1):
                if ONE.search(code(line)):
                    found.append(f"{os.path.relpath(path, os.path.join(HERE, '..', '..'))}:{number}: {line.strip()}")
for f in found:
    print(f)
print(f"{len(found)} lists of one list written as a braced copy" if found else "no list of one list written as a braced copy")
sys.exit(1 if found else 0)
