/*
 * schematicdiff.cpp - what changed in a schematic, part by part
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "schematicdiff.h"
#include "gitstatus.h"

#include "components/component.h"
#include "module.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QPoint>
#include <QRegularExpression>

#include <memory>

namespace qucs_s::git {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("SchematicDiff", text);
}

// A part's line: "<R R1 1 140 150 -28 -51 1 0 "100k" 1 ... >".
struct Part {
    QString model;
    QString name;        // "*" for a part without one (a ground)
    int active = 1;
    QPoint at;
    QPoint text;
    int mirror = 0;
    int rotate = 0;
    QStringList values;  // the quoted values, in order
};

struct Parsed {
    QString version;
    QMap<QString, QString> settings;            // the <Properties>, View= left out
    QList<std::pair<QString, Part>> parts;      // by key, in order
    QStringList wires;
    QStringList labels;
    QList<std::pair<QString, QString>> diagrams;   // "Rect at 100,500" and its block
    QStringList paintings;
    QStringList symbol;
};

// A line's fields, a quoted value as one (its quotes kept).
QStringList fieldsOf(const QString& line)
{
    QString t = line.trimmed();
    if (t.startsWith(QLatin1Char('<'))) t.remove(0, 1);
    if (t.endsWith(QLatin1Char('>'))) t.chop(1);
    QStringList out;
    QString field;
    bool quoted = false, any = false;
    for (const QChar c : std::as_const(t)) {
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
            field += c;
            any = true;
        } else if (c == QLatin1Char(' ') && !quoted) {
            if (any) out << field;
            field.clear();
            any = false;
        } else {
            field += c;
            any = true;
        }
    }
    if (any) out << field;
    return out;
}

bool isQuoted(const QString& f)
{
    return f.size() >= 2 && f.startsWith(QLatin1Char('"')) && f.endsWith(QLatin1Char('"'));
}

Parsed parse(const QString& text)
{
    Parsed p;
    QString section;
    QString diagramKey, diagramText, diagramEnd;
    QHash<QString, int> seen;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        if (p.version.isEmpty() && line.startsWith(QLatin1String("<Qucs Schematic "))) {
            p.version = line.mid(16).chopped(line.endsWith(QLatin1Char('>')) ? 1 : 0);
            continue;
        }
        if (section.isEmpty()) {
            if (line.startsWith(QLatin1Char('<')) && !line.startsWith(QLatin1String("</")) && !line.contains(QLatin1Char(' ')))
                section = line.sliced(1, std::max<qsizetype>(0, line.size() - 2));   // (a line "<" alone: none)
            continue;
        }
        if (line == QStringLiteral("</%1>").arg(section)) {
            section.clear();
            continue;
        }
        if (section == QLatin1String("Properties")) {
            // (">" alone: nothing after its first character to cut the last from.)
            const QString body = line.sliced(1, std::max<qsizetype>(0, line.size() - 1 - (line.endsWith(QLatin1Char('>')) ? 1 : 0)));
            const QString key = body.section(QLatin1Char('='), 0, 0);
            if (key != QLatin1String("View")) p.settings.insert(key, body.section(QLatin1Char('='), 1));
        } else if (section == QLatin1String("Components")) {
            const QStringList f = fieldsOf(line);
            if (f.size() < 9) continue;
            Part part;
            part.model = f.at(0);
            part.name = f.at(1);
            part.active = f.at(2).toInt();
            part.at = QPoint(f.at(3).toInt(), f.at(4).toInt());
            part.text = QPoint(f.at(5).toInt(), f.at(6).toInt());
            part.mirror = f.at(7).toInt();
            part.rotate = f.at(8).toInt();
            for (qsizetype i = 9; i < f.size(); ++i)
                if (isQuoted(f.at(i))) part.values << f.at(i).mid(1).chopped(1);
            // A part without a name (a ground) by where it is; a name given
            // twice, the second as name#2.
            QString key = part.name.isEmpty() || part.name == QLatin1String("*")
                              ? QStringLiteral("%1 at %2,%3").arg(part.model).arg(part.at.x()).arg(part.at.y())
                              : part.name;
            if (const int n = ++seen[key]; n > 1) key += QStringLiteral("#%1").arg(n);
            p.parts.append({key, part});
        } else if (section == QLatin1String("Wires")) {
            p.wires << line;
            const QStringList f = fieldsOf(line);
            if (f.size() > 4 && isQuoted(f.at(4)) && f.at(4).size() > 2) p.labels << f.at(4).mid(1).chopped(1);
        } else if (section == QLatin1String("Diagrams")) {
            if (diagramKey.isEmpty()) {
                const QStringList f = fieldsOf(line);
                if (f.size() < 3) continue;
                diagramKey = QStringLiteral("%1 at %2,%3").arg(f.at(0), f.at(1), f.at(2));
                diagramText = line;
                diagramEnd = QStringLiteral("</%1>").arg(f.at(0));
                continue;
            }
            diagramText += QLatin1Char('\n') + line;
            if (line == diagramEnd) {
                p.diagrams.append({diagramKey, diagramText});
                diagramKey.clear();
            }
        } else if (section == QLatin1String("Paintings")) {
            p.paintings << line;
        } else if (section == QLatin1String("Symbol")) {
            p.symbol << line;
        }
    }
    return p;
}

// Of two lists, what is in one and not the other, as many times as more.
std::pair<int, int> addedRemoved(const QStringList& before, const QStringList& after)
{
    QHash<QString, int> count;
    for (const QString& s : before) --count[s];
    for (const QString& s : after) ++count[s];
    int added = 0, removed = 0;
    for (auto it = count.cbegin(); it != count.cend(); ++it) {
        if (it.value() > 0) added += it.value();
        else removed -= it.value();
    }
    return {added, removed};
}

QString quoted(const QString& value)
{
    return QLatin1Char('"') + value + QLatin1Char('"');
}

// "2 added, 1 removed" of \a what.
QString counted(const QString& what, int added, int removed)
{
    QStringList said;
    if (added > 0) said << tr("%1 added").arg(added);
    if (removed > 0) said << tr("%1 removed").arg(removed);
    return said.isEmpty() ? QString() : QStringLiteral("%1: %2").arg(what, said.join(QStringLiteral(", ")));
}

// A part's property names, as its type has them (empty: not known here).
class Names
{
public:
    QStringList of(const QString& model)
    {
        if (const auto it = a_names.constFind(model); it != a_names.cend()) return *it;
        QStringList names;
        if (Module::Modules.contains(model))
            if (std::unique_ptr<Component> c{Module::getComponent(model)})
                for (const Property* p : c->Props) names << p->Name;
        a_names.insert(model, names);
        return names;
    }

private:
    QHash<QString, QStringList> a_names;
};

// What changed in one part's values: by the type's names where it has
// them, an equation's by its variable ("y=..."), else by place.
QStringList valuesChanged(const Part& was, const Part& now, Names& names)
{
    QStringList said;
    static const QRegularExpression named(QStringLiteral("^\\s*([A-Za-z_][\\w.]*)\\s*=(.*)$"));
    // "name=value" values (equations): by name.
    QMap<QString, QString> before, after;
    QStringList restBefore, restAfter;
    for (const QString& v : was.values) {
        const auto m = named.match(v);
        if (m.hasMatch() && !before.contains(m.captured(1))) before.insert(m.captured(1), m.captured(2).trimmed());
        else restBefore << v;
    }
    for (const QString& v : now.values) {
        const auto m = named.match(v);
        if (m.hasMatch() && !after.contains(m.captured(1))) after.insert(m.captured(1), m.captured(2).trimmed());
        else restAfter << v;
    }
    if (!before.isEmpty() || !after.isEmpty()) {
        for (auto it = before.cbegin(); it != before.cend(); ++it) {
            if (!after.contains(it.key())) said << tr("%1 removed").arg(it.key());
            else if (after.value(it.key()) != it.value())
                said << QStringLiteral("%1 %2 → %3").arg(it.key(), quoted(it.value()), quoted(after.value(it.key())));
        }
        for (auto it = after.cbegin(); it != after.cend(); ++it)
            if (!before.contains(it.key())) said << tr("%1 added (%2)").arg(it.key(), quoted(it.value()));
    } else {
        restBefore = was.values;
        restAfter = now.values;
    }
    // The others by place, named as the type names them.
    const QStringList propertyNames = before.isEmpty() && after.isEmpty() && was.model == now.model ? names.of(now.model) : QStringList();
    const qsizetype common = std::min(restBefore.size(), restAfter.size());
    for (qsizetype i = 0; i < common; ++i) {
        if (restBefore.at(i) == restAfter.at(i)) continue;
        const QString label = i < propertyNames.size() ? propertyNames.at(i) : tr("value %1").arg(i + 1);
        said << QStringLiteral("%1 %2 → %3").arg(label, quoted(restBefore.at(i)), quoted(restAfter.at(i)));
    }
    for (qsizetype i = common; i < restAfter.size(); ++i) said << tr("%1 added").arg(quoted(restAfter.at(i)));
    for (qsizetype i = common; i < restBefore.size(); ++i) said << tr("%1 removed").arg(quoted(restBefore.at(i)));
    return said;
}

QString activeText(int active)
{
    switch (active) {
    case 0: return tr("deactivated");
    case 2: return tr("shorted");
    default: return tr("activated");
    }
}

std::optional<QString> onDisk(const QString& root, const QString& rel)
{
    // (A file: a link's target may be a pipe, or a device with no end.)
    const QFileInfo info(fileIn(root, rel));
    if (info.isSymLink() || !info.isFile()) return std::nullopt;
    QFile f(info.filePath());
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    return QString::fromUtf8(f.readAll());
}

bool inScope(const QString& rel, const QString& scope)
{
    return scope.isEmpty() || scope == QLatin1String(".") || rel == scope || rel.startsWith(scope + QLatin1Char('/'));
}

QString block(const QStringList& files)
{
    if (files.isEmpty()) return {};
    return tr("Schematic changes, part by part:") + QLatin1Char('\n') + files.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString fileBlock(const QString& name, const QStringList& lines)
{
    return name + QLatin1Char(':') + QLatin1Char('\n') + QStringLiteral("  ") + lines.join(QStringLiteral("\n  "));
}

} // namespace

bool isSchematicFile(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("sch") || suffix == QLatin1String("dpl") || suffix == QLatin1String("sym");
}

QStringList schematicChanges(const std::optional<QString>& beforeText, const std::optional<QString>& afterText)
{
    QStringList said;
    if (!beforeText.has_value() && !afterText.has_value()) return said;
    if (!afterText.has_value()) return {tr("deleted")};
    const Parsed now = parse(*afterText);
    if (!beforeText.has_value()) {
        said << tr("new: %1 parts, %2 wires, %3 diagrams").arg(now.parts.size()).arg(now.wires.size()).arg(now.diagrams.size());
        return said;
    }
    const Parsed was = parse(*beforeText);
    Names names;

    // The parts: removed, added, changed - in the file's order.
    QHash<QString, Part> before, after;
    for (const auto& [key, part] : was.parts) before.insert(key, part);
    for (const auto& [key, part] : now.parts) after.insert(key, part);
    const auto title = [](const QString& key, const Part& p) {
        return key.contains(QLatin1String(" at ")) ? key : QStringLiteral("%1 (%2)").arg(key, p.model);
    };
    for (const auto& [key, part] : was.parts)
        if (!after.contains(key)) said << tr("%1 removed").arg(title(key, part));
    for (const auto& [key, part] : now.parts) {
        const auto it = before.constFind(key);
        if (it == before.cend()) {
            said << (key.contains(QLatin1String(" at ")) ? tr("%1 added").arg(key)
                                                          : tr("%1 added at %2,%3").arg(title(key, part)).arg(part.at.x()).arg(part.at.y()));
            continue;
        }
        const Part& old = *it;
        QStringList changes;
        if (old.model != part.model) changes << tr("type %1 → %2").arg(old.model, part.model);
        changes << valuesChanged(old, part, names);
        if (old.at != part.at)
            changes << tr("moved from %1,%2 to %3,%4").arg(old.at.x()).arg(old.at.y()).arg(part.at.x()).arg(part.at.y());
        if (old.rotate != part.rotate) changes << tr("turned");
        if (old.mirror != part.mirror) changes << (part.mirror != 0 ? tr("mirrored") : tr("mirrored back"));
        if (old.active != part.active) changes << activeText(part.active);
        if (old.text != part.text && changes.isEmpty()) changes << tr("its texts moved");
        if (!changes.isEmpty()) said << QStringLiteral("%1: %2").arg(title(key, part), changes.join(QStringLiteral("; ")));
    }
    // The wires and labels.
    {
        const auto [added, removed] = addedRemoved(was.wires, now.wires);
        if (const QString w = counted(tr("wires"), added, removed); !w.isEmpty()) said << w;
        QStringList labels;
        for (const QString& l : now.labels)
            if (!was.labels.contains(l)) labels << tr("%1 added").arg(l);
        for (const QString& l : was.labels)
            if (!now.labels.contains(l)) labels << tr("%1 removed").arg(l);
        if (!labels.isEmpty()) said << tr("labels: %1").arg(labels.join(QStringLiteral(", ")));
    }
    // The diagrams, by type and place.
    {
        QHash<QString, QString> d0, d1;
        for (const auto& [key, text] : was.diagrams) d0.insert(key, text);
        for (const auto& [key, text] : now.diagrams) d1.insert(key, text);
        for (const auto& [key, text] : was.diagrams)
            if (!d1.contains(key)) said << tr("diagram %1 removed").arg(key);
        for (const auto& [key, text] : now.diagrams) {
            if (!d0.contains(key)) said << tr("diagram %1 added").arg(key);
            else if (d0.value(key) != text) said << tr("diagram %1 changed").arg(key);
        }
    }
    {
        const auto [added, removed] = addedRemoved(was.paintings, now.paintings);
        if (const QString p = counted(tr("paintings"), added, removed); !p.isEmpty()) said << p;
    }
    {
        const auto [added, removed] = addedRemoved(was.symbol, now.symbol);
        if (const QString p = counted(tr("symbol's paintings"), added, removed); !p.isEmpty()) said << p;
    }
    // The settings (not the view), and who wrote it.
    for (auto it = was.settings.cbegin(); it != was.settings.cend(); ++it) {
        if (!now.settings.contains(it.key())) said << tr("setting %1 removed").arg(it.key());
        else if (now.settings.value(it.key()) != it.value())
            said << tr("setting %1: %2 → %3").arg(it.key(), quoted(it.value()), quoted(now.settings.value(it.key())));
    }
    for (auto it = now.settings.cbegin(); it != now.settings.cend(); ++it)
        if (!was.settings.contains(it.key())) said << tr("setting %1 added: %2").arg(it.key(), quoted(it.value()));
    if (!said.isEmpty() && was.version != now.version && !was.version.isEmpty())
        said << tr("written by Qucs-S %1 (was %2)").arg(now.version, was.version);
    return said;
}

namespace {

// What \a specs hold in one git, as text; none for a spec "" (no such side).
QList<std::optional<QString>> texts(const QString& root, const QStringList& specs)
{
    QStringList asked;
    for (const QString& spec : specs)
        if (!spec.isEmpty()) asked << spec;
    const QList<std::optional<QByteArray>> found = blobs(root, asked);
    QList<std::optional<QString>> out;
    qsizetype at = 0;
    for (const QString& spec : specs) {
        if (spec.isEmpty() || at >= found.size() || !found.at(at).has_value()) out << std::nullopt;
        else out << QString::fromUtf8(*found.at(at));
        if (!spec.isEmpty()) ++at;
    }
    return out;
}

} // namespace

QString schematicChangesOf(const QString& root, const QString& path, DiffOf of, const Repository* known)
{
    const Repository repo = known != nullptr ? *known : read(root);
    if (!repo.valid) return {};
    bool inside = true;
    const QString scope = path.isEmpty() ? QString() : relativePath(root, path, &inside);
    if (!inside) return {};
    // Each file's two sides; those git holds asked for in one program (twenty
    // schematics changed took 42 of them).
    struct One {
        QString name, disk;
        int before = -1, after = -1;
    };
    QList<One> ones;
    QStringList specs;
    const auto spec = [&specs](const QString& s) {
        specs << s;
        return int(specs.size()) - 1;
    };
    for (const Entry& e : repo.entries) {
        if (e.ignored || e.conflicted || e.isFolder() || !isSchematicFile(e.path) || !inScope(e.path, scope)) continue;
        const QString was = e.from.isEmpty() ? e.path : e.from;
        One one;
        one.name = e.from.isEmpty() ? e.path : QStringLiteral("%1 → %2").arg(e.from, e.path);
        switch (of) {
        case DiffOf::Head:
            if (!e.untracked) one.before = spec(QStringLiteral("HEAD:") + was);
            one.disk = e.path;
            break;
        case DiffOf::Staged:
            if (!e.hasStaged()) continue;
            one.before = spec(QStringLiteral("HEAD:") + was);
            one.after = spec(QStringLiteral(":") + e.path);
            break;
        case DiffOf::Unstaged:
            if (!e.hasUnstaged()) continue;
            if (!e.untracked) one.before = spec(QStringLiteral(":") + e.path);
            one.disk = e.path;
            break;
        }
        ones << one;
    }
    const QList<std::optional<QString>> held = texts(root, specs);
    const auto heldAt = [&held](int i) { return i >= 0 && i < held.size() ? held.at(i) : std::optional<QString>(); };
    QStringList files;
    for (const One& one : std::as_const(ones)) {
        const QStringList lines = schematicChanges(heldAt(one.before), one.disk.isEmpty() ? heldAt(one.after) : onDisk(root, one.disk));
        if (!lines.isEmpty()) files << fileBlock(one.name, lines);
    }
    return block(files);
}

QString schematicChangesIn(const QString& root, const QString& commit, const QString& path)
{
    if (commit.isEmpty() || commit.startsWith(QLatin1Char('-'))) return {};
    bool inside = true;
    const QString scope = path.isEmpty() ? QString() : relativePath(root, path, &inside);
    if (!inside) return {};
    // (Its files as the History lists them: any name, read whole - git's plain
    // lines quote one with an accent, and it would be no schematic - and a
    // merge's against its first parent, where plain diff-tree says nothing.)
    QList<ChangedFile> changed;
    QStringList specs;
    for (const ChangedFile& f : changedFiles(root, commit)) {
        const QString was = f.from.isEmpty() ? f.path : f.from;
        if (!isSchematicFile(f.path) && !isSchematicFile(was)) continue;
        if (!inScope(f.path, scope) && !inScope(was, scope)) continue;
        changed << f;
        specs << (f.status == QLatin1Char('A') ? QString() : commit + QStringLiteral("^:") + was);
        specs << (f.status == QLatin1Char('D') ? QString() : commit + QLatin1Char(':') + f.path);
    }
    const QList<std::optional<QString>> held = texts(root, specs);
    QStringList files;
    for (qsizetype i = 0; i < changed.size() && 2 * i + 1 < held.size(); ++i) {
        const ChangedFile& f = changed.at(i);
        const QString was = f.from.isEmpty() ? f.path : f.from;
        const QStringList lines = schematicChanges(held.at(2 * i), held.at(2 * i + 1));
        if (!lines.isEmpty()) files << fileBlock(was == f.path ? f.path : QStringLiteral("%1 → %2").arg(was, f.path), lines);
    }
    return block(files);
}

QString schematicConflictChanges(const ConflictVersions& v)
{
    if (!v.inConflict) return {};
    QStringList parts;
    if (v.base.has_value()) {
        const QStringList mine = schematicChanges(v.base, v.ours), theirs = schematicChanges(v.base, v.theirs);
        parts << fileBlock(tr("mine, against where both started"), mine.isEmpty() ? QStringList{tr("nothing")} : mine);
        parts << fileBlock(tr("theirs, against where both started"), theirs.isEmpty() ? QStringList{tr("nothing")} : theirs);
    } else {
        const QStringList lines = schematicChanges(v.ours, v.theirs);
        parts << fileBlock(tr("theirs, against mine (no version both started from)"), lines.isEmpty() ? QStringList{tr("nothing")} : lines);
    }
    return parts.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace qucs_s::git
