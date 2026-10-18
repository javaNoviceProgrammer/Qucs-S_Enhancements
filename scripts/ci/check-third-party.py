#!/usr/bin/env python3
"""No vendor's own project, script or data file, and no vendor's copyright
notice, in the repository.

Qucs-S reads and writes Lumerical's project and script formats and was
compared with Ansys's published examples (Lumerical's, Zemax's). Those
examples are Ansys's, under its terms: they stay in a private workspace,
never here. The files of those formats here are ours, written by the test
data's generator scripts (qucs/tests/data/lumerical_*.py); each is listed
below with its generator, which must still write it.

    python3 scripts/ci/check-third-party.py

Exits 1, and says what, when a file of a vendor's format is not one of ours
or a text file carries a vendor's copyright line - or when a string the
program shows (a C++ string literal, not a comment) or the README writes a
vendor's name as a possessive ("Lumerical's database"): the owner's
Trademark Usage Guidelines ask for a mark as an adjective before a noun
("the Lumerical database"), never possessive or plural.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DATA = "qucs-s-26.1.1/qucs/tests/data/"
# Lumerical's (DEVICE, MODE, FDTD, INTERCONNECT, data, scripts) and Zemax's
# (OpticStudio's lens, archive, glass catalog) formats.
VENDOR = (".ldev", ".lms", ".fsp", ".icp", ".lsf", ".ldf", ".zar", ".zmx", ".zos", ".zda", ".agf")
# Ours: each written by its generator.
OURS = {
    DATA + "lumerical_charge.ldev": DATA + "lumerical_projects.py",
    DATA + "lumerical_device.ldev": DATA + "lumerical_projects.py",
    DATA + "lumerical_heat.ldev": DATA + "lumerical_projects.py",
    DATA + "lumerical_damaged_dataspace.ldev": DATA + "lumerical_projects.py",
    DATA + "lumerical_mode.lms": DATA + "lumerical_projects.py",
    DATA + "lumerical_sample.ldev": DATA + "lumerical_sample.py",
    DATA + "lumerical_wurtzite.ldev": DATA + "lumerical_sample.py",
    DATA + "lumerical_sample.ldf": DATA + "lumerical_sample.py",
    DATA + "lumerical_mqw_offsets.ldev": DATA + "lumerical_sample.py",
    DATA + "lumerical_mqw_workfunctions.ldev": DATA + "lumerical_sample.py",
}
# A vendor's copyright line (its examples carry one); not a trademark notice.
NOTICE = re.compile(r"(copyright|\(c\)|©)[^\n]{0,40}\b(ansys|lumerical|zemax|synopsys)\b", re.IGNORECASE)
TEXT = (".cpp", ".h", ".py", ".md", ".txt", ".json", ".qfem", ".sch", ".cmake", ".sh", ".yml", ".html", ".csv", ".lsf")
POSSESSIVE = re.compile(r"\b(Lumerical|Ansys|ANSYS|Zemax|OpticStudio)'s\b")


def strings(text):
    """Each line's string literals (number, text), comments passed over."""
    block = False
    for number, line in enumerate(text.split("\n"), 1):
        j, out = 0, []
        while j < len(line):
            if block:
                k = line.find("*/", j)
                if k < 0:
                    break
                block, j = False, k + 2
            elif line.startswith("/*", j):
                block, j = True, j + 2
            elif line.startswith("//", j):
                break
            elif line[j] == '"':
                k = j + 1
                while k < len(line) and line[k] != '"':
                    k += 2 if line[k] == "\\" else 1
                out.append(line[j + 1:k])
                j = k + 1
            elif line[j] == "'" and j + 2 < len(line):
                # (a character literal: '"' is not a string's start)
                k = line.find("'", j + 1 + (line[j + 1] == "\\"))
                j = k + 1 if k > j else j + 1
            else:
                j += 1
        for t in out:
            yield number, t


def tracked():
    """The repository's files (all of them where git is not there)."""
    try:
        out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], capture_output=True, check=True).stdout
        return [p for p in out.decode("utf-8", "replace").split("\0") if p]
    except (OSError, subprocess.CalledProcessError):
        files = []
        for root, dirs, names in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d not in (".git", "build", "build-asan", "bin")]
            files += [os.path.relpath(os.path.join(root, n), ROOT).replace(os.sep, "/") for n in names]
        return files


found = []
for path in tracked():
    full = os.path.join(ROOT, path)
    if not os.path.isfile(full):
        continue
    if path.lower().endswith(VENDOR):
        generator = OURS.get(path)
        if generator is None:
            found.append(f"{path}: a file of a vendor's format, not one of the test data's (scripts/ci/check-third-party.py lists those)")
        else:
            with open(os.path.join(ROOT, generator), encoding="utf-8", errors="replace") as f:
                if f"'{os.path.basename(path)}'" not in f.read():
                    found.append(f"{path}: {generator} no longer writes it")
    if path.lower().endswith(TEXT) and not path.startswith("scripts/ci/check-third-party"):
        with open(full, encoding="utf-8", errors="replace") as f:
            text = f.read()
        for number, line in enumerate(text.split("\n"), 1):
            if NOTICE.search(line):
                found.append(f"{path}:{number}: a vendor's copyright line: {line.strip()[:120]}")
        said = []
        if path.startswith("qucs-s-26.1.1/qucs/") and path.endswith((".cpp", ".h")):
            said = [(n, t) for n, t in strings(text) if POSSESSIVE.search(t)]
        elif path == "README.md":
            said = [(n, line) for n, line in enumerate(text.split("\n"), 1) if POSSESSIVE.search(line)]
        for number, t in said:
            found.append(f"{path}:{number}: a vendor's name as a possessive ({POSSESSIVE.search(t).group(0)}) - "
                         f"as an adjective (\"the Lumerical database\"): {t.strip()[:100]}")
for f in found:
    print(f)
print(f"{len(found)} vendor files, notices or possessives" if found else "no vendor's file, copyright notice or possessive")
sys.exit(1 if found else 0)
