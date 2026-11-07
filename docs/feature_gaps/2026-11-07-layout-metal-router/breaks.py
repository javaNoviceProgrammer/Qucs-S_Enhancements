#!/usr/bin/env python3
"""Breaks each piece of the metal router, one at a time; its tests must fail.

    python3 breaks.py <build tree> [name ...]

Each break is a text replaced in a source file. The tree is built with it and
test_layout_router (the engine's breaks) or test_layout_routing (the tools'
and the panel's) run, offscreen; a break that nothing fails on is said to have
survived. The sources are put back whatever happens.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "..", "qucs-s-26.1.1", "qucs")

ENGINE = "test_layout_router"
APP = "test_layout_routing"

BREAKS = [
    # The engine's lattice
    ("nothing fixed is ever too near", "layoutrouter/router_geometry.h", ENGINE,
     "    return dx * dx + dy * dy < double(s) * double(s);\n}",
     "    return false;\n}"),
    ("the obstacles rasterised without their clearance", "layoutrouter/router_engine.cpp", ENGINE,
     "raster(b.box, half, b.clearance, [&](int j, int i0, int i1) { bits.setRange(cellAt(i0, j), cellAt(i1, j)); });",
     "raster(b.box, half, 0, [&](int j, int i0, int i1) { bits.setRange(cellAt(i0, j), cellAt(i1, j)); });"),
    ("a square inside its own metal still tested whole", "layoutrouter/router_engine.cpp", ENGINE,
     "        for (const Box& o : own)\n            if (o.contains(s)) return true;",
     "        for (const Box& o : own)\n            if (false && o.contains(s)) return true;"),
    ("a via needs no landing", "layoutrouter/router_engine.cpp", ENGINE,
     "            if (a1 == Blocked || a2 == Blocked) continue;\n            // Not within",
     "            // Not within"),
    ("a keep-out lets every net in", "layoutrouter/router_engine.cpp", ENGINE,
     "                bool in = ko.except.contains(p.nets.at(n).name);",
     "                bool in = true;"),
    ("the start's tail not drawn", "layoutrouter/router_engine.cpp", ENGINE,
     "        if (k == 0) tail(st.terminals.front(), lp.front(), firstHorizontal, true);",
     ""),
    ("a guide's band not kept to", "layoutrouter/router_engine.cpp", ENGINE,
     "            if (!inBand(ni, nj)) continue;",
     ""),
    # Negotiation
    ("no present sharing cost", "layoutrouter/router_engine.cpp", ENGINE,
     "        return float((1.0 + h) * (1.0 + present * u));",
     "        return float(1.0 + h);"),
    ("contested nets' windows never grow", "layoutrouter/router_engine.cpp", ENGINE,
     "            ++st.grow;\n",
     ""),
    ("a stalled run goes on", "layoutrouter/router_engine.cpp", ENGINE,
     "        if (p.options.stallPasses > 0 && sinceBest >= p.options.stallPasses) {",
     "        if (false) {"),
    ("the best pass not kept", "layoutrouter/router_engine.cpp", ENGINE,
     "    if (best.pass > 0 && best.pass != int(result.passes.size())) {",
     "    if (false) {"),
    ("nets still too near are not routed again", "layoutrouter/router_engine.cpp", ENGINE,
     "    if (!contested.empty() && stopped != QStringLiteral(\"cancelled\")) {",
     "    if (false) {"),
    ("a kept route is never kept", "layoutrouter/router_engine.cpp", ENGINE,
     "        if (k && k->route.key == st.key && !asked &&",
     "        if (false && k && k->route.key == st.key && !asked &&"),
    ("a key blind to its terminals", "layoutrouter/router_engine.cpp", ENGINE,
     "    for (const Terminal& t : net.terminals) {\n        addInt(t.at.x);",
     "    for (const Terminal& t : net.terminals) {\n        continue;\n        addInt(t.at.x);"),
    ("the reason names nothing in the way", "layoutrouter/router_engine.cpp", ENGINE,
     "    if (!what.isEmpty()) text += QStringLiteral(\", stopped there by \") + what;",
     ""),
    ("prepare() says nothing of a terminal that cannot be left", "layoutrouter/router_engine.cpp", ENGINE,
     "            if (leg.cells.empty() && !p.nets.at(n).fixed) {",
     "            if (false) {"),
    ("no memory cap", "layoutrouter/router_engine.cpp", ENGINE,
     "    if (estimate > cap)\n",
     "    if (false)\n"),
    # The check on the shapes drawn
    ("nets' lines not checked against each other", "layoutrouter/router_verify.cpp", ENGINE,
     "                if (d >= double(need) - tol) return true;\n                const std::pair<int, int> key{std::min(pa.net, pb.net)",
     "                return true;\n                const std::pair<int, int> key{std::min(pa.net, pb.net)"),
    ("opens not found", "layoutrouter/router_verify.cpp", ENGINE,
     "            if (terminalNode[std::size_t(t)] >= 0 && terminalNode[0] >= 0 && find(terminalNode[std::size_t(t)]) == find(terminalNode[0])) continue;",
     "            continue;"),
    ("a net not routed is no finding", "layoutrouter/router_verify.cpp", ENGINE,
     "        if (r.status == NetResult::Status::Failed) {\n            Finding f;",
     "        if (false) {\n            Finding f;"),
    ("keep-outs not checked", "layoutrouter/router_verify.cpp", ENGINE,
     "                    if (area <= 0) continue;",
     "                    continue;"),
    ("cuts not checked", "layoutrouter/router_verify.cpp", ENGINE,
     "        if (need <= 0) continue;\n        std::vector<Box> all;",
     "        continue;\n        std::vector<Box> all;"),
    ("obstacles not checked", "layoutrouter/router_verify.cpp", ENGINE,
     "                    if (!bad) continue;",
     "                    continue;"),
    # Committing, reading, pairing
    ("a commit places the cell again", "layoutrouter/router_gds.cpp", ENGINE,
     "        if (!placed) {\n            auto* ref",
     "        if (true) {\n            auto* ref"),
    ("a commit keeps the old routes", "layoutrouter/router_gds.cpp", ENGINE,
     "        if (cell) {\n            empty(cell);",
     "        if (cell) {\n            ;"),
    ("an unknown key is taken", "layoutrouter/router_json.h", ENGINE,
     "        if (!known) {",
     "        if (false) {"),
    ("a pair kept by hand is not", "layoutrouter/router_assign.cpp", ENGINE,
     "        out[s] = p;\n        startTaken[s] = padTaken[p] = true;",
     ""),
    ("a CSV's headed rows add no pads", "layoutrouter/router_job.cpp", ENGINE,
     "                net->terminals.append(pad);",
     ""),
    # The tools and the panel
    ("route_commit ignores the findings", "qucscontrol_router.cpp", APP,
     "    if (!remove && !v.clean() && !args.value(QLatin1String(\"allow_findings\")).toBool()) {",
     "    if (false) {"),
    ("route_run's changed mode keeps nothing", "layoutrouting.cpp", APP,
     "    if (changedOnly || !which.isEmpty()) kept = keptRoutes();",
     ""),
    ("route_setup reads no labels", "layoutrouting.cpp", APP,
     "            if (f.isText()) labels[",
     "            if (false) labels["),
    ("the old route cell is routed around", "layoutrouting.cpp", APP,
     "        if (f.cell == routeCell || (routeCell >= 0 && f.via.contains(routeCell))) {",
     "        if (false) {"),
    ("the panel draws no routes", "layoutrouterpanel.cpp", APP,
     "    overlays.append(failed);\n    view->setOverlays(overlays);",
     "    view->setOverlays({});"),
    ("a pen ring is no keep-out", "layoutrouterpanel.cpp", APP,
     "        if (g == markup::Gesture::Ring) {",
     "        if (false) {"),
    ("a failed net not marked", "layoutrouterpanel.cpp", APP,
     "                failed.labels.append({QPointF(p.um(at.x), p.um(at.y)), t == 0 ? tr(\"%1 failed\")",
     "                failed.labels.append({QPointF(p.um(at.x), p.um(at.y)), t == 0 ? tr(\"%1\")"),
]


def run(build, wanted):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
    survived, caught = [], []
    for name, file, test, old, new in BREAKS:
        if wanted and name not in wanted:
            continue
        path = os.path.join(SOURCE, file)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if text.count(old) != 1:
            print(f"!! {name}: its text is in {file} {text.count(old)} times")
            survived.append(name + " (not applied)")
            continue
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text.replace(old, new))
            b = subprocess.run(["cmake", "--build", build, "--target", test], capture_output=True, text=True)
            if b.returncode != 0:
                print(f"-- {name}: does not build (caught by the compiler)")
                caught.append(name)
                continue
            t = subprocess.run([os.path.join(build, "qucs", "tests", test)], capture_output=True, text=True, env=env, timeout=1200)
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
    subprocess.run(["cmake", "--build", build, "--target", ENGINE, APP], capture_output=True)
    print(f"{len(caught)} caught, {len(survived)} survived" + (": " + "; ".join(survived) if survived else ""))
    return 1 if survived else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(run(sys.argv[1], set(sys.argv[2:])))
