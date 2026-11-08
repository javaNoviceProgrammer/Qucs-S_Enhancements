#!/usr/bin/env python3
"""Breaks each piece of the pen-markers follow-up, one at a time; its tests must fail.

    python3 breaks.py <build tree> [name ...]
    python3 breaks.py --check        (each break's text is found in its file, once)

Each break is a text replaced in a source file. The tree is built with it and
test_pen_reading run, offscreen; a break that nothing fails on is said to have
survived. The sources are put back whatever happens.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "..", "qucs-s-26.1.1", "qucs")
TEST = "test_pen_reading"

BREAKS = [
    # What a stroke is about (item 1)
    ("a large shape never set apart", "layoutstrokes.cpp",
     "        if (n.found.isText() || !n.box.contains(b) || n.box.width() * n.box.height() < 8 * area) continue;",
     "        continue;"),
    ("every large shape set apart, none kept in turn", "layoutstrokes.cpp",
     "        for (int kept = 0; kept <= large.size(); ++kept) {",
     "        for (int kept = 0; kept <= 0; ++kept) {"),
    ("a ring holds no part of a large shape", "layoutstrokes.cpp",
     "        if (const QJsonArray parts = partsWithin(layout, base, ring, apart, step); !parts.isEmpty()) o.insert(QStringLiteral(\"holds part of\"), parts);",
     ""),
    ("an edge preferred to a vertex as near", "layoutstrokes.cpp",
     "        const double allowance = l.halfWidth + close;",
     "        const double allowance = 0;"),
    ("a stroke across a shape follows it", "layoutstrokes.cpp",
     "        on[i] = best <= near && lengths > 0 && std::abs(dot(here, way)) >= std::cos(30 / Degrees) * lengths;",
     "        on[i] = best <= near;"),
    ("a detour is not following", "layoutstrokes.cpp",
     "    if (kept < (ons >= 2 ? 0.1 : 0.25) * total) return std::nullopt;",
     "    if (kept < 0.25 * total) return std::nullopt;"),
    ("an arrow's way said backwards", "layoutstrokes.cpp",
     "            if (const QString from = compassOf(tail, tip); !from.isEmpty()) o.insert(QStringLiteral(\"comes from\"), from);",
     "            if (const QString from = compassOf(tip, tail); !from.isEmpty()) o.insert(QStringLiteral(\"comes from\"), from);"),
    ("a polygon's corner snapped to before a path's end", "layoutstrokes.cpp",
     "            if ((pass == 1 && l.path) || (pass == 2 && !l.path))",
     "            if (pass == 1)"),
    # A freehand stroke as a polyline (item 2)
    ("no run snapped to what is near", "strokeshape.cpp",
     "        if (std::abs(s.value - value) <= reach && (best == nullptr || std::abs(s.value - value) < std::abs(best->value - value))) best = &s;",
     ""),
    ("a round value of any grid, however far", "strokeshape.cpp",
     "        if (std::abs(v - value) <= tolerance) {\n            if (step != nullptr) *step = s;",
     "        if (true) {\n            if (step != nullptr) *step = s;"),
    ("no corner rounded", "strokeshape.cpp",
     "        if (std::isfinite(nearest) && nearest > std::max(width, 2.5 * noise) && angle < 170) {",
     "        if (false) {"),
    ("a circle read as a polygon", "strokeshape.cpp",
     "    if (runs >= 3 && circleMiss(used) <= 1.2 * miss) return std::nullopt;",
     ""),
    ("no symmetry found", "strokeshape.cpp",
     "            if (within <= allowed && (out.axis.isEmpty() || within < out.within)) {",
     "            if (false) {"),
    ("ends judged as leniently as corners", "strokeshape.cpp",
     "                out.endsToo = std::hypot(2 * axis - along(a) - along(b), other(a) - other(b)) <= std::max({2 * width, 0.02 * size, 2 * within});",
     "                out.endsToo = std::hypot(2 * axis - along(a) - along(b), other(a) - other(b)) <= allowed;"),
    ("a hook where the pen lifted kept as a run", "strokeshape.cpp",
     "            } else if (lengthOf(segs.last()) < 2 * tolerance) {\n                segs.removeLast();\n                changed = true;\n            }",
     "            }"),
    # Points in the prompt, numbers, the pen (items 3 and 6)
    ("no points in the prompt", "markup.cpp",
     "    if (!has(\"polyline\") && !points.isEmpty() && points.size() <= MostPointsSaid) {",
     "    if (false) {"),
    ("a stroke numbered by its place", "markup.cpp",
     "    QJsonObject o{{QStringLiteral(\"stroke\"), stroke.id > 0 ? stroke.id : number},",
     "    QJsonObject o{{QStringLiteral(\"stroke\"), number},"),
    ("numbers given again after a stroke went", "markup.cpp",
     "    a_strokes.last().id = ++a_lastId[stroke.on];",
     "    a_strokes.last().id = int(a_strokes.size());"),
    ("the pen's state not said", "qucs.cpp",
     "      if (!told.isEmpty() && pen != nullptr && pen->isChecked())",
     "      if (false)"),
    ("show_layout's 'demo.gds' looked for in the project", "qucscontrol_layout.cpp",
     "    if (const QString given = file; !QFileInfo(absolute(given)).isFile()) {",
     "    if (const QString given = file; false) {"),
    ("a reload says nothing of what changed", "qucscontrol_layout.cpp",
     "        if (reload && beforeReload && beforeReload != layout) {",
     "        if (false) {"),
    # The overlay and addressed strokes (item 4)
    ("the overlay not drawn", "layoutdoc.cpp",
     "    paintSketch(p, c);\n",
     ""),
    ("an addressed stroke told in the prompt", "markup.cpp",
     "        if (!r.value(QStringLiteral(\"addressed\")).toBool()) {",
     "        if (true) {"),
    # (Not broken: an overlay item's unknown key. The tools' own check of the
    #  schema's items refuses it before sketchItemOf() sees it, so breaking
    #  sketchItemOf()'s check is caught by nothing - by design.)
    # edit_layout (item 5)
    ("replace drops a path's width", "layoutedit.cpp",
     "        double width = p->elements[0].half_width_and_offset.count > 0 ? 2 * p->elements[0].half_width_and_offset[0].u / a_e.scale : 0;",
     "        double width = 1;"),
    ("an edit written when a later one fails", "layoutedit.cpp",
     "        if (!edits.at(i).isObject() || !editor.apply(edits.at(i).toObject(), &done, &why)) {",
     "        if (!edits.at(i).isObject() || (!editor.apply(edits.at(i).toObject(), &done, &why) && i == 0)) {"),
    ("an array's element changed", "layoutedit.cpp",
     "        if (rep) {\n            *why = QObject::tr(\"%1 is repeated (an array)",
     "        if (false) {\n            *why = QObject::tr(\"%1 is repeated (an array)"),
    ("a cell placed in itself", "layoutedit.cpp",
     "            if (places(placed, c, seen)) {",
     "            if (false) {"),
    ("undo over a file changed since", "qucscontrol_layoutedit.cpp",
     "        if (fileHash(file) != last.value(QStringLiteral(\"after\")).toString().toLatin1())",
     "        if (false)"),
    ("what changed not counted", "layoutedit.cpp",
     "                const int more = int(it.value().size()) - int(b.value(it.key()).size());",
     "                const int more = 0;"),
]


def check():
    bad = 0
    for name, file, old, _ in BREAKS:
        text = open(os.path.join(SOURCE, file), encoding="utf-8").read()
        if text.count(old) != 1:
            print(f"!! {name}: found {text.count(old)} times in {file}")
            bad += 1
    print(f"{len(BREAKS)} breaks, {bad} not found once")
    return 1 if bad else 0


def run(build, only):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    caught, survived = [], []
    for name, file, old, new in BREAKS:
        if only and name not in only:
            continue
        path = os.path.join(SOURCE, file)
        text = open(path, encoding="utf-8").read()
        if text.count(old) != 1:
            print(f"?? {name}: its text is not in {file} once")
            survived.append(name)
            continue
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text.replace(old, new))
            b = subprocess.run(["cmake", "--build", build, "--target", TEST], capture_output=True, text=True)
            if b.returncode != 0:
                print(f"-- {name}: does not build (caught by the compiler)")
                caught.append(name)
                continue
            t = subprocess.run([os.path.join(build, "qucs", "tests", TEST)], capture_output=True, text=True, env=env, timeout=1200,
                               stdin=subprocess.DEVNULL)
            failing = [l for l in t.stdout.splitlines() if l.startswith("FAIL!")]
            if t.returncode != 0:
                print(f"ok {name}: {failing[0] if failing else 'exit ' + str(t.returncode)}")
                caught.append(name)
            else:
                print(f"!! {name}: SURVIVED")
                survived.append(name)
        finally:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
    subprocess.run(["cmake", "--build", build, "--target", TEST], capture_output=True)
    print(f"{len(caught)} caught, {len(survived)} survived" + (": " + "; ".join(survived) if survived else ""))
    return 1 if survived else 0


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "--check":
        sys.exit(check())
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(run(sys.argv[1], set(sys.argv[2:])))
