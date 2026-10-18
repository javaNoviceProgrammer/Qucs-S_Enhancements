#!/usr/bin/env python3
"""The GPU viewers' compiled shaders are those of their sources.

Each qucs-s-26.1.1/qucs/shaders/*.vert and *.frag has its .qsb beside it, and
manifest.sha256 holds the digests of the sources they were compiled from
(scripts/compile-shaders.sh writes both). A source changed without its .qsb
compiled again - or a .qsb missing, or not in qucs.qrc - fails here, in
seconds, rather than as a viewer drawing the old shader.
"""
import hashlib
import pathlib
import sys

here = pathlib.Path(__file__).resolve().parents[2] / "qucs-s-26.1.1" / "qucs" / "shaders"
manifest = {}
for line in (here / "manifest.sha256").read_text().splitlines():
    digest, name = line.split(maxsplit=1)
    manifest[name.strip()] = digest
problems = []
sources = sorted(list(here.glob("*.vert")) + list(here.glob("*.frag")))
for src in sources:
    digest = hashlib.sha256(src.read_bytes()).hexdigest()
    if manifest.get(src.name) != digest:
        problems.append(f"{src.name}: changed since it was compiled")
    if not (here / (src.name + ".qsb")).is_file():
        problems.append(f"{src.name}.qsb: missing")
for name in manifest:
    if not (here / name).is_file():
        problems.append(f"{name}: in the manifest, not in the folder")
# Each in the program's resources (qucs.qrc), where rhicanvas.cpp reads it.
qrc = (here.parent / "qucs.qrc").read_text()
for src in sources:
    if f"<file>shaders/{src.name}.qsb</file>" not in qrc:
        problems.append(f"{src.name}.qsb: not in qucs.qrc")
if problems:
    print("Shaders not compiled (run scripts/compile-shaders.sh):")
    for p in problems:
        print("  " + p)
    sys.exit(1)
print(f"check-shaders: {len(sources)} shaders, each compiled")
