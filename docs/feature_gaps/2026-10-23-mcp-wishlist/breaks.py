# The wishlist's fixes broken on purpose: each break of BREAKS is applied to
# the source, the tests it names are built in build/ and run - each must fail
# -, then the source is put back. Rebuild afterwards: the source is
# restored, the build is not.
#   HUNT_RUNS=<scratch> python3 breaks.py [name ...]
#   BRK_ONLY_CHECK=1 ...: each break's text looked for, nothing built
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
SRC = os.path.join(ROOT, 'qucs-s-26.1.1', 'qucs')
H = os.environ['HUNT_RUNS']
W = 'test_mcp_wishlist:'
E = 'test_fem_engine:selectionRules'

BREAKS = [
    # N1: a bare number read in SI, far outside the unit expected.
    ('N1 the scale not judged', 'multiphysics/fem_expr.cpp',
     '    if (e.dim().isNone())\n        if (const QString why = outsideItsScale(e.constant(), expected); !why.isEmpty()) {',
     '    if (false)\n        if (const QString why = outsideItsScale(e.constant(), expected); !why.isEmpty()) {',
     [W + 'aBareNumberOutsideItsUnitIsRefused'], {}),
    ("N1 an energy's bounds wide open", 'multiphysics/fem_expr.cpp',
     '{Dim::of(2, 1, -2), 1e-26, 1e-12, "an energy", "1.7[eV]"},',
     '{Dim::of(2, 1, -2), 1e-300, 1e300, "an energy", "1.7[eV]"},',
     [W + 'aBareNumberOutsideItsUnitIsRefused'], {}),
    ('N1 zero refused with the rest', 'multiphysics/fem_expr.cpp',
     '    if (si == 0 || expected.isNone()) return {};', '    if (expected.isNone()) return {};',
     [W + 'aBareNumberOutsideItsUnitIsRefused'], {}),
    # N2: an input read under another name, and a rule left out.
    ('N2 the renamed columns not said', 'qucscontrol_results.cpp',
     '    for (auto it = data.renamed.cbegin(); it != data.renamed.cend(); ++it) renamedForCall(it.key(), it.value());', '',
     [W + 'aRenamedInputIsSaid'], {}),
    ('N2 the rename not kept', 'dataimport.cpp',
     '        if (name != c.name.trimmed()) data->renamed.insert(c.name.trimmed(), name);', '',
     [W + 'aRenamedInputIsSaid'], {}),
    ('N2 renamed and ignored not merged into the answer', 'qucscontrol.cpp',
     '        if (!a_renamed.isEmpty() || !a_ignored.isEmpty()) {', '        if (false) {',
     [W + 'aRenamedInputIsSaid', W + 'aSingularIsReadAsTheListItNames'], {}),
    ("N2 a selection's other rules not said", 'multiphysics/fem_geometry.cpp',
     '    if (!taken.isEmpty()) {\n        static const QStringList rules{', '    if (false) {\n        static const QStringList rules{',
     [E], {}),
    ("N2 'side' with another rule not said", 'multiphysics/fem_geometry.cpp',
     '        if (taken != QLatin1String("of") && rule.contains(QStringLiteral("side"))) over << QStringLiteral("side");', '',
     [E], {}),
    ('N2 a rule that is read said as left out', 'multiphysics/fem_geometry.cpp',
     '            if (key == taken || !rule.contains(key)) continue;', '            if (!rule.contains(key)) continue;',
     [E], {}),
    # N3: how a call would be treated.
    ('N3 get_state without the guards', 'qucscontrol.cpp',
     '    state.insert(QStringLiteral("guards"), guardsState());\n', '',
     [W + 'theGuardsAreToldBeforeACallIsMade'], {}),
    ('N3 describe_tool without the call', 'qucscontrol.cpp',
     '        if (args.contains(QLatin1String("arguments")))\n'
     '            whole.insert(QStringLiteral("this call"), callGuarded(name, args.value(QLatin1String("arguments")).toObject()));',
     '        if (false)\n'
     '            whole.insert(QStringLiteral("this call"), callGuarded(name, args.value(QLatin1String("arguments")).toObject()));',
     [W + 'theGuardsAreToldBeforeACallIsMade'], {}),
    ('N3 everything said to be let through', 'qucscontrol_resources.cpp',
     '    if (const QString always = askedEachTime(tool, arguments); !always.isEmpty()) {',
     '    if (const QString always = QString(); !always.isEmpty()) {',
     [W + 'theGuardsAreToldBeforeACallIsMade'], {}),
    # N4: an axis' own notation.
    ("N4 the axis' notation not read", 'diagrams/diagram.h',
     '      return axis != nullptr && axis->notation >= 0 ? qucs_s::numberformat::fromInt(axis->notation) : notation;',
     '      return notation;', [W + 'anAxisWritesItsNumbersItsOwnWay'], {}),
    ("N4 the axis' decimals not read", 'diagrams/diagram.h',
     '      return axis != nullptr && axis->decimals >= -1 ? axis->decimals : notationDecimals;', '      return notationDecimals;',
     [W + 'anAxisWritesItsNumbersItsOwnWay'], {}),
    ('N4 not kept in the file', 'diagrams/diagram.cpp',
     '    if (!options.isEmpty()) s += " \\"" + options + "\\"";\n', '',
     [W + 'anAxisWritesItsNumbersItsOwnWay'], {}),
    ('N4 not read from the file', 'diagrams/diagram.cpp',
     '    for (const QString& item : s.section(\'"\', 11, 11).split(QLatin1Char(\' \'), Qt::SkipEmptyParts)) {',
     '    for (const QString& item : QStringList()) {', [W + 'anAxisWritesItsNumbersItsOwnWay'], {}),
    ("N4 null does not give the diagram's back", 'qucscontrol_results.cpp',
     '        if (v.isNull()) a->notation = -1;\n        else {', '        if (false) a->notation = -1;\n        else {',
     [W + 'anAxisWritesItsNumbersItsOwnWay'], {}),
    # N5: a plot group's styling kept.
    ("N5 the user's label written over", 'qucs_python.cpp',
     '        if (ours(same->yAxis.Label, yLabel)) same->yAxis.Label = yLabel;',
     '        if (true) same->yAxis.Label = yLabel;', [W + 'aPlotGroupKeepsWhatTheUserSet'], {}),
    ('N5 a unit changed not followed', 'qucs_python.cpp',
     '      return now.isEmpty() || stem(now) == stem(asked);', '      return now.isEmpty();',
     [W + 'aPlotGroupKeepsWhatTheUserSet'], {}),
    ('N5 what was kept not said', 'qucs_python.cpp', '    if (kept != nullptr) *kept = theirs;\n', '',
     [W + 'aPlotGroupKeepsWhatTheUserSet'], {}),
    # N6: a filled box its own paper.
    ('N6 the fill not paper', 'paintings/shapepainting.cpp',
     '  if (fill.style() != Qt::NoBrush && fill.color().alpha() >= 192) inside.emplace(fill.color());', '',
     [W + 'aFilledBoxIsItsOwnPaper'], {}),
    # N7: mesh convergence.
    ('N7 the finer mesh not set', 'qucscontrol_fem.cpp',
     '    mesh->props.insert(QStringLiteral("hmax"), formatNumber(h0 / factor, 6));', '',
     [W + 'aMeshConvergesAndIsMeasured'], {}),
    ('N7 the mesh not put back', 'qucscontrol_fem.cpp',
     '        if (run->doc && run->changed && run->meshWas) {\n            Model m = run->doc->model();',
     '        if (false) {\n            Model m = run->doc->model();', [W + 'aMeshConvergesAndIsMeasured'], {}),
    ('N7 the model left unsaved', 'qucscontrol_fem.cpp',
     '            run->doc->setDocChanged(run->wasChanged);', '', [W + 'aMeshConvergesAndIsMeasured'], {}),
    ('N7 a refinement of 0.5 taken', 'qucscontrol_fem.cpp',
     '        if (!(f > 1 && f <= 100))', '        if (!(f > 0 && f <= 100))', [W + 'aMeshConvergesAndIsMeasured'], {}),
    # N8: reference numbers.
    ('N8 the reference not measured', 'qucscontrol_fem.cpp',
     '    if (args.contains(QLatin1String("reference"))) {\n        QJsonObject want;', '    if (false) {\n        QJsonObject want;',
     [W + 'aMeshConvergesAndIsMeasured'], {}),
    ('N8 a reference that is no number taken', 'qucscontrol_fem.cpp',
     '            if (!it.value().isDouble()) {', '            if (false) {', [W + 'aMeshConvergesAndIsMeasured'], {}),
    ('N8 a file that is not there passed', 'qucscontrol_fem.cpp',
     '    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {', '    if (false) {',
     [W + 'aMeshConvergesAndIsMeasured'], {}),
    ('N8 none matched not said', 'qucscontrol_fem.cpp',
     '        if (matched == 0)\n            against.insert(QStringLiteral("none matched"),',
     '        if (false)\n            against.insert(QStringLiteral("none matched"),',
     [W + 'aMeshConvergesAndIsMeasured'], {}),
    # N9: a ground placed and left.
    ('N9 no loose ground looked for', 'qucscontrol.cpp',
     '    if (const QPoint loose = looseGroundNear(sch, p); !loose.isNull()) {',
     '    if (const QPoint loose = QPoint(); !loose.isNull()) {', [W + 'groundReusesOneLeftUnwired'], {}),
    ('N9 one wired already taken', 'qucscontrol.cpp',
     '        if (n == nullptr || n->conn_count() > 1) continue;        // wired to something already', '',
     [W + 'groundReusesOneLeftUnwired'], {}),
    ('N9 one out of reach taken', 'qucscontrol.cpp', 'constexpr int kGroundReach = 200;', 'constexpr int kGroundReach = 100000;',
     [W + 'groundReusesOneLeftUnwired'], {}),
    ('N9 one out of line taken', 'qucscontrol.cpp',
     '        if (at.x() != p.x() && at.y() != p.y()) continue;         // not in a straight line', '',
     [W + 'groundReusesOneLeftUnwired'], {}),
    # N10: a script's error, and the user's time.
    ('N10 the call made not said', 'qucscontrol_script.cpp',
     '            else if (api.calls() > 0)', '            else if (false)',
     [W + 'aStoppedScriptSaysWhatItWasDoing'], {}),
    ("N10 the user's time counted", 'qucscontrol_script.cpp',
     '            if (clock.elapsed() - (askedUserMs() - askedBefore) >= qint64(timeout) * 1000) {',
     '            if (clock.elapsed() >= qint64(timeout) * 1000) {', [W + 'aStoppedScriptSaysWhatItWasDoing'], {}),
    ("N10 the user's time not measured", 'qucscontrol_resources.cpp',
     '            if (const qint64 since = a_askedUserSince.exchange(0); since > 0) a_askedUserMs += monotonicMs() - since;',
     '            a_askedUserSince.store(0);', [W + 'aStoppedScriptSaysWhatItWasDoing'], {}),
    ("N10 the user's time counted only once it is over", 'qucscontrol.h',
     '        return a_askedUserMs.load() + (since > 0 ? std::max<qint64>(0, monotonicMs() - since) : 0);',
     '        return a_askedUserMs.load();', [W + 'aStoppedScriptSaysWhatItWasDoing'], {}),
    ("N10 the clock from zero, so no wait is seen", 'qucscontrol_resources.cpp',
     '    return 1 + std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();',
     '    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();',
     [W + 'aStoppedScriptSaysWhatItWasDoing'], {}),
    # N11: a singular read as the list it names.
    ('N11 no singular read', 'qucscontrol.cpp',
     '    if (const QJsonObject plural = asPlural(tool, args); !plural.isEmpty()) return call(tool, plural, done, async);\n', '',
     [W + 'aSingularIsReadAsTheListItNames'], {}),
    ('N11 a plural given over-written', 'qucscontrol.cpp',
     '            if (args.contains(plural) || !fields.contains(plural)) continue;', '            if (!fields.contains(plural)) continue;',
     [W + 'aSingularIsReadAsTheListItNames'], {}),
    ('N11 the value not wrapped for a list', 'qucscontrol.cpp',
     '            out.insert(plural, list && !it.value().isArray() ? QJsonValue(QJsonArray{it.value()}) : it.value());',
     '            out.insert(plural, it.value());', [W + 'aSingularIsReadAsTheListItNames'], {}),
    ('N11 the value wrapped whatever the field takes', 'qucscontrol.cpp',
     '            out.insert(plural, list && !it.value().isArray() ? QJsonValue(QJsonArray{it.value()}) : it.value());',
     '            out.insert(plural, QJsonValue(QJsonArray{it.value()}));', [W + 'aSingularIsReadAsTheListItNames'], {}),
    ('N11 an empty name read', 'qucscontrol.cpp',
     '        if (it.key().isEmpty() || fields.contains(it.key())) continue;', '        if (fields.contains(it.key())) continue;',
     [W + 'aSingularIsReadAsTheListItNames'], {}),
    # N12: the selection as data.
    ('N12 get_state without it', 'qucscontrol.cpp',
     '        if (const QJsonObject selected = selectionOf(doc); !selected.isEmpty())\n'
     '            d.insert(QStringLiteral("selection"), selected);',
     '        if (const QJsonObject selected = QJsonObject(); !selected.isEmpty())\n'
     '            d.insert(QStringLiteral("selection"), selected);', [W + 'theSelectionIsGivenAsData'], {}),
    ('N12 get_ui without it', 'qucscontrol_ui.cpp',
     '    if (name == QLatin1String("document"))\n        if (QucsDoc* doc = QucsApp::docIn(root); doc != nullptr)',
     '    if (false)\n        if (QucsDoc* doc = QucsApp::docIn(root); doc != nullptr)', [W + 'theSelectionIsGivenAsData'], {}),
    ('N12 a text tab left out', 'qucscontrol.cpp',
     '        if (!cursor.hasSelection()) return {};', '        if (true) return {};',
     [W + 'theSelectionIsGivenAsData'], {}),
    # N13: a smaller picture.
    ('N13 the long side not cut', 'qucscontrol.cpp',
     '    if (const int longest = std::max(out.width(), out.height()); longest > asked.longest && longest > 0)',
     '    if (const int longest = out.width(); longest > 1800 && longest > 0)', [W + 'aPictureIsAsSmallAsAsked'], {}),
    ("N13 'max_width' not read", 'qucscontrol.cpp', '        asked.longest = wanted;', '',
     [W + 'aPictureIsAsSmallAsAsked'], {}),
    ('N13 a width of 10 taken', 'qucscontrol.cpp', '        if (wanted < 100 || wanted > 4000) {', '        if (false) {',
     [W + 'aPictureIsAsSmallAsAsked'], {}),
    ('N13 a JPEG asked for sent as a PNG', 'qucscontrol.cpp',
     '    image.save(&buffer, asked.jpeg ? "JPEG" : "PNG", asked.jpeg ? 80 : -1);', '    image.save(&buffer, "PNG");',
     [W + 'aPictureIsAsSmallAsAsked'], {}),
    ('N13 a format that is no format taken', 'qucscontrol.cpp',
     '    if (!format.isEmpty() && format != QLatin1String("png") && format != QLatin1String("jpeg") && format != QLatin1String("jpg")) {',
     '    if (false) {', [W + 'aPictureIsAsSmallAsAsked'], {}),
    ('N13 a scale of 9 taken', 'qucscontrol.cpp', '        if (!(wanted >= 0.05 && wanted <= 4)) {', '        if (false) {',
     [W + 'aPictureIsAsSmallAsAsked'], {}),
    # N14: what a screen picture shows.
    ('N14 nothing said of the view', 'qucscontrol.cpp', '    QString inView = shownInView(doc);', '    QString inView;',
     [W + 'aScreenPictureSaysWhatItShows'], {}),
    ("N14 a text tab's lines left out", 'qucscontrol.cpp',
     '        return tr("Lines %1 to %2 of %3 in view (get_text with \'from\' and \'to\' reads them).")\n'
     '            .arg(first).arg(last).arg(text->document()->blockCount());',
     '        return QString::number(first + last + text->document()->blockCount()).isEmpty() ? QString() : QString();',
     [W + 'aScreenPictureSaysWhatItShows'], {}),
    # The check of 23 October, its seven findings.
    ('C1 the mesh refined by its own size alone', 'qucscontrol_fem.cpp',
     '            if (c.type == QLatin1String("mesh/size") || c.type == QLatin1String("mesh/layered"))',
     '            if (false)', [W + 'aWeakRefinementShowsNothingAndAnEditIsChecked'], {}),
    ('C1 the counts not multiplied', 'qucscontrol_fem.cpp',
     '                        c.props.insert(QStringLiteral("count"), std::clamp(qRound(count * factor), 1, 100000));', '',
     [W + 'aWeakRefinementShowsNothingAndAnEditIsChecked'], {}),
    ('C1 nothing compared still called converged', 'qucscontrol_fem.cpp',
     '            const bool compared = !moved.isEmpty();', '            const bool compared = true;',
     [W + 'aWeakRefinementShowsNothingAndAnEditIsChecked'], {}),
    ('C1 a mesh that hardly grew still called converged', 'qucscontrol_fem.cpp',
     '            const bool finer = grew >= 1.5;', '            const bool finer = true;',
     [W + 'aWeakRefinementShowsNothingAndAnEditIsChecked'], {}),
    ('C1 how far it grew not said', 'qucscontrol_fem.cpp',
     '                                tr("%1 times the unknowns of the one before, %2 %3 divided")',
     '                                tr("%1%2 %3 divided")',
     [W + 'aMeshConvergesAndIsMeasured'], {}),
    ("C1 a custom mesh's stale fields applied", 'qucscontrol_fem.cpp',
     '        if (!wasCustom) mesh->props.remove(QLatin1String(key));', '        if (false) mesh->props.remove(QLatin1String(key));',
     [W + 'aWeakRefinementShowsNothingAndAnEditIsChecked'], {}),
    ('C2 nothing checked at edit time', 'qucscontrol_fem.cpp',
     '                    def != nullptr && def->kind == PropertyDef::Expression && !def->unit.isEmpty()',
     '                    false && def != nullptr && def->kind == PropertyDef::Expression && !def->unit.isEmpty()',
     [W + 'aWeakRefinementShowsNothingAndAnEditIsChecked'], {}),
    ('C2 the edit refused, not said', 'qucscontrol_fem.cpp',
     '                                checked << tr("%1: %2").arg(it.key(), why);',
     '                                return tr("%1: %2").arg(it.key(), why);',
     [W + 'aWeakRefinementShowsNothingAndAnEditIsChecked'], {}),
    ('C3 the unit spelled out of its base units', 'multiphysics/fem_expr.cpp',
     '        {"C·m", Dim::of(1, 0, 1, 1), true},', '', [W + 'aBareNumberOutsideItsUnitIsRefused'], {}),
    ('C3 give the number in SI again', 'multiphysics/fem_expr.cpp',
     '"brackets, as %5, or give a value within that range")', '"brackets, as %5, or give the number in SI")',
     [W + 'aBareNumberOutsideItsUnitIsRefused'], {}),
    ('C4 a reference row left over only counted', 'qucscontrol_fem.cpp',
     '        if (matched > 0)\n            for (const QString& name : std::as_const(unmatched))',
     '        if (false)\n            for (const QString& name : std::as_const(unmatched))',
     [W + 'aMeshConvergesAndIsMeasured'], {}),
    ('C4 a row that did match named all the same', 'qucscontrol_fem.cpp',
     '        met.insert(reference.contains(name) ? name : reference.constBegin().key());', '',
     [W + 'aMeshConvergesAndIsMeasured'], {}),
    ('C5 a number given as text not read', 'qucscontrol.cpp',
     '    if (const QJsonObject numbers = asNumbers(tool, args, &asText); !numbers.isEmpty()) {',
     '    if (const QJsonObject numbers = QJsonObject(); !numbers.isEmpty()) {',
     [W + 'aSingularIsReadAsTheListItNames'], {}),
    ('C5 text taken for a field of any type', 'qucscontrol.cpp',
     '        if (type != QLatin1String("integer") && type != QLatin1String("number")) continue;', '',
     [W + 'aSingularIsReadAsTheListItNames'], {}),
    ('C5 a fraction taken for a whole number', 'qucscontrol.cpp',
     '        if (type == QLatin1String("integer") && (value != std::floor(value) || std::abs(value) > 2147483647.0)) continue;', '',
     [W + 'aSingularIsReadAsTheListItNames'], {}),
    ('C5 what it read not said', 'qucscontrol.cpp',
     '        for (const QString& line : std::as_const(asText)) noteForCall(line);', '',
     [W + 'aSingularIsReadAsTheListItNames'], {}),
    ('C6 the PDF lines left ending CR LF', 'qucscontrol_design.cpp',
     '''        QString text = all.text().replace(QLatin1String("\\r\\n"), QLatin1String("\\n")).replace(QLatin1Char('\\r'), QLatin1Char('\\n'));''',
     '        QString text = all.text();', [W + 'aRegionKeepsThePdfsLinesAndOneLineEnding'], {}),
    ('C7 the region not applied to the text', 'qucscontrol_design.cpp',
     '        if (!corners.isEmpty()) {\n            const QSizeF paper = pdf.pagePointSize(page - 1);',
     '        if (false) {\n            const QSizeF paper = pdf.pagePointSize(page - 1);',
     [W + 'aRegionKeepsThePdfsLinesAndOneLineEnding'], {}),
    # (C7's "every line kept" break now names the grouped boxes.)
    ('C7 every line kept whatever its box', 'qucscontrol_design.cpp',
     '                    if (ofLines.at(i).intersects(region)) kept << lines.at(i);',
     '                    kept << lines.at(i);', [W + 'aRegionKeepsThePdfsLinesAndOneLineEnding'], {}),
    ('C7 a region outside the page taken', 'qucscontrol_design.cpp',
     '            if (region.isEmpty())', '            if (false)',
     [W + 'aRegionKeepsThePdfsLinesAndOneLineEnding'], {}),
    # The re-check of 23 October, its three open items.
    ('R1 the boxes paired run by run', 'qucscontrol_design.cpp',
     '                    if (over > 0.5 * std::min(last.height(), box.height())) {',
     '                    if (false) {', [W + 'aRegionKeepsThePdfsLinesAndOneLineEnding'], {}),
    ('R1 how the region was read not said', 'qucscontrol_design.cpp',
     '            result.insert(QStringLiteral("of the region"), how);', '',
     [W + 'aRegionKeepsThePdfsLinesAndOneLineEnding'], {}),
    ('R2 the sizes of the mesh before divided again', 'qucscontrol_fem.cpp',
     '    *mesh = meshWas;', '    mesh->props = meshWas.props;',
     [W + 'aWeakRefinementShowsNothingAndAnEditIsChecked'], {}),
    ('R2 the features not put back', 'qucscontrol_fem.cpp',
     '            if (Node* mesh = m.find(m.mesh().tag)) *mesh = *run->meshWas;',
     '            if (Node* mesh = m.find(m.mesh().tag)) mesh->props = run->meshWas->props;',
     [W + 'aWeakRefinementShowsNothingAndAnEditIsChecked'], {}),
    ('R2 the smallest element left where it was', 'qucscontrol_fem.cpp',
     '    if (hmin0 > 0 && hmin0 / factor < h0 / factor) {', '    if (false) {',
     [W + 'aWeakRefinementShowsNothingAndAnEditIsChecked'], {}),
    ('R3 the tool named twice in a script', 'qucscontrol_script.cpp',
     '            return fail(why.startsWith(tool + QLatin1Char(\':\')) ? why : QStringLiteral("%1: %2").arg(tool, why));',
     '            return fail(QStringLiteral("%1: %2").arg(tool, why));',
     [W + 'aStoppedScriptSaysWhatItWasDoing'], {}),
    # The second re-check of 23 October.
    ('S1 the corners not moved onto the text', 'qucscontrol_design.cpp',
     '                               pdf.getSelection(page - 1, QPointF(first.left() + 1, first.center().y()),\n'
     '                                                QPointF(last.right() - 1, last.center().y()));',
     '                               pdf.getSelection(page - 1, region.topLeft(), region.bottomRight());',
     [W + 'aRegionKeepsThePdfsLinesAndOneLineEnding'], {}),
    ('S1 a region with no text of the page read all the same', 'qucscontrol_design.cpp',
     '                if (first.isNull()) {\n                    text.clear();', '                if (false) {\n                    text.clear();',
     [W + 'aRegionKeepsThePdfsLinesAndOneLineEnding'], {}),
    ('S1 every run taken, not those the region meets', 'qucscontrol_design.cpp',
     '                    if (box.isNull() || !box.intersects(region)) continue;', '                    if (box.isNull()) continue;',
     [W + 'aRegionKeepsThePdfsLinesAndOneLineEnding'], {}),
    ('S3 a model not watched on disk', 'qucs.cpp',
     '            || dynamic_cast<MultiphysicsDoc *>(doc) != nullptr)', '            )',
     [W + 'aModelTabFollowsItsFileOnDisk'], {}),
    ('S3 a model not read again', 'qucs.cpp',
     '  } else if (auto *model = dynamic_cast<MultiphysicsDoc *>(doc)) {\n    loaded = model->reload();',
     '  } else if (auto *model = dynamic_cast<MultiphysicsDoc *>(doc); false) {\n    loaded = model->reload();',
     [W + 'aModelTabFollowsItsFileOnDisk'], {}),
    # T1-T4 (the third re-check, nit 1): a call reads a file changed on disk
    # before it answers.
    ('T1 the file not read at a call', 'qucscontrol.cpp',
     '    if (a_callDepth == 0 && a_alone == 0 && a_spinning == 0) a_app->reloadChangedDocumentsNow();',
     '    if (false) a_app->reloadChangedDocumentsNow();',
     [W + 'aCallRightAfterAWriteSeesTheFile'], {}),
    ('T2 the reload after this call\'s notes', 'qucscontrol.cpp',
     '    readAnyFileChangedOnDisk();\n    if (alone) ++a_alone;\n'
     '    const QStringList changes = changesSince(caller) + notesFor(caller);',
     '    if (alone) ++a_alone;\n'
     '    const QStringList changes = changesSince(caller) + notesFor(caller);\n    readAnyFileChangedOnDisk();',
     [W + 'aCallRightAfterAWriteSeesTheFile'], {}),
    ('T3 unsaved changes written over', 'qucs.cpp',
     '  if (doc->getDocChanged()) {\n    statusBar()->showMessage(tr("%1 was changed by another program; it has unsaved changes here, so it was not loaded again.")',
     '  if (false) {\n    statusBar()->showMessage(tr("%1 was changed by another program; it has unsaved changes here, so it was not loaded again.")',
     [W + 'aCallRightAfterAWriteSeesTheFile'], {}),
    ("T4 a model's file not followed", 'qucs.cpp',
     '             || dynamic_cast<MultiphysicsDoc *>(doc) != nullptr);', '             );',
     [W + 'aCallRightAfterAWriteSeesTheFile', W + 'aModelTabFollowsItsFileOnDisk'], {}),
]


def watched(cmd, env, timeout, rss_kb):
    """The test run, killed past \a timeout seconds or \a rss_kb of memory: what stopped it, or its result."""
    import tempfile, time
    out, err = tempfile.TemporaryFile('w+'), tempfile.TemporaryFile('w+')
    p = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=out, stderr=err, text=True)
    start, why = time.time(), None
    while p.poll() is None:
        time.sleep(0.25)
        if time.time() - start > timeout:
            why = f'past {timeout} s'
        elif rss_kb:
            r = subprocess.run(['ps', '-o', 'rss=', '-p', str(p.pid)], capture_output=True, text=True).stdout.strip()
            if r.isdigit() and int(r) > rss_kb:
                why = f'past {rss_kb >> 20} GB of memory'
        if why:
            p.kill()
            p.wait()
            return why
    out.seek(0), err.seek(0)
    return subprocess.CompletedProcess(cmd, p.returncode, out.read(), err.read())


def run(name, path, old, new, tests, opts):
    # (path, old, new), a list of them, or old ('before', start, end): that part as it was at BEFORE.
    if isinstance(path, list):
        edits = path
    elif isinstance(old, tuple):
        now, was = before(path, old[1], old[2])
        edits = [(path, now, was)]
    else:
        edits = [(path, old, new)]
    kept = {}
    for p, o, n in edits:
        src = os.path.join(SRC, p)
        kept.setdefault(src, open(src, encoding='utf-8').read())
        now = open(src, encoding='utf-8').read()
        if now.count(o) != 1 or o == n:
            for f, t in kept.items():
                open(f, 'w', encoding='utf-8').write(t)
            return f'{name}: its text found {now.count(o)} times in {p}' + (' (and is as it was)' if o == n else '')
        open(src, 'w', encoding='utf-8').write(now.replace(o, n))
    if os.environ.get('BRK_ONLY_CHECK'):
        for f, t in kept.items():
            open(f, 'w', encoding='utf-8').write(t)
        return f'{name}: found'
    build = os.path.join(ROOT, opts.get('build', 'build'))
    try:
        targets = sorted({t.split(':')[0] for t in tests})
        b = subprocess.run(['ninja', '-C', build] + [f'qucs/tests/{t}' for t in targets], capture_output=True, text=True)
        if b.returncode != 0:
            return f'{name}: the build failed\n{b.stdout[-1500:]}'
        env = dict(os.environ, HOME=H + '/home', QUCS_SETTINGS_DIR=H + '/settings', QUCS_TRASH_DIR=H + '/trash',
                   QUCS_CACHE_DIR=H + '/cache', QT_QPA_PLATFORM='offscreen',
                   ASAN_OPTIONS='detect_leaks=0' + (':' + opts['asan'] if 'asan' in opts else ''),
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        env.pop('QUCS_TEST_SLOW', None)
        caught = []
        for t in tests:
            exe, fn = t.split(':')
            p = watched([os.path.join(build, 'qucs', 'tests', exe), fn], env, opts.get('timeout', 900), opts.get('rss'))
            if isinstance(p, str):
                caught.append(f'{exe}: {p}')
                continue
            fails = [l for l in p.stdout.splitlines() if l.startswith('FAIL!')]
            report = [l for l in (p.stdout + p.stderr).splitlines() if 'runtime error' in l or 'ERROR: AddressSanitizer' in l]
            if p.returncode != 0 or fails:
                caught.append(f'{exe}: ' + (fails[0][:200] if fails else (report[0][:200] if report else f'exit {p.returncode}')))
        return f'{name}: ' + ('CAUGHT - ' + ' | '.join(caught) if caught else 'NOT CAUGHT')
    finally:
        for f, t in kept.items():
            open(f, 'w', encoding='utf-8').write(t)


for d in ('home', 'settings', 'trash', 'cache'):
    os.makedirs(os.path.join(H, d), exist_ok=True)
for name, path, old, new, tests, opts in BREAKS:
    if len(sys.argv) > 1 and not any(a in name for a in sys.argv[1:]):
        continue
    print(run(name, path, old, new, tests, opts), flush=True)
