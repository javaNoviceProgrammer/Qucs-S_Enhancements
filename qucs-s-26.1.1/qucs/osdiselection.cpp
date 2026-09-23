/*
 * osdiselection.cpp - which OSDI libraries a netlist needs
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "osdiselection.h"
#include "vamodule.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>

#include <algorithm>

namespace qucs_s::osdi {

namespace {

// A file as it was when it was read: read again when it has changed.
struct Stamp {
    QDateTime modified;
    qint64 size = -1;

    static Stamp of(const QFileInfo& info) { return {info.lastModified(), info.size()}; }
    bool operator==(const Stamp& other) const { return modified == other.modified && size == other.size; }
};

struct Library {
    Stamp stamp;
    bool readable = false;
    QStringList modules;
};

struct SpiceFile {
    Stamp stamp;
    QSet<QString> types;
    QStringList includes;
};

struct Source {
    Stamp stamp;
    QStringList modules;    // lower case
    QStringList includes;   // as written
};

QMutex cacheMutex;
QHash<QString, Library> libraries;
QHash<QString, SpiceFile> spiceFiles;
QHash<QString, Source> sources;

// A Verilog-A source's modules and `include lines, cached.
Source readSource(const QString& path)
{
    const QFileInfo info(path);
    const Stamp stamp = Stamp::of(info);
    const QString key = info.absoluteFilePath();
    {
        QMutexLocker lock(&cacheMutex);
        auto it = sources.constFind(key);
        if (it != sources.constEnd() && it->stamp == stamp)
            return *it;
    }
    Source read;
    read.stamp = stamp;
    QFile file(key);
    if (file.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(file.readAll());
        for (const QString& name : vamodule::sourceModules(text))
            read.modules << name.toLower();
        static const QRegularExpression include(QStringLiteral(R"re(^\s*`include\s+"([^"]+)")re"),
                                                QRegularExpression::MultilineOption);
        for (auto it = include.globalMatch(text); it.hasNext();)
            read.includes << it.next().captured(1);
    }
    QMutexLocker lock(&cacheMutex);
    sources.insert(key, read);
    return read;
}

// A SPICE text as its logical lines: a "+" line continues the one before,
// a "*" line is a comment.
QStringList logicalLines(const QString& spice)
{
    QStringList lines;
    const QStringList physical = spice.split(QLatin1Char('\n'));
    for (QString line : physical) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('*')))
            continue;
        if (trimmed.startsWith(QLatin1Char('+'))) {
            if (!lines.isEmpty())
                lines.last() += QLatin1Char(' ') + trimmed.mid(1);
            continue;
        }
        lines << trimmed;
    }
    return lines;
}

// A library file's text, cached with what it names.
const SpiceFile* readSpiceFile(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isFile())
        return nullptr;
    const Stamp stamp = Stamp::of(info);
    auto it = spiceFiles.find(path);
    if (it != spiceFiles.end() && it->stamp == stamp)
        return &*it;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return nullptr;
    const QString text = QString::fromUtf8(file.readAll());
    SpiceFile read;
    read.stamp = stamp;
    read.types = modelTypes(text);
    read.includes = includedFiles(text, info.absolutePath());
    return &*spiceFiles.insert(path, read);
}

// The library as read (and remembered while it is unchanged).
Library readLibrary(const QString& osdiFile)
{
    const QFileInfo info(osdiFile);
    const Stamp stamp = Stamp::of(info);
    const QString key = info.absoluteFilePath();
    {
        QMutexLocker lock(&cacheMutex);
        auto it = libraries.constFind(key);
        if (it != libraries.constEnd() && it->stamp == stamp)
            return *it;
    }
    Library read;
    read.stamp = stamp;
    QStringList names;
    if (info.isFile() && vamodule::osdiModules(key, &names)) {
        read.readable = true;
        for (const QString& name : names)
            read.modules << name.toLower();
    }
    QMutexLocker lock(&cacheMutex);
    libraries.insert(key, read);
    return read;
}

// A library that cannot be loaded here (built for another architecture):
// does it hold the name as a string of its own - a NUL on each side, any
// case?
bool holdsName(const QString& osdiFile, const QString& module)
{
    QFile file(osdiFile);
    if (module.isEmpty() || !file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = file.readAll().toLower();
    const QByteArray name = '\0' + module.toLower().toUtf8() + '\0';
    return bytes.contains(name);
}

} // namespace

QSet<QString> modelTypes(const QString& spice)
{
    static const QRegularExpression card(QStringLiteral(R"(^\.model\s+\S+\s+([A-Za-z_][\w$]*))"),
                                         QRegularExpression::CaseInsensitiveOption);
    QSet<QString> types;
    for (const QString& line : logicalLines(spice)) {
        const QRegularExpressionMatch m = card.match(line);
        if (m.hasMatch())
            types.insert(m.captured(1).toLower());
    }
    return types;
}

QStringList includedFiles(const QString& spice, const QString& baseDir)
{
    static const QRegularExpression include(
        QStringLiteral(R"re(^\.(?:include|inc|lib)\s+(?:"([^"]+)"|'([^']+)'|(\S+)))re"),
        QRegularExpression::CaseInsensitiveOption);
    const QDir base(baseDir);
    QStringList files;
    for (const QString& line : logicalLines(spice)) {
        const QRegularExpressionMatch m = include.match(line);
        if (!m.hasMatch())
            continue;
        QString file = m.captured(1);
        if (file.isEmpty()) file = m.captured(2);
        if (file.isEmpty()) file = m.captured(3);
        file = QDir::cleanPath(base.absoluteFilePath(file));
        if (!files.contains(file))
            files << file;
    }
    return files;
}

QSet<QString> usedModelTypes(const QString& spice, const QString& baseDir)
{
    QSet<QString> types = modelTypes(spice);
    QStringList pending = includedFiles(spice, baseDir);
    QSet<QString> seen;
    QMutexLocker lock(&cacheMutex);
    // A library brings in others; a few hundred files is a PDK, more is a
    // loop the paths hide.
    while (!pending.isEmpty() && seen.size() < 1000) {
        const QString path = pending.takeFirst();
        const QString key = QFileInfo(path).canonicalFilePath();
        if (key.isEmpty() || seen.contains(key))
            continue;
        seen.insert(key);
        const SpiceFile* file = readSpiceFile(key);
        if (file == nullptr)
            continue;   // not there (a .lib line that names a section)
        types.unite(file->types);
        pending << file->includes;
    }
    return types;
}

QStringList modulesOf(const QString& osdiFile, bool* readable)
{
    const Library library = readLibrary(osdiFile);
    if (readable)
        *readable = library.readable;
    return library.modules;
}

bool defines(const QString& osdiFile, const QString& module)
{
    bool readable = false;
    const QStringList modules = modulesOf(osdiFile, &readable);
    return readable ? modules.contains(module.toLower()) : holdsName(osdiFile, module);
}

QStringList needed(const QStringList& osdiFiles, const QSet<QString>& types, QStringList* notes)
{
    QStringList sortedTypes(types.cbegin(), types.cend());
    sortedTypes.sort();
    // What each library has of what the netlist uses.
    QHash<QString, QSet<QString>> has;
    for (const QString& file : osdiFiles)
        for (const QString& type : sortedTypes)
            if (defines(file, type))
                has[file].insert(type);

    QStringList chosen;
    QSet<QString> covered;
    for (const QString& type : sortedTypes) {
        QStringList candidates;
        for (const QString& file : osdiFiles)
            if (has.value(file).contains(type) && !candidates.contains(file))
                candidates << file;
        if (candidates.isEmpty())
            continue;   // a built-in device, a code model - or a module no library has
        // A library already loaded has it; else the one with the most of
        // what is still needed, then the most recently built (the first
        // of those built at the same time).
        QString pick;
        for (const QString& file : candidates)
            if (chosen.contains(file)) {
                pick = file;
                break;
            }
        if (pick.isEmpty()) {
            const auto uncovered = [&](const QString& file) { return (has.value(file) - covered).size(); };
            pick = candidates.first();
            for (const QString& file : candidates) {
                const int more = uncovered(file) - uncovered(pick);
                if (more > 0 || (more == 0 && QFileInfo(file).lastModified() > QFileInfo(pick).lastModified()))
                    pick = file;
            }
            chosen << pick;
            covered.unite(has.value(pick));
        }
        if (notes && candidates.size() > 1) {
            QStringList others = candidates;
            others.removeAll(pick);
            for (QString& other : others)
                other = QDir::toNativeSeparators(other);
            *notes << QStringLiteral("%1 from %2, not from %3")
                          .arg(type, QDir::toNativeSeparators(pick), others.join(QStringLiteral(", ")));
        }
    }
    QStringList out;
    for (const QString& file : osdiFiles)
        if (chosen.contains(file) && !out.contains(file))
            out << file;
    return out;
}

bool sourceDefines(const QString& vaFile, const QString& module)
{
    return readSource(vaFile).modules.contains(module.toLower());
}

QStringList sourceIncludes(const QString& vaFile)
{
    QStringList found;
    QStringList pending{QFileInfo(vaFile).absoluteFilePath()};
    QSet<QString> seen{pending.first()};
    while (!pending.isEmpty() && seen.size() < 500) {
        const QString file = pending.takeFirst();
        const QDir folder = QFileInfo(file).absoluteDir();
        for (const QString& name : readSource(file).includes) {
            const QFileInfo included(folder.absoluteFilePath(name));
            // "disciplines.vams" and the like come with OpenVAF.
            if (!included.isFile() || seen.contains(included.absoluteFilePath()))
                continue;
            seen.insert(included.absoluteFilePath());
            found << included.absoluteFilePath();
            pending << included.absoluteFilePath();
        }
    }
    return found;
}

QList<Build> builds(const QStringList& vaFiles, const QStringList& osdiFiles, const QSet<QString>& types)
{
    QList<Build> out;
    for (const QString& va : vaFiles) {
        Build build;
        for (const QString& module : readSource(va).modules)
            if (types.contains(module))
                build.modules << module;
        if (build.modules.isEmpty())
            continue;
        const QFileInfo source(va);
        build.source = source.absoluteFilePath();
        build.library = source.absoluteDir().absoluteFilePath(source.completeBaseName() + QStringLiteral(".osdi"));
        const QFileInfo library(build.library);
        if (library.isFile()) {
            // Older than the source, or than a file it includes.
            QDateTime newest = source.lastModified();
            for (const QString& included : sourceIncludes(va))
                newest = std::max(newest, QFileInfo(included).lastModified());
            if (library.lastModified() < newest)
                out << build;
            continue;
        }
        // No library of its own: built when no other has the modules.
        bool elsewhere = false;
        for (const QString& module : std::as_const(build.modules))
            for (const QString& file : osdiFiles)
                if (defines(file, module)) {
                    elsewhere = true;
                    break;
                }
        if (!elsewhere) {
            build.missing = true;
            out << build;
        }
    }
    return out;
}

} // namespace qucs_s::osdi
