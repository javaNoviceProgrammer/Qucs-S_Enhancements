#!/usr/bin/env python3
"""No list of one list written as QJsonArray{QJsonArray{...}}.

A braced list of one element of the class itself is a copy to some
compilers (Apple's newer clang, CI's macOS runner) and a nesting to others:
QJsonArray{QJsonArray{600, 300}} is [[600, 300]] on one and [600, 300] on
the other. A test of connect's 'via' passed here and failed on CI ("via[0]
is a list, not the number 600"), CI red for days. Built by append, the
list is one list: QJsonArray points; points.append(QJsonArray{600, 300}).

The one element may be a call too: QJsonArray{row({...})}, row() a
QJsonArray, was a row to CI's macOS and a list of one row here (an RF 2.5D
test's layer map, red from 11 November 2026). So a braced list whose one
element is a call of a function the sources declare as returning the list's
own class - or of toArray(), toStringList(), split(), keys(), toList() - is
said too. (A name the file declares itself is read by the file's own
declarations; any other only when every declaration of it in the sources
gives the one class: toJson() is a QJsonArray of problems and a QJsonObject
of everything else.) Wrap the element in its element type -
QJsonArray{QJsonValue(row({...}))} - or append it.

    python3 scripts/ci/check-json-nesting.py

Exits 1, and says where, when the sources hold one (comments aside).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
SOURCE = os.path.join(ROOT, "qucs-s-26.1.1")
TYPES = ("QJsonArray", "QVariantList", "QStringList")
# One inner list, nothing after it but the outer brace's end; in an
# expression, or as a variable's initializer - QJsonArray rows{QJsonArray{...}}
# or rows = {QJsonArray{...}}, which the first pattern let through (an RF 2.5D
# test's layer map, a row and no list of rows on CI's macOS: red from v27.0.8).
ONE = re.compile(r"\b(QJsonArray|QVariantList|QStringList)(?:\s+[A-Za-z_]\w*)?\s*(?:=\s*)?\{\s*\1\s*\{[^{}]*\}\s*\}")
# Qt's own calls that give one of them.
QT_RETURNS = {
    "QJsonArray": {"toArray"},
    "QStringList": {"toStringList", "split", "keys"},
    "QVariantList": {"toList"},
}
# Where a braced list of one of them opens: Type{, Type name{, Type name = {.
OPEN = re.compile(r"\b(QJsonArray|QVariantList|QStringList)(?:\s+[A-Za-z_]\w*)?\s*(?:=\s*)?\{")
# A call, the whole of the element: [object. or ->]name(...).
CALL = re.compile(r"^\s*(?:[A-Za-z_][\w:]*(?:\([^()]*\))?\s*(?:\.|->)\s*)*([A-Za-z_]\w*)\s*\($")


def code(line):
    """The line without a // comment (one in a string is rare enough here)."""
    at = line.find("//")
    return line if at < 0 else line[:at]


LITERAL = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'')


def blanked(text):
    """The text with string and character literals' insides blanked, so a
    brace or a comma in one is not read; lengths, and so lines, kept."""
    return LITERAL.sub(lambda m: m.group(0)[0] + " " * (len(m.group(0)) - 2) + m.group(0)[-1], text)


def single_call(text, at):
    """From just after an opening brace: the call that is its one element,
    and its name - or None when the list holds more, or something else."""
    depth, i, n = 0, at, len(text)
    while i < n:
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                if c != "}":
                    return None
                break
            depth -= 1
        elif c in ",;" and depth == 0:
            return None
        i += 1
    else:
        return None
    inner = text[at:i].strip()
    if not inner.endswith(")"):
        return None
    # The call's own parentheses: the last ")" back to its "(".
    depth, j = 0, len(inner) - 1
    while j >= 0:
        if inner[j] == ")":
            depth += 1
        elif inner[j] == "(":
            depth -= 1
            if depth == 0:
                break
        j -= 1
    m = CALL.match(inner[: j + 1]) if j > 0 else None
    return m.group(1) if m else None


sources = []
for root, _, files in os.walk(SOURCE):
    for name in files:
        if name.endswith((".cpp", ".h")):
            path = os.path.join(root, name)
            with open(path, encoding="utf-8", errors="replace") as f:
                sources.append((path, [code(line) for line in f.read().split("\n")]))
# What comes before a declared name: Type [&*] [Class::] - the type; a
# keyword before a call is none.
BEFORE = re.compile(r"\b([A-Za-z_][\w:]*(?:<[^;(){}]*>)?)\s*[&*]?\s+(?:[A-Za-z_]\w*::)*$")
NOT_TYPES = {"return", "else", "new", "delete", "case", "throw", "co_return", "co_yield", "typename", "const", "goto", "sizeof"}


def declared_as(text, name):
    """The types this text declares a function of this name as."""
    types = set()
    for m in re.finditer(re.escape(name) + r"\s*\(", text):
        if m.start() > 0 and (text[m.start() - 1].isalnum() or text[m.start() - 1] in "_:"):
            continue
        before = BEFORE.search(text, max(0, m.start() - 160), m.start())
        if before and before.group(1) not in NOT_TYPES:
            types.add(before.group(1))
    return types


texts = {path: "\n".join(lines) for path, lines in sources}
# The names any file declares as giving one of the lists: the only ones a
# call needs looking up for.
LISTS = re.compile(r"\b(?:QJsonArray|QVariantList|QStringList)\s*[&*]?\s+(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)\s*\(")
listy = {m.group(1) for text in texts.values() for m in LISTS.finditer(text)}
everywhere = {}


def gives(path, name, kind):
    """Whether a call of this name in this file gives a list of this kind."""
    if name in QT_RETURNS[kind]:
        return True
    if name not in listy:
        return False
    types = declared_as(texts[path], name)
    if not types:
        if name not in everywhere:
            everywhere[name] = set()
            for text in texts.values():
                if name in text:
                    everywhere[name] |= declared_as(text, name)
        types = everywhere[name]
    return types == {kind}


found = []
for path, lines in sources:
    where = os.path.relpath(path, ROOT)
    for number, line in enumerate(lines, 1):
        if ONE.search(line):
            found.append(f"{where}:{number}: {line.strip()}")
    if not any(t in texts[path] for t in TYPES):
        continue
    text = blanked(texts[path])
    for m in OPEN.finditer(text):
        name = single_call(text, m.end())
        if name is not None and gives(path, name, m.group(1)):
            number = text.count("\n", 0, m.start()) + 1
            found.append(f"{where}:{number}: {lines[number - 1].strip()}  ({name}() is a {m.group(1)})")
for f in sorted(set(found)):
    print(f)
print(f"{len(set(found))} lists of one list written as a braced copy" if found else "no list of one list written as a braced copy")
sys.exit(1 if found else 0)
