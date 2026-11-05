#!/usr/bin/env python3
"""breaks.py: RF 2.5D's export to and import from HFSS 3D Layout (4 November)
undone on purpose, one piece at a time - the stack and its materials, the shapes,
vias and nets, every kind of port, the sweep, the control file and the GDSII
file, the script's engine against its stand-in, the model carried in the script,
the edb.def reader's records and the project read as a model, the comparison with
a Touchstone file, the tools, the guard and the menu - each built and its tests
run. Sources are put back whatever happens. BRK_ONLY_CHECK=1 checks the anchors
and writes nothing; BRK_ONLY=3,7 runs those alone; HUNT_RUNS=<dir> for logs;
BRK_BUILD=<dir> the build (the checkout's build); BRK_SOURCE=<dir> a copy of
qucs-s-26.1.1 to break instead of the checkout's. Run as a script only: importing
it does nothing."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build")
A = os.environ.get("BRK_BUILD_ASAN", W + "/build-asan")   # (for what only the sanitizers see: UNDER_ASAN)
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-hfss-breaks")

H = "rf25/rf25_hfss.cpp"
I = "rf25/rf25_hfss_import.cpp"
E = "rf25/rf25_edb.cpp"
C = "qucscontrol_rf25.cpp"
G = "qucscontrol_resources.cpp"
U = "qucs_rf25.cpp"
S = "test_rf25_hfss"
MICRO = (S, "aMicrostripsScriptIsWhatTheGuideSays")
PLANES = (S, "aPlaneOrALidIsWhatGivesTheSameThere")
PLANE = (S, "aPlaneIsTheLayerThatCoversTheDesignWithNoVoid")
REFERENCE = (S, "aPortsReferenceIsWhereItReturnsThrough")
CLEAR = (S, "aViaClearsMetalOfAnotherNet")
NOUGHT = (S, "aSweepFromNoughtBeginsAtItsFirstStep")
FILLS = (S, "whatFillsAMetalAndWhatClosesTheStack")
PORTS = (S, "everyKindOfPort")
EDGE = (S, "anEdgeIsNumberedAsItsPolygonReadsBack")
KEY = (S, "aKeyholeIsAVoid")
VOIDNET = (S, "whatStandsInAVoidIsNoPartOfIt")
VIAS = (S, "viasAndNets")
CTRL = (S, "theControlFileIsWhatTheHelpSays")
CANNOT = (S, "whatCannotBeWrittenIsSaid")
ROUND = (S, "exportThenImportIsTheSameModel")
REFUSED = (S, "whatCannotBeImportedIsSaid")
CTRLREAD = (S, "aControlFileReadsAsAModel")
CTRLBACK = (S, "anExportsControlFileReadsBack")
PROJECT = (S, "aProjectReadsWithoutAnsysSoftware")
RUN = (S, "aRunOfSignalLayersTakesTurnsMetalAndVia")
FOOT = (S, "aFootprintIsNoDesign")
STOP = (S, "aRecordItDoesNotKnowStopsItAndIsSaid")
DAMAGE = (S, "aFileDamagedAnywhereIsNeverMisread")
AEDT = (S, "theAedtBesideIt")
LENGTHS = (S, "lengthsAsAProjectWritesThem")
COMPARE = (S, "aSolvesSBesideATouchstoneFile")
TOOLS = ("test_qucs_control", "anRf25ModelGoesToHfss3DLayoutAndBackThroughItsTools")
MENU = ("test_qucs_control", "theRf25MenuGoesToHfss3DLayoutAndBack")
GUARD = ("test_qucs_control", "guardsAskAboutCommandsAndFilesOutside")

BREAKS = [
 # ---- The stack
 ("a metal takes no room in the stack",
  [(H, "            z += l.thickness;\n            layers << m;", "            layers << m;")], [FILLS]),
 ("a dielectric takes no room in the stack",
  [(H, "            materialOf.insert(i, d.material);\n            z += l.thickness;", "            materialOf.insert(i, d.material);")], [MICRO, FILLS]),
 ("a metal's fill always the air",
  [(H, "if (l.signal && fillFrom.contains(l.name)) l.fill = materialOf.value(fillFrom.value(l.name), air);", "if (false) l.fill = air;")], [FILLS]),
 ("a perfect plane of copper's conductivity",
  [(H, "                g.material = addMaterial(g.name, 1, 0, 1e30);", "                g.material = addMaterial(g.name, 1, 0, 5.8e7);")], [MICRO]),
 ("a metal plane one skin depth thick",
  [(H, "g.thickness = 5 * delta;", "g.thickness = delta;")], [PLANES]),
 ("an impedance plane's sheet of the wrong conductivity",
  [(H, "1 / (l.sigma * g.thickness)", "1 / l.sigma")], [PLANES]),
 ("a metal lid not written",
  [(H, "else if (l.kind == Layer::Cover && l.variant == QLatin1String(\"perfect\")) {", "else if (false) {")], [PLANES]),
 ("the margin one height, not five",
  [(H, "options.margin : 5 * std::max(height, 1e-9)", "options.margin : std::max(height, 1e-9)")], [MICRO]),
 ("the air a half wave, not a quarter",
  [(H, "options.air : kC0 / out.adaptive / 4", "options.air : kC0 / out.adaptive / 2")], [MICRO]),
 # ---- Shapes, vias, nets
 ("a keyhole left in the outline",
  [(H, "        openKeyholes(c.outline, &outline, &p.holes);", "        outline = c.outline;")], [KEY]),
 ("an outline as it comes, not counter-clockwise",
  [(H, "p.outline = counterClockwise(outline);", "p.outline = outline;")], [EDGE]),
 ("a via's upper and lower layers swapped",
  [(H, "        via.upper = upperName;\n        via.lower = lowerName;", "        via.upper = lowerName;\n        via.lower = upperName;")], [VIAS]),
 ("a rectangular via written as a circle",
  [(H, "        if (rectangle) {\n            ps.shape", "        if (false) {\n            ps.shape")], [VIAS]),
 ("a round via of its radius for its diameter",
  [(H, "num(2 * std::sqrt(area / M_PI))", "num(std::sqrt(area / M_PI))")], [VIAS]),
 ("a padstack made for each via",
  [(H, "if (QStringLiteral(\"%1 %2 %3 %4 %5\").arg(o.shape, o.sizes.join(QLatin1Char('x')), o.material, o.upper, o.lower) == key) found = i;", "if (false) found = i;")], [VIAS]),
 ("shapes that touch not one net",
  [(H, "                sets.join(a, b);", "                {}")], [VIAS]),
 ("a via not joining its metals' nets",
  [(H, "if (overlapping(polygons.at(a).outline, boxes.at(a), shape, bound) && !inAHole(polygons.at(a).holes, shape)) sets.join(a, nPoly + v);", "if (false) sets.join(a, nPoly + v);")], [VIAS]),
 ("what stands in a void one net with the shape round it",
  [(H, "\n                && !inAHole(polygons.at(a).holes, polygons.at(b).outline) && !inAHole(polygons.at(b).holes, polygons.at(a).outline))", ")")], [VOIDNET]),
 ("a via in a void one net with the shape round it",
  [(H, "boxes.at(a), shape, bound) && !inAHole(polygons.at(a).holes, shape)) sets.join", "boxes.at(a), shape, bound)) sets.join")], [VOIDNET]),
 ("a chain of keyholes opened at its first slit alone",
  [(H, "if (part.size() >= 3 && std::abs(signedArea(part)) > 0) open << part;", "if (part.size() >= 3 && std::abs(signedArea(part)) > 0) loops << part;")], [KEY]),
 ("a lobe on a seam taken for a hole",
  [(H, "if (k != largest && (signedArea(loops.at(k)) > 0) == (signedArea(loops.at(largest)) > 0)) {", "if (false) {")], [KEY]),
 ("a via to the ground not of the ground's net",
  [(H, "if (via.lower == groundLayer || via.upper == groundLayer) sets.join(nPoly + v, groundSet);", "if (false) sets.join(nPoly + v, groundSet);")], [VIAS]),
 # ---- Ports
 ("a coplanar port's references not written",
  [(H, "for (const QLineF& stretch : beside) references += edgesOn(stretch, onLayer);", "for (const QLineF& stretch : beside) (void)stretch;")], [FILLS]),
 ("auto: every edge port a wave port",
  [(H, "(options.portType == QLatin1String(\"auto\") && (calibrate || deembed != 0))", "options.portType == QLatin1String(\"auto\")")], [MICRO, TOOLS]),
 ("auto: no edge port a wave port",
  [(H, "(options.portType == QLatin1String(\"auto\") && (calibrate || deembed != 0))", "false")], [PORTS]),
 ("port_type wave taken as auto",
  [(H, "const bool wave = options.portType == QLatin1String(\"wave\")\n", "const bool wave = false\n")], [TOOLS]),
 ("named returns an edge port, not a circuit port",
  [(H, "                kind = QStringLiteral(\"circuit\");\n                sayIt = tr(\"%1: circuit port on an edge of %2", "                kind = QStringLiteral(\"edge\");\n                sayIt = tr(\"%1: circuit port on an edge of %2")], [PORTS]),
 ("a named return with no edge dropped in silence",
  [(H, "                    if (other != nullptr && edgeOfTerminal(*other, &e)) references << e;\n                    else out.notes << tr(\"%1: its return %2 has no edge to reference.\").arg(own, r);\n                }\n            }\n            if (named) {",
       "                    if (other != nullptr && edgeOfTerminal(*other, &e)) references << e;\n                }\n            }\n            if (named) {")], [CANNOT]),
 ("the port solver on for a bare gap port",
  [(H, "solver = calibrate || wave ? QStringLiteral(\"true\") : QStringLiteral(\"false\");", "solver = QStringLiteral(\"true\");")], [MICRO]),
 ("a point port against its own metal",
  [(H, "negative = lower != nullptr ? lower->name : groundLayer;", "negative = positive;")], [PORTS]),
 ("a port on no edge not said",
  [(H, "out.notes << tr(\"%1 is not written: no edge of a shape on %2 near its point faces the way it says.\").arg(own, metal);\n                continue;", "continue;")], [CANNOT]),
 ("no port written, and no refusal",
  [(H, "out.problems << tr(\"No port could be written: the model's ports are on no edge or metal that is there.\");\n        return out;", "return out;")], [CANNOT]),
 # ---- The sweep, the control file, the GDSII file
 ("a LINC of steps, not of points",
  [(H, ".arg(num(sweep.first()), num(sweep.last())).arg(sweep.size());", ".arg(num(sweep.first()), num(sweep.last())).arg(sweep.size() - 1);")], [MICRO]),
 ("a log sweep written as a LINC",
  [(H, "    } else if (logarithmic) {\n        QStringList each;", "    } else if (false) {\n        QStringList each;")], [PLANES]),
 ("a discrete sweep still interpolating",
  [(H, "py(options.discrete ? QStringLiteral(\"kDiscrete\") : QStringLiteral(\"kInterpolating\"))", "py(QStringLiteral(\"kInterpolating\"))")], [TOOLS]),
 ("the control file's step a point short",
  [(H, "num((sweep.last() - sweep.first()) / (sweep.size() - 1))", "num((sweep.last() - sweep.first()) / sweep.size())")], [CTRL]),
 ("the control file's second layer the first's",
  [(H, "layer2 = polygons.at(references.first().polygon).layer;", "layer2 = layer1;\n                x2 = x1;")], [CTRL]),
 ("the control file's port impedance always 50",
  [(H, "xml(layer2), num(impedance));", "xml(layer2), QStringLiteral(\"50\"));")], [CTRL]),
 ("the plane left out of the GDSII file",
  [(H, "if (plane) out.gdsShapes << qMakePair(gdsLayer.value(groundLayer), outerPolygon);", "if (false) out.gdsShapes << qMakePair(gdsLayer.value(groundLayer), outerPolygon);")], [CTRL, CTRLBACK]),
 ("a via's shapes on the first metal's GDSII layer",
  [(H, "out.gdsShapes << qMakePair(viaGds.value(c.layer), c.outline);", "out.gdsShapes << qMakePair(1, c.outline);")], [VIAS]),
 ("the GDSII file's unit a nanometre, not a micrometre",
  [(H, "library.init(libraryName.isEmpty() ? \"RF25\" : libraryName.constData(), 1e-6, 1e-9);", "library.init(libraryName.isEmpty() ? \"RF25\" : libraryName.constData(), 1e-9, 1e-9);")], [ROUND]),
 ("the export writes no control file",
  [(H, "    if (!put(stem + QStringLiteral(\".xml\"), written.control)) return false;\n", "")], [TOOLS]),
 # ---- The script's engine, against the stand-in
 ("an edge as it was written, whatever reads back",
  [(H, "                say(\"%s: its edge %d is read back as %d\" % (primitive, number, k))\n            return k", "                say(\"%s: its edge %d is read back as %d\" % (primitive, number, k))\n            return number")], [EDGE]),
 ("no edge where it was written not said",
  [(H, "    say(\"%s: no edge of it reads back where edge %d was written\" % (primitive, number), 1)\n", "")], [EDGE]),
 ("a layer's elevation not given",
  [(H, "\"LowerElevation:=\", length(elevation),", "\"LowerElevation:=\", length(0),")], [MICRO]),
 ("a signal layer's fill not given",
  [(H, "        sublayer += [\"FillMaterial:=\", fill]\n", "        pass\n")], [MICRO, FILLS]),
 ("a material's conductivity not given",
  [(H, "\"conductivity:=\", str(conductivity),", "\"conductivity:=\", \"0\",")], [MICRO]),
 ("a void drawn as its owner's outline",
  [(H, "geometry(\"%s_void%d\" % (name, k), layer, void)", "geometry(\"%s_void%d\" % (name, k), layer, points)")], [KEY]),
 ("a via's highest and lowest layers swapped by the engine",
  [(H, "\"highest_layer:=\", upper,\n             \"lowest_layer:=\", lower])", "\"highest_layer:=\", lower,\n             \"lowest_layer:=\", upper])")], [VIAS]),
 ("nets not set",
  [(H, "[\"NAME:ChangedProps\", [\"NAME:Net\", \"Value:=\", net]]", "[\"NAME:ChangedProps\", [\"NAME:Layer\", \"Value:=\", net]]")], [VIAS]),
 ("a port's name left as Electronics Desktop gave it",
  [(H, "            oExcitations.Rename(name, port[\"name\"])\n            name = port[\"name\"]\n", "            pass\n")], [MICRO, PORTS]),
 ("a circuit port's returns added as an edge port's",
  [(H, "attempt(name + \": its returns\", oEditor.AddCircuitRefPort, [name], edges_of(port[\"references\"]))", "attempt(name + \": its returns\", oEditor.AddRefPort, [name], edges_of(port[\"references\"]))")], [PORTS]),
 ("an edge port's returns not added",
  [(H, "attempt(name + \": its returns\", oEditor.AddRefPort, [name], edges_of(port[\"references\"]))", "pass")], [FILLS]),
 ("every port into 50 Ohm",
  [(H, "\"%.12gohm + 0i ohm\" % port[\"impedance\"])", "\"50ohm + 0i ohm\")")], [PORTS]),
 ("a de-embedding not set",
  [(H, "        set_property(name, \"Deembed Distance\", length(port[\"deembed\"]))\n", "        pass\n")], [PORTS]),
 ("the plane's truncation not said",
  [(H, "\"TruncAtGnd:=\", TRUNCATE_AT_GROUND,", "\"TruncAtGnd:=\", False,")], [MICRO]),
 ("the adaptive frequency not given",
  [(H, "\"AdaptiveFrequency:=\", ADAPTIVE,", "\"AdaptiveFrequency:=\", \"1GHz\",")], [FILLS]),
 ("the sweep's data not given",
  [(H, "\"Variable:=\", SWEEP, \"Data:=\", SWEEP_DATA,", "\"Variable:=\", SWEEP, \"Data:=\", \"\",")], [MICRO]),
 ("the project not saved beside the script",
  [(H, "    oProject.SaveAs(project, True, \"ef_overwrite\", [\"NAME:OverrideActions\"])\n", "    pass\n")], [MICRO]),
 # ---- The model carried in the script
 ("no #qucs-s line in the script",
  [(H, "s += QStringLiteral(\"#qucs-s %1\\n\").arg(QString::fromUtf8(QJsonDocument(carried).toJson(QJsonDocument::Compact)));", "(void)carried;")], [ROUND, TOOLS]),
 ("the model's folder not kept in the script",
  [(H, "if (!options.modelFolder.isEmpty()) carried.insert(QStringLiteral(\"folder\"), options.modelFolder);", "")], [ROUND]),
 ("a script of another format read as this one",
  [(I, "if (carried.value(QLatin1String(\"format\")).toString() != QLatin1String(\"qucs-s rf 2.5d hfss 1\")) {", "if (false) {")], [REFUSED]),
 ("a script with no line read as nothing, not refused",
  [(I, "    if (line.isEmpty()) {\n        if (error != nullptr)\n            *error = tr(\"%1 has no \\\"#qucs-s\\\" line", "    if (false) {\n        if (error != nullptr)\n            *error = tr(\"%1 has no \\\"#qucs-s\\\" line")], [REFUSED]),
 ("a moved script's layout not looked for in the model's folder",
  [(I, "            there = QDir(folder).absoluteFilePath(named);", "            there = folder + QStringLiteral(\"/nothing\");")], [ROUND]),
 ("the shapes in the script not read",
  [(I, "                out->shapes << qMakePair(shape.value(QLatin1String(\"gds\")).toInt(), polygon);", "                (void)polygon;")], [ROUND, TOOLS]),
 ("shapes read back processed again",
  [(I, "                processing->props.insert(QLatin1String(off), false);", "                (void)off;")], [ROUND]),
 ("a script import's counts left at nought",
  [(I, "            (isVia.value(gds) ? out->vias : out->polygons) += 1;", "            (void)gds;")], [ROUND]),
 # ---- The edb.def reader
 ("a footprint taken as a design",
  [(E, "const int worth = footprint ? -1 : shapes;", "const int worth = shapes; (void)footprint;")], [FOOT]),
 ("the cell asked for not taken",
  [(E, "if (!wanted.isEmpty() && cells.at(k).name == wanted) {", "if (false) {")], [FOOT]),
 ("a rectangle by its centre read by corners",
  [(E, "QRectF box = rep == 1 ? QRectF(a - cc / 2, b - d / 2, cc, d) : QRectF(QPointF(a, b), QPointF(cc, d)).normalized();", "QRectF box = QRectF(QPointF(a, b), QPointF(cc, d)).normalized(); (void)rep;")], [RUN]),
 ("a rectangle's rotation ignored",
  [(E, "                if (angle != 0) {\n                    const QPointF centre = box.center();", "                if (false) {\n                    const QPointF centre = box.center();")], [RUN]),
 ("rounded corners said for each",
  [(E, "r.at(6).number != 0 && !out->notes.contains(note)) out->notes << note;", "r.at(6).number != 0) out->notes << note;")], [RUN]),
 ("a circle of its radius for its diameter",
  [(E, "const double radius = r.at(3).number * 1e6;", "const double radius = r.at(3).number * 1e6 / 2;")], [RUN]),
 ("a path of half its width",
  [(E, "    const double half = width / 2;\n    QList<QPolygonF> pieces;", "    const double half = width / 4;\n    QList<QPolygonF> pieces;")], [PROJECT]),
 ("an arc bowed the other way",
  [(E, "    const double side = height > 0 ? 1 : -1;\n", "    const double side = height > 0 ? -1 : 1;\n")], [PROJECT]),
 ("an arc's point not taken: its chord",
  [(E, "                arcPoints(out.last(), next, x * 1e6, &out);", "                (void)next;")], [PROJECT]),
 ("a void's owner not read",
  [(E, "s.owner = c11->at(4).integer;", "s.owner = -1;")], [PROJECT]),
 ("a via's place in the wrong unit",
  [(E, "v.at = QPointF(r.at(2).number * 1e6, r.at(3).number * 1e6);", "v.at = QPointF(r.at(2).number, r.at(3).number);")], [PROJECT]),
 ("a terminal's de-embedding not read",
  [(E, "lengthOf(quoted(excitation, QStringLiteral(\"DoDeembedLength\")), out->variables, &p.deembed);", "(void)excitation;")], [PROJECT]),
 ("an unknown record passed over, not stopping it",
  [(E, "            *why = s.why;\n            return false;", "            *why = s.why;\n            return true;")], [STOP]),
 ("an encrypted file read on",
  [(E, "QStringLiteral(\"Encrypted=(true|false)\")", "QStringLiteral(\"Encryptd=(true|false)\")")], [STOP]),
 ("a mil of a millimetre",
  [(E, "else if (u == QLatin1String(\"mil\")) *scale = 25.4e-6;", "else if (u == QLatin1String(\"mil\")) *scale = 1e-3;")], [LENGTHS]),
 ("a shape of no numbers handed on",
  [(E, "if (!sensible(q)) throw Stopped{tr(\"a shape at (%1, %2) µm\").arg(q.x()).arg(q.y())};", "(void)q;")], [DAMAGE]),
 ("a via of no numbers handed on",
  [(E, "if (!sensible(v.at)) throw Stopped{tr(\"a via at (%1, %2) µm\").arg(v.at.x()).arg(v.at.y())};", "")], [DAMAGE]),
 ("the end of the file not judged",
  [(E, "if (at < 0 || n > a_size - at) throw Stopped{tr(\"the end of the file at %1\").arg(at)};", "(void)n;")], [DAMAGE]),
 ("a string's length not judged",
  [(E, "if (n < 0 || n > a_size - at) throw Stopped{tr(\"a string of %1 bytes at %2\").arg(n).arg(at - 4)};", "")], [DAMAGE]),
 # ---- The project as a model
 ("a metal between two via layers taken as a via",
  [(I, "if (inRun % 2 == 1 && k + 1 < signalLayers.size() && touch(k)) out.insert(signalLayers.at(k).name);", "if (inRun > 0 && k + 1 < signalLayers.size() && touch(k)) out.insert(signalLayers.at(k).name);")], [RUN]),
 ("no layer taken as a via",
  [(I, "if (inRun % 2 == 1 && k + 1 < signalLayers.size() && touch(k)) out.insert(signalLayers.at(k).name);", "if (false) out.insert(signalLayers.at(k).name);")], [RUN]),
 ("a via layer of no height after the metal on it",
  [(I, "return a.elevation != b.elevation ? a.elevation < b.elevation : a.thickness < b.thickness;", "return a.elevation < b.elevation;")], [RUN]),
 ("a via layer's fill on top of the dielectric round it",
  [(I, "if (v.thickness > 0 && !v.fill.isEmpty() && !taken) slabs", "if (v.thickness > 0 && !v.fill.isEmpty()) slabs")], [RUN]),
 ("the dielectric not cut by the metals in it",
  [(I, "if (m.elevation < z1 - 1e-13 && m.elevation + m.thickness > z0 + 1e-13) room", "if (false) room")], [RUN]),
 ("overlapping layers not said",
  [(I, "if (item.z < z - 1e-12) overlapping << (item.metal ? metals.at(item.index).name : slabs.at(item.index).name);", "")], [RUN]),
 ("a plane taken though it has voids",
  [(I, "if (!voids && covered >= 0.9 * all.width() * all.height()) plane = lowest.name;", "if (covered >= 0.9 * all.width() * all.height()) plane = lowest.name;")], [PLANE]),
 ("a plane needs no covering",
  [(I, "if (!voids && covered >= 0.9 * all.width() * all.height()) plane = lowest.name;", "if (!voids) plane = lowest.name;")], [PROJECT]),
 ("a void not cut from its shape",
  [(I, "        for (const int v : voidsOf.value(s.id)) holes << design.shapes.at(v).outline;\n", "")], [PROJECT]),
 ("a via across metals only between the first two",
  [(I, "        for (int k = low; k < high; ++k) {\n            out->shapes << qMakePair(gdsOf.value(viaBetween(stack.metals.at(k), stack.metals.at(k + 1), sigma)), hole);", "        for (int k = low; k < std::min(high, low + 1); ++k) {\n            out->shapes << qMakePair(gdsOf.value(viaBetween(stack.metals.at(k), stack.metals.at(k + 1), sigma)), hole);")], [RUN]),
 ("an anti-pad not cut",
  [(I, "                    clearedFrom[s.id] << clearance;\n", "")], [CLEAR]),
 ("an anti-pad cut from metal of the via's own net",
  [(I, "s.layer == layer.id && s.net != v.net && s.outline", "s.layer == layer.id && s.outline")], [CLEAR]),
 ("no pad where a via passes a layer",
  [(I, "if (!landing.isEmpty()) padsOn[layer.name] << landing;", "")], [CLEAR, PROJECT]),
 ("a padstack's own layer names read from the bottom",
  [(I, "here = pad.onLayers.at(high - k);", "here = pad.onLayers.at(k - low);")], [CLEAR]),
 ("a padstack's own layer names not read at all",
  [(I, "indexOfLayer(pad.onLayers.first().layer) < 0) here = pad.onLayers.at(high - k);", "false) here = pad.onLayers.at(high - k);")], [CLEAR]),
 ("a plane taken though a via clears it",
  [(I, "voids = voids || voidsOf.contains(s.id) || clearedFrom.contains(s.id);", "voids = voids || voidsOf.contains(s.id);")], [CLEAR]),
 ("a pad it cannot draw not said",
  [(I, "if (landing.isEmpty() && !here.padShape.isEmpty() && here.padShape != QLatin1String(\"No\")) ++padsLeftOut;", "")], [CLEAR]),
 ("a circular pad inscribed, not of its area",
  [(I, "const double radius = sizes.first() * 1e6 / 2 * std::sqrt(2 * M_PI / (kSides * std::sin(2 * M_PI / kSides)));", "const double radius = sizes.first() * 1e6 / 2;")], [CLEAR]),
 ("a rectangular pad not turned with its via",
  [(I, "out << at + QPointF(corner.x() * std::cos(rotation) - corner.y() * std::sin(rotation), corner.x() * std::sin(rotation) + corner.y() * std::cos(rotation));", "out << at + corner;")], [CLEAR]),
 ("a padstack's anti-pad not read",
  [(E, "shapeOf(lgm, \"ant\", &one.antiShape, &one.antiSizes);", "one.antiShape = QStringLiteral(\"No\");")], [CLEAR]),
 ("a barrel of the number the via keeps, not its padstack's hole",
  [(I, "const double width = pad.sizes.value(0) > 0 ? pad.sizes.first() : v.hole;", "const double width = v.hole > 0 ? v.hole : pad.sizes.value(0);")], [CLEAR]),
 ("an edge's shape read from the field before it",
  [(E, "p.primitive = edge.at(4).integer;", "p.primitive = edge.at(3).integer;")], [PROJECT]),
 ("a via's rotation not read",
  [(E, "            v.rotation = r.at(4).number;\n", "")], [CLEAR]),
 ("a port's reference on a metal sent to the ground",
  [(I, "const bool throughTerminal = !beside && !returnMetal.isEmpty();", "const bool throughTerminal = false;")], [REFERENCE]),
 ("a port through a terminal still calibrated",
  [(I, "if (type == QLatin1String(\"Wave\") && !throughTerminal) n.props", "if (type == QLatin1String(\"Wave\")) n.props")], [REFERENCE]),
 ("a reference's terminal on the port's own metal",
  [(I, "terminal.props.insert(QStringLiteral(\"metal\"), returnMetal);", "terminal.props.insert(QStringLiteral(\"metal\"), metal);")], [REFERENCE]),
 ("a reference's terminal not under its port",
  [(I, "terminal.props.insert(QStringLiteral(\"y\"), lengthText(p.at.y() * 1e-6));", "terminal.props.insert(QStringLiteral(\"y\"), lengthText(0.5));")], [REFERENCE]),
 ("a reference on its own layer not beside it",
  [(I, "const bool beside = referenceLayer == on.name;", "const bool beside = false;")], [REFERENCE]),
 ("the sweep's points not read",
  [(I, "    study->props.insert(QStringLiteral(\"points\"), points);", "    (void)points;")], [PROJECT]),
 ("a sweep from nought handed to the solver",
  [(I, "    if (from == 0) {\n", "    if (false) {\n")], [NOUGHT]),
 # ---- Comparing S
 ("S compared at the solve's references, not the file's",
  [(H, "out.ours << scatteringOf(solved.z.at(k), references);", "out.ours << scatteringOf(solved.z.at(k), solved.references);")], [COMPARE]),
 ("the file's points read in order whatever is asked",
  [(H, "const QString key = QStringLiteral(\"%1,%2\").arg(map.at(i)).arg(map.at(j));", "const QString key = QStringLiteral(\"%1,%2\").arg(i + 1).arg(j + 1);")], [COMPARE]),
 ("between a file's points in real and imaginary parts",
  [(H, "return std::polar(std::abs(a) + t * (std::abs(b) - std::abs(a)), std::arg(a) + t * turn);", "return a + t * (b - a); (void)turn;")], [COMPARE]),
 ("frequencies outside the file's read at its ends",
  [(H, "if (f < frequency.first() * (1 - kSame) || f > frequency.last() * (1 + kSame)) continue;", "")], [COMPARE]),
 ("no frequency in common not refused",
  [(H, "out.problem = tr(\"The file's frequencies (%1 to %2 Hz) and the solve's do not meet.\").arg(frequency.first()).arg(frequency.last());\n        return out;", "return out;")], [COMPARE]),
 ("Y parameters compared as S",
  [(H, "    if (parameter != QLatin1String(\"S\")) {\n", "    if (false) {\n")], [COMPARE]),
 ("a term's worst the first, not the worst",
  [(H, "                if (apart >= t.worst) {\n                    t.worst = apart;", "                if (t.worst == 0) {\n                    t.worst = apart;")], [COMPARE]),
 ("rms of nothing",
  [(H, "t.rms = std::sqrt(sum / double(out.frequencies.size()));", "t.rms = sum;")], [COMPARE]),
 # ---- The tools, the guard, the menu
 ("over the model's own layout",
  [(C, "if (!layoutName.isEmpty() && sameFile(dir.absoluteFilePath(layoutName), stem + QStringLiteral(\".gds\")))", "if (false)")], [TOOLS]),
 ("over a .gds or .xml no export wrote",
  [(C, "    if (!QFileInfo::exists(where))\n        for (const QString& file : {stem + QStringLiteral(\".xml\"), stem + QStringLiteral(\".gds\")})", "    if (false)\n        for (const QString& file : {stem + QStringLiteral(\".xml\"), stem + QStringLiteral(\".gds\")})")], [TOOLS]),
 ("a model written over untold",
  [(C, "        if (!file.isEmpty() && QFileInfo::exists(file) && !replace)\n", "        if (false)\n")], [TOOLS]),
 ("a model written over while it is open",
  [(C, "if (!file.isEmpty() && sameFile(d->getDocName(), file)) return errorResult(tr(\"%1 is open: close it first.\").arg(QFileInfo(file).fileName()));", "(void)d;")], [TOOLS]),
 ("port_type not checked",
  [(C, "if (args.contains(QLatin1String(\"port_type\"))) how.portType = args.value(QLatin1String(\"port_type\")).toString().trimmed();", "if (args.contains(QLatin1String(\"port_type\"))) how.portType = QStringLiteral(\"auto\");")], [TOOLS]),
 ("'adaptive_frequency' not used",
  [(C, "        how.adaptive = *given;\n", "")], [TOOLS]),
 ("'thin_metal' not used",
  [(C, "!lengthIn(args.value(QLatin1String(\"thin_metal\")), &how.thinMetal))", "false)")], [TOOLS]),
 ("'max_passes' beyond its range",
  [(C, "if (how.maxPasses < 1 || how.maxPasses > 1000) return errorResult(tr(\"'max_passes' is a whole number from 1 to 1000.\"));", "")], [TOOLS]),
 ("compare with no solve not refused",
  [(C, "    if (!wave || !wave->ok())\n        return errorResult(model->solveProblem().isEmpty() ? tr(\"It has no full-wave solve to compare: rf25_solve it first, or press Solve on its tab.\") : model->solveProblem());\n", "    if (!wave) return jsonResult(QJsonObject{});\n")], [TOOLS]),
 ("the guard: the export's .gds not named",
  [(G, "        write(stem + QStringLiteral(\".xml\"));\n        write(stem + QStringLiteral(\".gds\"));", "        write(stem + QStringLiteral(\".xml\"));")], [GUARD]),
 ("the guard: the import's .gds not named",
  [(G, "            write(QFileInfo(model).absoluteDir().filePath(QFileInfo(model).completeBaseName() + QStringLiteral(\".gds\")));\n", "")], [GUARD]),
 ("the guard: the import's model not named",
  [(G, "            write(model);\n            write(QFileInfo(model)", "            write(QFileInfo(model)")], [GUARD]),
 ("the menu: no name suggested beside the model",
  [(U, ": info.dir().filePath(info.completeBaseName() + QStringLiteral(\"_hfss.py\"));", ": info.dir().filePath(QStringLiteral(\"rf25_hfss.py\"));")], [MENU]),
 ("the menu: written over without asking",
  [(U, "if (QMessageBox::question(this, title, tr(\"%1 is there already: write over it?\").arg(QDir::toNativeSeparators(there))) != QMessageBox::Yes) return;\n", "")], [MENU]),
 ("the menu: told No, written over all the same",
  [(U, "!= QMessageBox::Yes) return;\n    replace = true;", "!= QMessageBox::Yes) {}\n    replace = true;")], [MENU]),
 ("the menu: the compare's table not written",
  [(U, ", {QStringLiteral(\"table\"), info.dir().filePath(info.completeBaseName() + QStringLiteral(\"_compared.csv\"))}", "")], [MENU]),
]


# Reading past the end of the buffer is no change a test in Release can see.
UNDER_ASAN = {"the end of the file not judged"}


def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if back is None:
            continue
        if p not in back:
            back[p] = s
        open(p, "w", encoding="utf-8").write(s.replace(old, new))


def main():
    os.makedirs(os.path.join(RUNS, "cache"), exist_ok=True)
    if os.environ.get("BRK_ONLY_CHECK"):
        for i, (name, edits, tests) in enumerate(BREAKS, 1):
            apply(edits, None)
        print("%d breaks, every anchor once" % len(BREAKS))
        return 0
    which = {int(n) for n in os.environ.get("BRK_ONLY", "").split(",") if n.strip()}
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
        if which and i not in which:
            continue
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            log = os.path.join(RUNS, "brk%03d.log" % i)
            suites = sorted({t[0] for t in tests})
            targets = ["qucs/tests/" + s for s in suites]
            build = A if name in UNDER_ASAN else B
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", build] + targets, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=os.path.join(RUNS, "cache"),
                               ASAN_OPTIONS="detect_leaks=0", QUCS_SETTINGS_DIR=os.path.join(RUNS, "settings"),
                               QUCS_TRASH_DIR=os.path.join(RUNS, "trash"), QUCS_CLAUDE="/nonexistent/claude",
                               QUCS_GH="/nonexistent/gh", QTEST_FUNCTION_TIMEOUT="1800000")
                    env.pop("QUCS_TEST_SLOW", None)
                    env.pop("QUCS_TEST_PART", None)
                    try:
                        r = subprocess.run([os.path.join(build, "qucs/tests", suite), fn], stdout=out,
                                           stderr=subprocess.STDOUT, timeout=1800, env=env, cwd=RUNS)
                        said.append("%s %s" % (fn[:34], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:34])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-70s %s" % (name[:70], said))
        if "BUILD FAILED" in said or "hung" in said or "caught)" not in said:
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
