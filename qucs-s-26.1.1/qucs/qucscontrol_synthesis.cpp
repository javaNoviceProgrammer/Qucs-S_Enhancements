/*
 * qucscontrol_synthesis.cpp - Claude's tools for the Tools menu's
 *                             synthesis and calculation programs: filters
 *                             (LC and active), attenuators, matching
 *                             circuits, power combiners, line calculation
 *                             and a receiver's budget - each program run
 *                             with --json (its own calculation, a spec in
 *                             and the result out), matching in Qucs-S
 *                             itself; a design placed as its paste places
 *                             it, in a new schematic or one open
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"

#include "components/component.h"
#include "diagrams/diagram.h"
#include "dialogs/matchdialog.h"
#include "extsimkernels/spicecompat.h"
#include "main.h"
#include "misc.h"
#include "paintings/painting.h"
#include "qucs.h"
#include "schematic.h"
#include "valuereading.h"
#include "wire.h"
#include "wirelabel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QRegularExpression>
#include <QTextStream>

#include <cmath>
#include <complex>
#include <memory>

using namespace qucs_s::control;

namespace {

// The folder a program of the build tree is built in, by its name.
QString buildFolderOf(const QString& program)
{
    if (program == QLatin1String("rxcalc")) return QStringLiteral("rxcalc");
    if (program == QLatin1String(QUCS_NAME "trans")) return QStringLiteral("qucs-transcalc");
    // qucs-sfilter: qucs-filter
    return QStringLiteral("qucs-") + program.mid(QStringLiteral(QUCS_NAME).size());
}

// Where \a program is: beside Qucs-S as its Tools menu finds it (the
// settings' BinDir: an app's Contents/MacOS/bin), in QUCS_TOOLS_DIR, or
// in the build tree Qucs-S was built in; empty when it is not there.
// \a looked gets each place looked in.
QString programPath(const QString& program, QStringList* looked)
{
    const auto in = [&program](const QString& dir) {
#if defined(_WIN32) || defined(__MINGW32__)
        return QDir(dir).absoluteFilePath(program + QStringLiteral(".exe"));
#elif defined(__APPLE__)
        return QDir(dir).absoluteFilePath(program + QStringLiteral(".app/Contents/MacOS/") + program);
#else
        return QDir(dir).absoluteFilePath(program);
#endif
    };
    QStringList places{in(QucsSettings.BinDir)};
    if (const QString tools = qEnvironmentVariable("QUCS_TOOLS_DIR"); !tools.isEmpty()) {
        places << in(tools) << in(QDir(tools).absoluteFilePath(buildFolderOf(program)));
    }
    // The build tree: <build>/qucs/qucs-s(.app/Contents/MacOS), the
    // program in <build>/<its folder>.
    QDir up(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 5; ++i) {
        if (QFileInfo::exists(up.absoluteFilePath(QStringLiteral("CMakeCache.txt")))) {
            places << in(up.absoluteFilePath(buildFolderOf(program)));
            break;
        }
        if (!up.cdUp()) break;
    }
    for (const QString& p : std::as_const(places))
        if (QFileInfo(p).isExecutable()) return p;
    if (looked) *looked = places;
    return {};
}

// A number given as a number, or as text with a scale and a unit ("1.6
// mm", "900 MHz"); \a otherwise when it is neither.
double numberOf(const QJsonValue& v, double otherwise)
{
    if (v.isDouble()) return v.toDouble();
    if (!v.isString()) return otherwise;
    const qucs_s::units::Reading r = qucs_s::units::read(v.toString().trimmed());
    return r.kind == qucs_s::units::Reading::Number ? r.value : otherwise;
}

QJsonValue rounded(double v)
{
    return std::isfinite(v) ? QJsonValue(std::round(v * 1e6) / 1e6) : QJsonValue(QJsonValue::Null);
}

// The simulator a design is made for: the one 'simulator' names, else the
// settings'.
QString simulatorFor(const QJsonObject& args)
{
    const QString given = args.value(QLatin1String("simulator")).toString().trimmed().toLower();
    if (!given.isEmpty()) return given;
    switch (QucsSettings.DefaultSimulator) {
    case spicecompat::simQucsator: return QStringLiteral("qucsator");
    case spicecompat::simXyce: return QStringLiteral("xyce");
    case spicecompat::simSpiceOpus: return QStringLiteral("spiceopus");
    default: return QStringLiteral("ngspice");
    }
}

// The substrate of a microstrip design, in metres, as the programs take
// it: {"er", "h", "t", "tand", "resistivity", "roughness", "min_width",
// "max_width"} - numbers, or text with units (1.6 mm, 35 um).
tSubstrate substrateOf(const QJsonObject& o)
{
    const auto value = [&o](const char* key, double otherwise) { return numberOf(o.value(QLatin1String(key)), otherwise); };
    tSubstrate s;
    s.er = value("er", 9.8);
    s.height = value("h", 1e-3);
    s.thickness = value("t", 12.5e-6);
    s.tand = value("tand", 0.0);
    s.resistivity = value("resistivity", 2.43902e-8);
    s.roughness = value("roughness", 0.0);
    s.minWidth = value("min_width", 0.4e-3);
    s.maxWidth = value("max_width", 5e-3);
    return s;
}

// A complex number given as [re, im], {"re", "im"}, {"mag", "deg"} or
// text: "10-j20", "10 - 20j", "50".
bool complexOf(const QJsonValue& v, std::complex<double>* z)
{
    if (v.isDouble()) {
        *z = {v.toDouble(), 0.0};
        return true;
    }
    if (v.isArray() && v.toArray().size() == 2 && v.toArray().at(0).isDouble() && v.toArray().at(1).isDouble()) {
        *z = {v.toArray().at(0).toDouble(), v.toArray().at(1).toDouble()};
        return true;
    }
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        if (o.contains(QLatin1String("re"))) {
            *z = {o.value(QLatin1String("re")).toDouble(), o.value(QLatin1String("im")).toDouble()};
            return true;
        }
        if (o.contains(QLatin1String("mag"))) {
            *z = std::polar(o.value(QLatin1String("mag")).toDouble(), o.value(QLatin1String("deg")).toDouble() * M_PI / 180.0);
            return true;
        }
        return false;
    }
    if (!v.isString()) return false;
    QString t = v.toString().simplified().remove(QLatin1Char(' '));
    t.replace(QLatin1Char('i'), QLatin1Char('j'));
    // re, then +/- an imaginary part with j before or after it.
    static const QRegularExpression re(QStringLiteral("^([+-]?[0-9.]+(?:[eE][+-]?\\d+)?)?(?:([+-])j?([0-9.]+(?:[eE][+-]?\\d+)?)j?)?$"));
    const QRegularExpressionMatch m = re.match(t);
    if (!m.hasMatch() || (m.captured(1).isEmpty() && m.captured(3).isEmpty())) return false;
    const double real = m.captured(1).isEmpty() ? 0.0 : m.captured(1).toDouble();
    const double imag = m.captured(3).isEmpty() ? 0.0 : (m.captured(2) == QLatin1String("-") ? -1.0 : 1.0) * m.captured(3).toDouble();
    *z = {real, imag};
    return true;
}

QString complexText(std::complex<double> z)
{
    return QStringLiteral("%1 %2 j%3").arg(z.real(), 0, 'g', 6).arg(z.imag() < 0 ? QLatin1Char('-') : QLatin1Char('+')).arg(std::abs(z.imag()), 0, 'g', 6);
}

} // namespace

QJsonObject QucsControl::runToolProgram(const QString& program, const QJsonObject& spec, QString* error)
{
    QStringList looked;
    const QString path = programPath(program, &looked);
    if (path.isEmpty()) {
        *error = tr("%1, the program that does this, is not in this build of Qucs-S (looked for %2).")
                     .arg(program, looked.join(QStringLiteral(", ")));
        return {};
    }
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    // (Its fonts' and platform's chatter on standard error is left out.)
    env.insert(QStringLiteral("QT_LOGGING_RULES"), QStringLiteral("*.debug=false;qt.qpa.*=false"));
    p.setProcessEnvironment(env);
    p.start(path, {QStringLiteral("--json")});
    if (!p.waitForStarted(10000)) {
        *error = tr("%1 did not start: %2").arg(program, p.errorString());
        return {};
    }
    p.write(QJsonDocument(spec).toJson(QJsonDocument::Compact));
    p.closeWriteChannel();
    if (!p.waitForFinished(30000)) {
        p.kill();
        p.waitForFinished(2000);
        *error = tr("%1 did not finish within 30 s.").arg(program);
        return {};
    }
    const QByteArray out = p.readAllStandardOutput().trimmed();
    QJsonParseError parse;
    const QJsonDocument d = QJsonDocument::fromJson(out.mid(out.lastIndexOf('\n') + 1), &parse);
    if (!d.isObject()) {
        *error = tr("%1 gave no answer it should (%2): %3").arg(program, parse.errorString(), QString::fromUtf8(out.left(300)));
        return {};
    }
    QJsonObject r = d.object();
    if (r.contains(QLatin1String("error"))) {
        *error = r.value(QLatin1String("error")).toString();
        return {};
    }
    return r;
}

QJsonObject QucsControl::placeDesign(const QJsonObject& args, const QString& text, QString* error)
{
    // Where: the schematic 'path' names (at x, y, else below what is
    // there) - or, with neither, a new one (saved as 'save_as').
    const bool intoOne = args.contains(QLatin1String("path")) || args.contains(QLatin1String("x")) || args.contains(QLatin1String("y"));
    if (intoOne && args.contains(QLatin1String("save_as"))) {
        *error = tr("'save_as' names a new schematic for it: give 'path' (and x, y) to place it in one open, or 'save_as', not both.");
        return {};
    }
    Schematic* sch = nullptr;
    if (intoOne) {
        sch = schematic(args, error, true);
        if (sch == nullptr) return {};
    } else {
        newDocument(QJsonObject{});
        sch = dynamic_cast<Schematic*>(a_app->getDoc());
        if (sch == nullptr) {
            *error = tr("No new schematic could be made.");
            return {};
        }
    }
    // A value that is no number (nan, inf): a design that cannot be.
    static const QRegularExpression notANumber(QStringLiteral("\"[-+]?(nan|inf)[^\"]*\""), QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = notANumber.match(text); m.hasMatch()) {
        *error = tr("The design has a value that is no number (%1): it cannot be made with these values.").arg(m.captured(0));
        return {};
    }
    // As it was copied, its version this one's (the programs are built
    // with Qucs-S, and the format is the same).
    QString body = text;
    if (body.startsWith(QLatin1String("<Qucs Schematic "))) body = body.mid(body.indexOf(QLatin1Char('\n')) + 1);
    body.prepend(QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n"));
    std::list<Element*> elements;
    QTextStream stream(&body, QIODevice::ReadOnly);
    misc::ErrorCapture capture;
    const bool read = sch->pasteText(&stream, &elements);
    if (!read || elements.empty()) {
        for (Element* e : elements) delete e;
        *error = tr("The design could not be read as a schematic%1.")
                     .arg(capture.errors().isEmpty() ? QString() : QStringLiteral(": ") + capture.errors().join(QStringLiteral("; ")));
        return {};
    }
    // Moved so its top left corner is at x, y (on the grid), or below what
    // is there; in a new schematic, as the program drew it.
    QRect box;
    for (const Element* e : elements) box |= e->boundingRect();
    QPoint shift;
    if (intoOne) {
        const QRect used = sch->allBoundingRect();
        const bool empty = sch->a_DocComps.empty() && sch->a_DocWires.empty() && sch->a_DocDiags.empty() && sch->a_DocPaints.empty();
        int x = args.contains(QLatin1String("x")) ? args.value(QLatin1String("x")).toInt() : (empty ? box.left() : used.left());
        int y = args.contains(QLatin1String("y")) ? args.value(QLatin1String("y")).toInt() : (empty ? box.top() : used.bottom() + 80);
        sch->setOnGrid(x, y);
        shift = QPoint(x, y) - box.topLeft();
        int gx = shift.x(), gy = shift.y();
        sch->setOnGrid(gx, gy);   // (a grid's step, so every pin stays on the grid)
        shift = QPoint(gx, gy);
    }
    QList<Component*> ports;
    QStringList portsBefore;
    for (Component* c : sch->a_DocComps)
        if (c->Model == QLatin1String("Pac")) portsBefore << c->Props.value(0)->Value;
    prepare(sch);
    QStringList parts, analyses;
    QFileInfo info(sch->getDocName());
    // As a paste puts it down (MouseActions::MReleasePaste): each element
    // moved - a wire's label with it, a node's label too - then the parts,
    // the wires, the diagrams, the paintings and the node labels.
    Schematic::Selection selection = sch->elementsToSelection(elements);
    selection.moveCenter(shift.x(), shift.y());
    {
        Schematic::BulkNaming naming{sch};
        for (Component* c : selection.components) {
            c->isSelected = false;
            sch->insertComponent(c);
            sch->enlargeView(c);
            if (c->Model == QLatin1String("Pac")) ports << c;
            if (c->Ports.isEmpty()) analyses << c->Name;
            else if (c->Model != QLatin1String("GND")) {
                const QString value = c->Props.isEmpty() ? QString() : c->Props.at(0)->Value;
                parts << (value.isEmpty() || c->Model == QLatin1String("Pac") ? c->Name : QStringLiteral("%1 %2").arg(c->Name, value));
            }
        }
    }
    for (Wire* w : selection.wires) {
        w->isSelected = false;
        sch->installWire(w);
    }
    for (Diagram* d : selection.diagrams) {
        d->isSelected = false;
        sch->a_DocDiags.push_back(d);
        d->loadGraphData(info.absolutePath() + QDir::separator() + sch->getDataSet());
        sch->enlargeView(d);
    }
    for (Painting* p : selection.paintings) {
        p->isSelected = false;
        sch->a_DocPaints.push_back(p);
        sch->enlargeView(p);
    }
    for (WireLabel* l : selection.labels) {
        l->isSelected = false;
        if (l->owner() == nullptr) sch->placeNodeLabel(l);
    }
    finish(sch, {box.translated(shift).center()});
    QJsonObject result{{QStringLiteral("schematic"), titleOf(sch)},
                       {QStringLiteral("placed"), intoOne ? tr("in %1, its top left corner at %2, %3").arg(titleOf(sch)).arg(box.left() + shift.x()).arg(box.top() + shift.y())
                                                          : tr("in a new schematic, %1").arg(titleOf(sch))},
                       {QStringLiteral("parts"), QJsonArray::fromStringList(parts)}};
    if (!analyses.isEmpty()) result.insert(QStringLiteral("analyses and equations"), QJsonArray::fromStringList(analyses));
    // Its ports numbered on from those there: its equations (S[2,1]) are of
    // ports 1 and 2.
    if (!portsBefore.isEmpty() && !ports.isEmpty()) {
        QStringList nums;
        for (const Component* c : std::as_const(ports)) nums << QStringLiteral("%1 (Num %2)").arg(c->Name, c->Props.value(0)->Value);
        result.insert(QStringLiteral("ports"), tr("%1 had ports already (Num %2): the design's are %3 - its S-parameter equations name "
                                                  "ports 1, 2 ...: renumber them or simulate it alone").arg(titleOf(sch), portsBefore.join(QStringLiteral(", ")),
                                                                                                           nums.join(QStringLiteral(", "))));
    }
    if (!intoOne && args.contains(QLatin1String("save_as"))) {
        const QJsonObject saved = saveDocument(QJsonObject{{QStringLiteral("as"), args.value(QLatin1String("save_as"))}});
        if (saved.value(QLatin1String("isError")).toBool()) {
            result.insert(QStringLiteral("not saved"), textOf(saved));
        } else {
            result.insert(QStringLiteral("saved"), QDir::toNativeSeparators(sch->getDocName()));
            // (Its name now.)
            result.insert(QStringLiteral("schematic"), titleOf(sch));
            result.insert(QStringLiteral("placed"), tr("in a new schematic, %1").arg(titleOf(sch)));
        }
    }
    result.insert(QStringLiteral("one step to undo"), true);
    return result;
}

QJsonObject QucsControl::synthesizeFilter(const QJsonObject& args)
{
    const QString kind = args.value(QLatin1String("kind")).toString(QStringLiteral("lc")).trimmed().toLower();
    if (kind != QLatin1String("lc") && kind != QLatin1String("active"))
        return errorResult(tr("'kind' is lc (a ladder of inductors and capacitors, or lines) or active (op-amps, resistors and "
                              "capacitors)."));
    QJsonObject spec;
    // The design, as the program's window takes it.
    for (const char* key : {"order", "fc", "f2", "fs", "ripple", "atten", "impedance", "gain", "transition", "ap"})
        if (args.contains(QLatin1String(key))) spec.insert(QLatin1String(key), args.value(QLatin1String(key)));
    spec.insert(QStringLiteral("type"), args.value(QLatin1String("response")).toString(kind == QLatin1String("lc") ? QStringLiteral("chebyshev") : QStringLiteral("butterworth")));
    spec.insert(QStringLiteral("class"), args.value(QLatin1String("type")).toString(QStringLiteral("lowpass")));
    if (args.contains(QLatin1String("topology"))) spec.insert(QStringLiteral("realization"), args.value(QLatin1String("topology")));
    if (args.contains(QLatin1String("substrate"))) spec.insert(QStringLiteral("substrate"), args.value(QLatin1String("substrate")));
    spec.insert(QStringLiteral("simulator"), simulatorFor(args));
    QString error;
    const QJsonObject design = runToolProgram(kind == QLatin1String("lc") ? QStringLiteral(QUCS_NAME "filter") : QStringLiteral(QUCS_NAME "activefilter"),
                                              spec, &error);
    if (design.isEmpty()) return errorResult(tr("No filter: %1").arg(error));
    QJsonObject placed = placeDesign(args, design.value(QLatin1String("schematic")).toString(), &error);
    if (placed.isEmpty()) return errorResult(error);
    QJsonObject filter{{QStringLiteral("kind"), kind},
                       {QStringLiteral("response"), design.value(QLatin1String("type"))},
                       {QStringLiteral("type"), design.value(QLatin1String("class"))},
                       {QStringLiteral("topology"), design.value(QLatin1String("realization"))}};
    if (design.contains(QLatin1String("order"))) filter.insert(QStringLiteral("order"), design.value(QLatin1String("order")));
    if (design.contains(QLatin1String("order from"))) filter.insert(QStringLiteral("order from"), design.value(QLatin1String("order from")));
    if (design.contains(QLatin1String("parts"))) filter.insert(QStringLiteral("stages"), design.value(QLatin1String("parts")));
    if (design.contains(QLatin1String("note"))) filter.insert(QStringLiteral("note"), design.value(QLatin1String("note")));
    placed.insert(QStringLiteral("filter"), filter);
    placed.insert(QStringLiteral("next"), tr("simulate it (its analysis and equations came with it), then get_dataset measures it"));
    return jsonResult(placed);
}

QJsonObject QucsControl::synthesizeAttenuator(const QJsonObject& args)
{
    QJsonObject spec;
    for (const char* key : {"topology", "attenuation", "z_in", "z_out", "f", "lumped", "r_below_z0", "p_in", "s_parameters"})
        if (args.contains(QLatin1String(key))) spec.insert(QLatin1String(key), args.value(QLatin1String(key)));
    spec.insert(QStringLiteral("simulator"), simulatorFor(args));
    QString error;
    const QJsonObject design = runToolProgram(QStringLiteral(QUCS_NAME "attenuator"), spec, &error);
    if (design.isEmpty()) return errorResult(tr("No attenuator: %1").arg(error));
    QJsonObject placed = placeDesign(args, design.value(QLatin1String("schematic")).toString(), &error);
    if (placed.isEmpty()) return errorResult(error);
    QJsonObject att{{QStringLiteral("topology"), design.value(QLatin1String("topology"))},
                    {QStringLiteral("ohms"), design.value(QLatin1String("ohms"))},
                    {QStringLiteral("watts dissipated"), design.value(QLatin1String("watts dissipated"))}};
    if (design.contains(QLatin1String("quarter wave length (m)"))) att.insert(QStringLiteral("quarter wave length (m)"), design.value(QLatin1String("quarter wave length (m)")));
    placed.insert(QStringLiteral("attenuator"), att);
    return jsonResult(placed);
}

QJsonObject QucsControl::synthesizePowerCombiner(const QJsonObject& args)
{
    QJsonObject spec;
    for (const char* key : {"type", "f", "z0", "ways", "stages", "ratio_db", "implementation", "alpha", "substrate", "s_parameters"})
        if (args.contains(QLatin1String(key))) spec.insert(QLatin1String(key), args.value(QLatin1String(key)));
    spec.insert(QStringLiteral("simulator"), simulatorFor(args));
    QString error;
    const QJsonObject design = runToolProgram(QStringLiteral(QUCS_NAME "powercombining"), spec, &error);
    if (design.isEmpty()) return errorResult(tr("No power combiner: %1").arg(error));
    QJsonObject placed = placeDesign(args, design.value(QLatin1String("schematic")).toString(), &error);
    if (placed.isEmpty()) return errorResult(error);
    placed.insert(QStringLiteral("combiner"), QJsonObject{{QStringLiteral("type"), design.value(QLatin1String("type"))},
                                                         {QStringLiteral("implementation"), design.value(QLatin1String("implementation"))}});
    return jsonResult(placed);
}

QJsonObject QucsControl::synthesizeMatching(const QJsonObject& args)
{
    static const QStringList topologies{QStringLiteral("l_section"), QStringLiteral("single_stub"), QStringLiteral("double_stub"),
                                        QStringLiteral("quarter_wave"), QStringLiteral("cascaded_l_sections"), QStringLiteral("lambda8_lambda4")};
    QString topology = args.value(QLatin1String("topology")).toString(QStringLiteral("l_section")).trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('_'));
    if (topology == QLatin1String("l") || topology == QLatin1String("lc")) topology = QStringLiteral("l_section");
    if (topology == QLatin1String("multistage_quarter_wave") || topology == QLatin1String("lambda4")) topology = QStringLiteral("quarter_wave");
    const int t = int(topologies.indexOf(topology));
    if (t < 0) return errorResult(tr("'topology' is one of %1, not %2.").arg(topologies.join(QStringLiteral(", ")), topology));
    const auto number = [&args](const char* key, double otherwise) { return numberOf(args.value(QLatin1String(key)), otherwise); };
    const double f = number("f", NAN);
    if (!(f > 0)) return errorResult(tr("'f' is the frequency to match at, in Hz."));
    const double z0 = number("z_source", 50.0);
    const double z2 = number("z_out", z0);
    if (!(z0 > 0) || !(z2 > 0)) return errorResult(tr("'z_source' (and 'z_out') are real impedances in ohms, above 0: the ports' references."));
    const bool micro = args.value(QLatin1String("microstrip")).toBool();
    const bool sp = args.value(QLatin1String("s_parameters")).toBool(true);
    const bool open = args.value(QLatin1String("stubs")).toString(QStringLiteral("open")) != QLatin1String("short");
    const bool balanced = args.value(QLatin1String("balanced_stubs")).toBool();
    const int order = args.value(QLatin1String("sections")).toInt(3) + 1;
    const bool binomial = args.value(QLatin1String("weighting")).toString(QStringLiteral("binomial")) != QLatin1String("chebyshev");
    const double ripple = number("max_ripple", 0.05);
    const tSubstrate substrate = substrateOf(args.value(QLatin1String("substrate")).toObject());
    QStringList said;
    QString text;
    QJsonObject about;
    // (Made as Tools > Matching Circuit makes it, not shown - its equations
    // for the simulator asked for, the settings' put back after.)
    const int settingsSimulator = QucsSettings.DefaultSimulator;
    const QString simulator = simulatorFor(args);
    QucsSettings.DefaultSimulator = simulator == QLatin1String("qucsator") ? spicecompat::simQucsator
                                    : simulator == QLatin1String("xyce")   ? spicecompat::simXyce
                                                                           : spicecompat::simNgspice;
    const auto restore = qScopeGuard([settingsSimulator] { QucsSettings.DefaultSimulator = settingsSimulator; });
    MatchDialog dialog(a_app);
    if (args.contains(QLatin1String("s"))) {
        // A two-port's S-parameters: both ports conjugately matched.
        const QJsonObject s = args.value(QLatin1String("s")).toObject();
        std::complex<double> s11, s12, s21, s22;
        if (!complexOf(s.value(QLatin1String("s11")), &s11) || !complexOf(s.value(QLatin1String("s12")), &s12)
            || !complexOf(s.value(QLatin1String("s21")), &s21) || !complexOf(s.value(QLatin1String("s22")), &s22))
            return errorResult(tr("'s' is a two-port's {\"s11\", \"s12\", \"s21\", \"s22\"}, each [re, im], {\"mag\", \"deg\"} or \"0.5-j0.2\"."));
        const std::complex<double> det = s11 * s22 - s12 * s21;
        const double delta = std::abs(det);
        const double k = (1 - std::norm(s11) - std::norm(s22) + delta * delta) / (2 * std::abs(s12 * s21));
        about.insert(QStringLiteral("stability"), QJsonObject{{QStringLiteral("K"), rounded(k)}, {QStringLiteral("|delta|"), rounded(delta)}});
        if (!(k > 1 && delta < 1))
            return errorResult(tr("The two-port is not unconditionally stable (K = %1, |delta| = %2): it cannot be conjugately matched. "
                                  "Losses or feedback that make K > 1 and |delta| < 1 would.").arg(k, 0, 'f', 3).arg(delta, 0, 'f', 3));
        text = dialog.designTwoPort(t, binomial, s11.real(), s11.imag(), s22.real(), s22.imag(), det.real(), det.imag(), z0, z2, f, micro,
                                    sp, open, substrate, order, ripple, balanced, &said);
    } else {
        std::complex<double> zl;
        if (!complexOf(args.value(QLatin1String("z_load")), &zl))
            return errorResult(tr("'z_load' is the load's impedance: \"10-j20\", [10, -20] or {\"re\": 10, \"im\": -20} (or 's' a two-port's S-parameters)."));
        // Its reflection against the source's reference, as the dialog takes it.
        const std::complex<double> gamma = (zl - z0) / (zl + z0);
        about.insert(QStringLiteral("z_load"), complexText(zl));
        about.insert(QStringLiteral("reflection"), QJsonArray{rounded(gamma.real()), rounded(gamma.imag())});
        text = dialog.designOnePort(t, binomial, gamma.real(), gamma.imag(), z0, f, micro, sp, open, substrate, order, ripple, balanced, &said);
    }
    if (text.isEmpty()) return errorResult(tr("No matching circuit: %1").arg(said.isEmpty() ? tr("it cannot be made with these values.") : said.join(QLatin1Char(' '))));
    QString error;
    QJsonObject placed = placeDesign(args, text, &error);
    if (placed.isEmpty()) return errorResult(error);
    about.insert(QStringLiteral("topology"), topologies.at(t));
    if (!said.isEmpty()) about.insert(QStringLiteral("said"), QJsonArray::fromStringList(said));
    placed.insert(QStringLiteral("matching"), about);
    return jsonResult(placed);
}

QJsonObject QucsControl::lineCalc(const QJsonObject& args)
{
    QJsonObject spec;
    for (const char* key : {"type", "do", "solve_for", "substrate", "f", "z0", "z0e", "z0o", "angle", "er", "mur", "h", "h_t", "t", "cond",
                            "sigma", "tand", "tanm", "rough", "w", "l", "s", "a", "b", "din", "dout"})
        if (args.contains(QLatin1String(key))) spec.insert(QLatin1String(key), args.value(QLatin1String(key)));
    QString error;
    const QJsonObject r = runToolProgram(QStringLiteral(QUCS_NAME "trans"), spec, &error);
    if (r.isEmpty()) return errorResult(tr("No line: %1").arg(error));
    return jsonResult(r);
}

namespace {

// A receiver stage as rxcalc takes it: its fields (one it has not, the
// schema's check refused before).
QJsonObject stageSpec(const QJsonObject& stage)
{
    QJsonObject out;
    for (const char* key : {"name", "gain", "nf", "iip3", "oip3", "ip1db", "op1db", "enabled"})
        if (stage.contains(QLatin1String(key))) out.insert(QLatin1String(key), stage.value(QLatin1String(key)));
    return out;
}

} // namespace

QJsonObject QucsControl::receiverBudget(const QJsonObject& args)
{
    QJsonObject spec;
    for (const char* key : {"input_power", "bandwidth", "snr_min", "temperature", "peak_to_average"})
        if (args.contains(QLatin1String(key))) spec.insert(QLatin1String(key), args.value(QLatin1String(key)));
    QJsonArray stages;
    QString error;
    for (const QJsonValue& v : args.value(QLatin1String("stages")).toArray()) {
        if (!v.isObject()) return errorResult(tr("Stage %1 is no object: {\"name\", \"gain\", \"nf\", ...}.").arg(stages.size() + 1));
        stages.append(stageSpec(v.toObject()));
    }
    spec.insert(QStringLiteral("stages"), stages);
    const QJsonObject r = runToolProgram(QStringLiteral("rxcalc"), spec, &error);
    if (r.isEmpty()) return errorResult(tr("No budget: %1").arg(error));
    return jsonResult(r);
}
