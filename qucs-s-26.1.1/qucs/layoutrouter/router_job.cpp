/*
 * router_job.cpp - a routing job read from JSON or a CSV (router_job.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "router_job.h"
#include "router_json.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <cmath>

namespace qucs_s::router {

namespace {

constexpr double MostMicrometres = 1e7;
constexpr int MostNets = 100000;
constexpr int MostPoints = 100000;

bool coordinate(const QJsonValue& v, double* out)
{
    if (!v.isDouble()) return false;
    const double x = v.toDouble();
    if (!std::isfinite(x) || std::abs(x) > MostMicrometres) return false;
    *out = x;
    return true;
}

bool point(const QJsonValue& v, QPointF* p)
{
    const QJsonArray a = v.toArray();
    double x, y;
    if (!v.isArray() || a.size() != 2 || !coordinate(a.at(0), &x) || !coordinate(a.at(1), &y)) return false;
    *p = {x, y};
    return true;
}

bool points(const QJsonValue& v, QVector<QPointF>* out, int least, const QString& where, QString* error)
{
    if (!v.isArray() || v.toArray().size() < least || v.toArray().size() > MostPoints) {
        *error = QStringLiteral("%1 is a list of %2 points [x, y] in µm at least").arg(where).arg(least);
        return false;
    }
    for (const QJsonValue& p : v.toArray()) {
        QPointF q;
        if (!point(p, &q)) {
            *error = QStringLiteral("%1: each point is [x, y] in µm").arg(where);
            return false;
        }
        out->append(q);
    }
    return true;
}

bool names(const QJsonValue& v, QStringList* out, const QString& where, QString* error)
{
    if (v.isUndefined()) return true;
    if (!v.isArray() || v.toArray().size() > MostNets) {
        *error = QStringLiteral("%1 is a list of names").arg(where);
        return false;
    }
    for (const QJsonValue& n : v.toArray()) {
        if (!n.isString()) {
            *error = QStringLiteral("%1 is a list of names").arg(where);
            return false;
        }
        out->append(n.toString());
    }
    return true;
}

QString modeName(Terminal::Mode m)
{
    switch (m) {
    case Terminal::Mode::Exact: return QStringLiteral("exact");
    case Terminal::Mode::Anywhere: return QStringLiteral("anywhere");
    default: return QStringLiteral("centre");
    }
}

bool readMode(const QString& s, Terminal::Mode* m)
{
    if (s == QLatin1String("centre") || s == QLatin1String("center")) *m = Terminal::Mode::Centre;
    else if (s == QLatin1String("exact")) *m = Terminal::Mode::Exact;
    else if (s == QLatin1String("anywhere")) *m = Terminal::Mode::Anywhere;
    else return false;
    return true;
}

bool readTerminal(const QJsonValue& v, JobTerminal* t, const QString& where, QString* error)
{
    if (v.isArray()) {
        if (!point(v, &t->at)) {
            *error = where + QStringLiteral(" is [x, y] in µm, or {\"at\": [x, y]} or {\"text\": \"G1\"}");
            return false;
        }
        t->hasPoint = true;
        return true;
    }
    if (!v.isObject()) {
        *error = where + QStringLiteral(" is [x, y] in µm, or {\"at\": [x, y]} or {\"text\": \"G1\"}");
        return false;
    }
    const QJsonObject o = v.toObject();
    if (!onlyKeys(o, {"at", "text", "metal", "mode"}, where, error)) return false;
    if (o.contains(QLatin1String("at"))) {
        if (!point(o.value(QLatin1String("at")), &t->at)) {
            *error = where + QStringLiteral(": 'at' is [x, y] in µm");
            return false;
        }
        t->hasPoint = true;
    }
    if (o.contains(QLatin1String("text"))) {
        if (!o.value(QLatin1String("text")).isString() || o.value(QLatin1String("text")).toString().isEmpty()) {
            *error = where + QStringLiteral(": 'text' is a label's text");
            return false;
        }
        t->text = o.value(QLatin1String("text")).toString();
    }
    if (!t->hasPoint && t->text.isEmpty()) {
        *error = where + QStringLiteral(" needs 'at' or 'text'");
        return false;
    }
    if (o.contains(QLatin1String("metal"))) {
        if (!o.value(QLatin1String("metal")).isString()) {
            *error = where + QStringLiteral(": 'metal' is a metal's name");
            return false;
        }
        t->metal = o.value(QLatin1String("metal")).toString();
    }
    if (o.contains(QLatin1String("mode")) && !readMode(o.value(QLatin1String("mode")).toString(), &t->mode)) {
        *error = where + QStringLiteral(": 'mode' is centre, exact or anywhere");
        return false;
    }
    return true;
}

QJsonValue terminalJson(const JobTerminal& t)
{
    QJsonObject o;
    if (t.hasPoint) o.insert(QStringLiteral("at"), QJsonArray{t.at.x(), t.at.y()});
    if (!t.text.isEmpty()) o.insert(QStringLiteral("text"), t.text);
    if (!t.metal.isEmpty()) o.insert(QStringLiteral("metal"), t.metal);
    if (t.mode != Terminal::Mode::Centre) o.insert(QStringLiteral("mode"), modeName(t.mode));
    return o;
}

QJsonArray pointsJson(const QVector<QPointF>& ps)
{
    QJsonArray a;
    for (const QPointF& p : ps) a.append(QJsonArray{p.x(), p.y()});
    return a;
}

} // namespace

int Job::netIndex(const QString& name) const
{
    for (int i = 0; i < nets.size(); ++i)
        if (nets.at(i).name == name) return i;
    return -1;
}

bool readNet(const QJsonValue& v, JobNet* net, QString* error)
{
    if (!v.isObject()) {
        *error = QStringLiteral("A net is an object: {\"name\", \"start\", \"pads\"}");
        return false;
    }
    const QJsonObject o = v.toObject();
    const QString where = QStringLiteral("Net %1").arg(o.value(QLatin1String("name")).toString());
    if (!onlyKeys(o, {"name", "class", "start", "pads", "guide", "band", "fixed"}, where, error)) return false;
    if (!o.value(QLatin1String("name")).isString() || o.value(QLatin1String("name")).toString().trimmed().isEmpty()
        || o.value(QLatin1String("name")).toString().size() > 200) {
        *error = QStringLiteral("A net needs a 'name'");
        return false;
    }
    net->name = o.value(QLatin1String("name")).toString().trimmed();
    if (o.contains(QLatin1String("class"))) {
        if (!o.value(QLatin1String("class")).isString()) {
            *error = where + QStringLiteral(": 'class' is a class's name");
            return false;
        }
        net->netClass = o.value(QLatin1String("class")).toString();
    }
    JobTerminal start;
    if (!readTerminal(o.value(QLatin1String("start")), &start, where + QStringLiteral(": 'start'"), error)) return false;
    net->terminals.append(start);
    const QJsonValue pads = o.value(QLatin1String("pads"));
    if (!pads.isArray() || pads.toArray().isEmpty() || pads.toArray().size() > 1000) {
        *error = where + QStringLiteral(": 'pads' lists where it ends, one at least");
        return false;
    }
    for (const QJsonValue& p : pads.toArray()) {
        JobTerminal t;
        if (!readTerminal(p, &t, where + QStringLiteral(": a pad"), error)) return false;
        net->terminals.append(t);
    }
    if (o.contains(QLatin1String("guide")) && !points(o.value(QLatin1String("guide")), &net->guide, 2, where + QStringLiteral(": 'guide'"), error))
        return false;
    if (o.contains(QLatin1String("band"))) {
        if (!coordinate(o.value(QLatin1String("band")), &net->band) || net->band < 0) {
            *error = where + QStringLiteral(": 'band' is a length in µm");
            return false;
        }
    }
    if (o.contains(QLatin1String("fixed"))) {
        if (!o.value(QLatin1String("fixed")).isBool()) {
            *error = where + QStringLiteral(": 'fixed' is true or false");
            return false;
        }
        net->fixed = o.value(QLatin1String("fixed")).toBool();
    }
    return true;
}

QJsonObject netJson(const JobNet& net)
{
    QJsonObject o{{"name", net.name}};
    if (!net.netClass.isEmpty()) o.insert(QStringLiteral("class"), net.netClass);
    if (!net.terminals.isEmpty()) o.insert(QStringLiteral("start"), terminalJson(net.terminals.first()));
    QJsonArray pads;
    for (int i = 1; i < net.terminals.size(); ++i) pads.append(terminalJson(net.terminals.at(i)));
    o.insert(QStringLiteral("pads"), pads);
    if (!net.guide.isEmpty()) o.insert(QStringLiteral("guide"), pointsJson(net.guide));
    if (net.band > 0) o.insert(QStringLiteral("band"), net.band);
    if (net.fixed) o.insert(QStringLiteral("fixed"), true);
    return o;
}

bool readKeepOut(const QJsonValue& v, JobKeepOut* k, QString* error)
{
    if (!v.isObject()) {
        *error = QStringLiteral("A keep-out is an object: {\"name\", \"outline\" or \"box\", \"metals\", \"except_nets\", \"let_through\"}");
        return false;
    }
    const QJsonObject o = v.toObject();
    const QString where = QStringLiteral("Keep-out %1").arg(o.value(QLatin1String("name")).toString());
    if (!onlyKeys(o, {"name", "outline", "box", "metals", "except_nets", "let_through"}, where, error)) return false;
    k->name = o.value(QLatin1String("name")).toString();
    if (o.contains(QLatin1String("outline")) == o.contains(QLatin1String("box"))) {
        *error = where + QStringLiteral(" needs an 'outline' or a 'box', one of them");
        return false;
    }
    if (o.contains(QLatin1String("box"))) {
        const QJsonArray b = o.value(QLatin1String("box")).toArray();
        double x0, y0, x1, y1;
        if (b.size() != 4 || !coordinate(b.at(0), &x0) || !coordinate(b.at(1), &y0) || !coordinate(b.at(2), &x1) || !coordinate(b.at(3), &y1)
            || x1 <= x0 || y1 <= y0) {
            *error = where + QStringLiteral(": 'box' is [x0, y0, x1, y1] in µm, x0 < x1 and y0 < y1");
            return false;
        }
        k->outline = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    } else if (!points(o.value(QLatin1String("outline")), &k->outline, 3, where + QStringLiteral(": 'outline'"), error)) {
        return false;
    }
    if (!names(o.value(QLatin1String("metals")), &k->metals, where + QStringLiteral(": 'metals'"), error)) return false;
    if (!names(o.value(QLatin1String("except_nets")), &k->except, where + QStringLiteral(": 'except_nets'"), error)) return false;
    if (o.contains(QLatin1String("let_through"))) {
        const QString l = o.value(QLatin1String("let_through")).toString();
        if (l == QLatin1String("way_in")) k->letThrough = KeepOut::LetThrough::WayIn;
        else if (l == QLatin1String("free")) k->letThrough = KeepOut::LetThrough::Free;
        else if (l == QLatin1String("none")) k->letThrough = KeepOut::LetThrough::None;
        else {
            *error = where + QStringLiteral(": 'let_through' is way_in, free or none");
            return false;
        }
    }
    return true;
}

QJsonObject keepOutJson(const JobKeepOut& k)
{
    QJsonObject o{{"name", k.name}, {"outline", pointsJson(k.outline)}};
    if (!k.metals.isEmpty()) o.insert(QStringLiteral("metals"), QJsonArray::fromStringList(k.metals));
    if (!k.except.isEmpty()) o.insert(QStringLiteral("except_nets"), QJsonArray::fromStringList(k.except));
    if (k.letThrough != KeepOut::LetThrough::WayIn)
        o.insert(QStringLiteral("let_through"), k.letThrough == KeepOut::LetThrough::Free ? "free" : "none");
    return o;
}

bool readOptions(const QJsonValue& v, Options* options, QString* error)
{
    if (v.isUndefined()) return true;
    if (!v.isObject()) {
        *error = QStringLiteral("'options' is an object");
        return false;
    }
    const QJsonObject o = v.toObject();
    if (!onlyKeys(o, {"passes", "stall_passes", "threads", "max_memory_mb", "negotiate", "search_weight"}, QStringLiteral("'options'"), error)) return false;
    const auto integer = [&](const char* key, int lo, int hi, auto* out) {
        const QJsonValue x = o.value(QLatin1String(key));
        if (x.isUndefined()) return true;
        if (!x.isDouble() || x.toDouble() != std::floor(x.toDouble()) || x.toDouble() < lo || x.toDouble() > hi) {
            *error = QStringLiteral("'options': '%1' is a whole number from %2 to %3").arg(QLatin1String(key)).arg(lo).arg(hi);
            return false;
        }
        *out = decltype(+*out)(x.toDouble());
        return true;
    };
    if (!integer("passes", 1, 1000, &options->passes) || !integer("stall_passes", 0, 1000, &options->stallPasses)
        || !integer("threads", 0, 256, &options->threads) || !integer("max_memory_mb", 16, 1 << 22, &options->maxMemory))
        return false;
    if (o.contains(QLatin1String("search_weight"))) {
        const QJsonValue w = o.value(QLatin1String("search_weight"));
        if (!w.isDouble() || !std::isfinite(w.toDouble()) || w.toDouble() < 1 || w.toDouble() > 3) {
            *error = QStringLiteral("'options': 'search_weight' is a number from 1 to 3");
            return false;
        }
        options->searchWeight = w.toDouble();
    }
    if (o.contains(QLatin1String("negotiate"))) {
        if (!o.value(QLatin1String("negotiate")).isBool()) {
            *error = QStringLiteral("'options': 'negotiate' is true or false");
            return false;
        }
        options->negotiate = o.value(QLatin1String("negotiate")).toBool();
    }
    return true;
}

QJsonObject optionsJson(const Options& options)
{
    return QJsonObject{{"passes", options.passes},
                       {"stall_passes", options.stallPasses},
                       {"threads", options.threads},
                       {"max_memory_mb", double(options.maxMemory)},
                       {"negotiate", options.negotiate},
                       {"search_weight", options.searchWeight}};
}

Job readJob(const QJsonObject& o, QString* error)
{
    Job job;
    const auto fail = [&](const QString& why) {
        *error = why;
        return Job();
    };
    QString e;
    if (!onlyKeys(o, {"layout", "cell", "technology", "nets", "keep_outs", "options", "route_cell", "comment"}, QStringLiteral("The job"), &e))
        return fail(e);
    job.layout = o.value(QLatin1String("layout")).toString();
    job.cell = o.value(QLatin1String("cell")).toString();
    const QJsonValue tech = o.value(QLatin1String("technology"));
    if (tech.isString()) job.technologyFile = tech.toString();
    else if (tech.isObject()) job.technology = tech.toObject();
    else if (!tech.isUndefined()) return fail(QStringLiteral("The job: 'technology' is a file or the technology itself"));
    const QJsonValue nets = o.value(QLatin1String("nets"));
    if (!nets.isUndefined() && (!nets.isArray() || nets.toArray().size() > MostNets))
        return fail(QStringLiteral("The job: 'nets' is a list of nets"));
    for (const QJsonValue& n : nets.toArray()) {
        JobNet net;
        if (!readNet(n, &net, &e)) return fail(e);
        if (job.netIndex(net.name) >= 0) return fail(QStringLiteral("Two nets are named %1").arg(net.name));
        job.nets.append(net);
    }
    const QJsonValue keeps = o.value(QLatin1String("keep_outs"));
    if (!keeps.isUndefined() && (!keeps.isArray() || keeps.toArray().size() > 10000))
        return fail(QStringLiteral("The job: 'keep_outs' is a list of keep-outs"));
    for (const QJsonValue& k : keeps.toArray()) {
        JobKeepOut keep;
        if (!readKeepOut(k, &keep, &e)) return fail(e);
        job.keepOuts.append(keep);
    }
    if (!readOptions(o.value(QLatin1String("options")), &job.options, &e)) return fail(e);
    if (o.contains(QLatin1String("route_cell"))) {
        const QString c = o.value(QLatin1String("route_cell")).toString().trimmed();
        if (c.isEmpty() || c.size() > 120 || c.contains(QRegularExpression(QStringLiteral("[^A-Za-z0-9_$?.-]"))))
            return fail(QStringLiteral("The job: 'route_cell' is a cell name of letters, digits and _$?.-"));
        job.routeCell = c;
    }
    error->clear();
    return job;
}

QJsonObject jobJson(const Job& job)
{
    QJsonObject o;
    if (!job.layout.isEmpty()) o.insert(QStringLiteral("layout"), job.layout);
    if (!job.cell.isEmpty()) o.insert(QStringLiteral("cell"), job.cell);
    if (!job.technologyFile.isEmpty()) o.insert(QStringLiteral("technology"), job.technologyFile);
    else if (!job.technology.isEmpty()) o.insert(QStringLiteral("technology"), job.technology);
    QJsonArray nets, keeps;
    for (const JobNet& n : job.nets) nets.append(netJson(n));
    for (const JobKeepOut& k : job.keepOuts) keeps.append(keepOutJson(k));
    o.insert(QStringLiteral("nets"), nets);
    if (!keeps.isEmpty()) o.insert(QStringLiteral("keep_outs"), keeps);
    o.insert(QStringLiteral("options"), optionsJson(job.options));
    o.insert(QStringLiteral("route_cell"), job.routeCell);
    return o;
}

namespace {

// One CSV line's fields: commas (or semicolons, or tabs, as the first
// line has), quotes doubled within quotes.
QStringList csvFields(const QString& line, QChar sep)
{
    QStringList out;
    QString f;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) f += c, ++i;
                else quoted = false;
            } else {
                f += c;
            }
        } else if (c == QLatin1Char('"')) {
            quoted = true;
        } else if (c == sep) {
            out << f.trimmed();
            f.clear();
        } else {
            f += c;
        }
        if (out.size() > 64) break;
    }
    out << f.trimmed();
    return out;
}

bool number(const QString& s, double* x)
{
    bool ok = false;
    *x = s.toDouble(&ok);
    return ok && std::isfinite(*x) && std::abs(*x) <= MostMicrometres;
}

} // namespace

QVector<JobNet> readNetsCsv(const QString& text, QString* error)
{
    QVector<JobNet> nets;
    QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
    if (lines.size() > 200000) {
        *error = QStringLiteral("The CSV has more rows than a net list would (200 000 at most)");
        return {};
    }
    lines.removeIf([](const QString& l) { return l.trimmed().isEmpty() || l.trimmed().startsWith(QLatin1Char('#')); });
    if (lines.isEmpty()) {
        *error = QStringLiteral("The CSV has no rows");
        return {};
    }
    const QString first = lines.first();
    const QChar sep = first.contains(QLatin1Char('\t')) ? QLatin1Char('\t') : (first.count(QLatin1Char(';')) > first.count(QLatin1Char(',')) ? QLatin1Char(';') : QLatin1Char(','));
    QStringList header = csvFields(first, sep);
    for (QString& h : header) h = h.toLower().replace(QLatin1Char(' '), QLatin1Char('_'));
    const bool named = header.contains(QStringLiteral("net"));
    const auto col = [&](const char* n) { return int(header.indexOf(QLatin1String(n))); };
    QHash<QString, int> byName;
    const auto netFor = [&](const QString& name) -> JobNet* {
        const auto it = byName.constFind(name);
        if (it != byName.constEnd()) return &nets[*it];
        JobNet n;
        n.name = name;
        byName.insert(name, int(nets.size()));
        nets.append(n);
        return &nets.last();
    };
    for (int r = named ? 1 : 0; r < lines.size(); ++r) {
        const QStringList f = csvFields(lines.at(r), sep);
        const QString row = QStringLiteral("Row %1").arg(r + 1);
        const auto field = [&](int c) { return c >= 0 && c < f.size() ? f.at(c) : QString(); };
        if (named) {
            const QString name = field(col("net"));
            if (name.isEmpty() || name.size() > 200) {
                *error = row + QStringLiteral(" has no net name");
                return {};
            }
            JobNet* net = netFor(name);
            if (!field(col("class")).isEmpty()) net->netClass = field(col("class"));
            const auto terminal = [&](const char* prefix, JobTerminal* t) -> int {
                const QString xs = field(col((QByteArray(prefix) + "_x").constData()));
                const QString ys = field(col((QByteArray(prefix) + "_y").constData()));
                t->text = field(col((QByteArray(prefix) + "_text").constData()));
                t->metal = field(col((QByteArray(prefix) + "_layer").constData()));
                if (!field(col("mode")).isEmpty() && !readMode(field(col("mode")), &t->mode)) return -1;
                if (xs.isEmpty() && ys.isEmpty()) return t->text.isEmpty() ? 0 : 1;
                double x, y;
                if (!number(xs, &x) || !number(ys, &y)) return -1;
                t->at = {x, y};
                t->hasPoint = true;
                return 1;
            };
            JobTerminal start, pad;
            const int s = terminal("start", &start), p = terminal("pad", &pad);
            if (s < 0 || p < 0) {
                *error = row + QStringLiteral(": a terminal's x and y are numbers in µm (and 'mode' centre, exact or anywhere)");
                return {};
            }
            if (s > 0) {
                if (!net->terminals.isEmpty() && net->terminals.size() >= 1 && (net->terminals.first().hasPoint || !net->terminals.first().text.isEmpty())) {
                    *error = row + QStringLiteral(": net %1 has its start already").arg(name);
                    return {};
                }
                if (net->terminals.isEmpty()) net->terminals.append(start);
                else net->terminals.first() = start;
            }
            if (p > 0) {
                if (net->terminals.isEmpty()) net->terminals.append(JobTerminal());   // its start may come on a later row
                net->terminals.append(pad);
            }
        } else {
            JobNet net;
            net.name = f.value(0);
            if (net.name.isEmpty() || net.name.size() > 200) {
                *error = row + QStringLiteral(" has no net name");
                return {};
            }
            double sx, sy, px, py;
            if (f.size() >= 5 && number(f.at(1), &sx) && number(f.at(2), &sy) && number(f.at(3), &px) && number(f.at(4), &py)) {
                JobTerminal s, p;
                s.at = {sx, sy};
                s.hasPoint = true;
                p.at = {px, py};
                p.hasPoint = true;
                net.terminals = {s, p};
                net.netClass = f.value(5);
            } else if (f.size() >= 3 && f.size() <= 4 && !f.at(1).isEmpty() && !f.at(2).isEmpty()) {
                JobTerminal s, p;
                s.text = f.at(1);
                p.text = f.at(2);
                net.terminals = {s, p};
                net.netClass = f.value(3);
            } else {
                *error = row + QStringLiteral(" is neither net, start x, start y, pad x, pad y[, class] nor net, start label, pad label[, class]");
                return {};
            }
            if (byName.contains(net.name)) {
                *error = row + QStringLiteral(": net %1 is given twice (a header row lets a net have more pads)").arg(net.name);
                return {};
            }
            byName.insert(net.name, int(nets.size()));
            nets.append(net);
        }
        if (nets.size() > MostNets) {
            *error = QStringLiteral("The CSV has more than %1 nets").arg(MostNets);
            return {};
        }
    }
    for (const JobNet& n : std::as_const(nets)) {
        if (n.terminals.size() < 2 || (!n.terminals.first().hasPoint && n.terminals.first().text.isEmpty())) {
            *error = QStringLiteral("Net %1 needs a start and a pad").arg(n.name);
            return {};
        }
    }
    error->clear();
    return nets;
}

} // namespace qucs_s::router
