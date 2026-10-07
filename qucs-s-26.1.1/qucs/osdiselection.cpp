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

#include <QCryptographicHash>
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

namespace {

enum class Format { Unknown, Elf, MachO, Pe };
enum class Cpu { X86_64, Arm64, Other };

// What a binary is for: its format and the processors it has code for
// (several in a universal Mach-O; none known: empty).
struct Binary {
    Format format = Format::Unknown;
    QList<Cpu> cpus;
};

Binary binaryOf(const QString& path)
{
    Binary out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QByteArray head = f.read(4096);
    const auto byte = [&](int at) { return at < head.size() ? quint32(quint8(head.at(at))) : 0u; };
    const auto u16 = [&](int at, bool little) {
        return little ? byte(at) | byte(at + 1) << 8 : byte(at) << 8 | byte(at + 1);
    };
    const auto u32 = [&](int at, bool little) {
        return little ? u16(at, true) | u16(at + 2, true) << 16 : u16(at, false) << 16 | u16(at + 2, false);
    };
    const auto machCpu = [](quint32 type) {
        return type == 0x01000007u ? Cpu::X86_64 : type == 0x0100000cu ? Cpu::Arm64 : Cpu::Other;
    };
    if (head.size() >= 20 && head.startsWith("\x7f" "ELF")) {
        out.format = Format::Elf;
        const quint32 machine = u16(18, byte(5) == 1);   // EI_DATA: 1 little-endian
        out.cpus << (machine == 62 ? Cpu::X86_64 : machine == 183 ? Cpu::Arm64 : Cpu::Other);
    } else if (head.size() >= 8 && (u32(0, true) == 0xfeedfacfu || u32(0, true) == 0xfeedfaceu)) {
        out.format = Format::MachO;
        out.cpus << machCpu(u32(4, true));
    } else if (head.size() >= 8 && u32(0, false) == 0xcafebabeu) {
        // A universal Mach-O - if the count of its parts is one (Java class
        // files begin so too, and so do test files).
        const quint32 count = u32(4, false);
        if (count < 1 || count > 8 || head.size() < int(8 + 20 * count))
            return out;
        out.format = Format::MachO;
        for (quint32 i = 0; i < count; ++i)
            out.cpus << machCpu(u32(8 + 20 * int(i), false));
    } else if (head.size() >= 0x40 && head.startsWith("MZ")) {
        const int pe = int(u32(0x3c, true));
        if (pe > 0 && pe + 6 <= head.size() && head.mid(pe, 4) == QByteArray("PE\0\0", 4)) {
            out.format = Format::Pe;
            const quint32 machine = u16(pe + 4, true);
            out.cpus << (machine == 0x8664 ? Cpu::X86_64 : machine == 0xaa64 ? Cpu::Arm64 : Cpu::Other);
        }
    }
    return out;
}

// What this Qucs-S was built for. On macOS without its processor: one of
// either runs the other's code (Rosetta), so that says nothing about what
// the simulator loads.
Binary thisBuild()
{
    Binary out;
#if defined(Q_OS_MACOS)
    out.format = Format::MachO;
    return out;
#elif defined(Q_OS_WIN)
    out.format = Format::Pe;
#else
    out.format = Format::Elf;
#endif
#if defined(Q_PROCESSOR_ARM_64)
    out.cpus << Cpu::Arm64;
#elif defined(Q_PROCESSOR_X86_64)
    out.cpus << Cpu::X86_64;
#endif
    return out;
}

} // namespace

bool builtForAnotherPlatform(const QString& file, const QString& simulator)
{
    const Binary library = binaryOf(file);
    if (library.format == Format::Unknown)
        return false;
    Binary host = simulator.isEmpty() ? Binary() : binaryOf(simulator);
    if (host.format == Format::Unknown)
        host = thisBuild();   // a script, or not found
    if (library.format != host.format)
        return true;
    if (library.cpus.contains(Cpu::Other) || host.cpus.isEmpty() || host.cpus.contains(Cpu::Other))
        return false;   // nothing to tell by
    for (Cpu cpu : library.cpus)
        if (host.cpus.contains(cpu))
            return false;
    return true;
}

bool defines(const QString& osdiFile, const QString& module)
{
    bool readable = false;
    const QStringList modules = modulesOf(osdiFile, &readable);
    return readable ? modules.contains(module.toLower()) : holdsName(osdiFile, module);
}

namespace {
// One file by two names (a link, ..): their real paths, else as given.
bool sameFile(const QString& a, const QString& b)
{
    const QString ra = QFileInfo(a).canonicalFilePath(), rb = QFileInfo(b).canonicalFilePath();
    return !ra.isEmpty() && !rb.isEmpty() ? ra == rb : QFileInfo(a).absoluteFilePath() == QFileInfo(b).absoluteFilePath();
}
} // namespace

QStringList needed(const QStringList& osdiFiles, const QSet<QString>& types, QStringList* notes, const QStringList& preferred)
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
        // Of those a placed part's own library brings, when one has it: a
        // project file that happens to define the module too was taken when
        // built last, and the part ran another model.
        QStringList among;
        for (const QString& file : candidates)
            if (std::any_of(preferred.cbegin(), preferred.cend(), [&](const QString& p) { return sameFile(p, file); }))
                among << file;
        if (among.isEmpty()) among = candidates;
        // A library already loaded has it; else the one with the most of
        // what is still needed, then the most recently built (the first
        // of those built at the same time).
        QString pick;
        for (const QString& file : among)
            if (chosen.contains(file)) {
                pick = file;
                break;
            }
        if (pick.isEmpty()) {
            const auto uncovered = [&](const QString& file) { return (has.value(file) - covered).size(); };
            pick = among.first();
            for (const QString& file : among) {
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
        // Through a link (a library's source linked into a project), from
        // where the file is, as OpenVAF takes it.
        const QFileInfo info(file);
        const QDir folder = info.isSymLink() ? QFileInfo(info.symLinkTarget()).absoluteDir() : info.absoluteDir();
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

QStringList missingSourceIncludes(const QString& vaFile)
{
    static const QStringList ownHeaders{QStringLiteral("disciplines.vams"), QStringLiteral("constants.vams"),
                                        QStringLiteral("discipline.h"), QStringLiteral("constants.h")};
    QStringList missing;
    QStringList files{QFileInfo(vaFile).absoluteFilePath()};
    files << sourceIncludes(vaFile);
    for (const QString& file : std::as_const(files)) {
        const QFileInfo info(file);
        const QDir folder = info.isSymLink() ? QFileInfo(info.symLinkTarget()).absoluteDir() : info.absoluteDir();
        for (const QString& name : readSource(file).includes) {
            if (QFileInfo(folder.absoluteFilePath(name)).isFile()) continue;
            if (!name.contains(QLatin1Char('/')) && ownHeaders.contains(name, Qt::CaseInsensitive)) continue;
            if (!missing.contains(name)) missing << name;
        }
    }
    return missing;
}

namespace {

QString besideLibrary(const QFileInfo& source)
{
    return source.absoluteDir().absoluteFilePath(source.completeBaseName() + QStringLiteral(".osdi"));
}

// Its folder in the cache: one for each source, by where it is - a link
// by where the link is (a library's source linked into a project is the
// project's: its model is not the one compiled for the library's file).
QString cachedLibrary(const QFileInfo& source, const QString& cacheDir)
{
    const QString real = source.isSymLink() ? QString() : source.canonicalFilePath();
    const QByteArray key = QCryptographicHash::hash((real.isEmpty() ? source.absoluteFilePath() : real).toUtf8(),
                                                    QCryptographicHash::Sha1).toHex().left(10);
    const QString name = source.completeBaseName();
    return QDir(cacheDir).absoluteFilePath(QStringLiteral("osdi/%1-%2/%1.osdi").arg(name, QString::fromLatin1(key)));
}

// Built after the source and the files it includes.
bool current(const QString& library, const QString& vaFile)
{
    QDateTime newest = QFileInfo(vaFile).lastModified();
    for (const QString& included : sourceIncludes(vaFile))
        newest = std::max(newest, QFileInfo(included).lastModified());
    return QFileInfo(library).lastModified() >= newest;
}

} // namespace

QString buildTarget(const QString& vaFile, const QString& cacheDir, const QString& simulator, Into* into)
{
    const QFileInfo source(vaFile);
    const QString beside = besideLibrary(source);
    const QFileInfo there(beside);
    Into where = Into::Beside;
    if (!cacheDir.isEmpty()) {
        if (!QFileInfo(source.absolutePath()).isWritable() || (there.exists() && !there.isWritable()))
            where = Into::CacheReadOnly;
        else if (there.isFile() && builtForAnotherPlatform(beside, simulator))
            where = Into::CacheKeepsForeign;
    }
    if (into)
        *into = where;
    return where == Into::Beside ? beside : cachedLibrary(source, cacheDir);
}

QStringList compileArguments(const QString& vaFile, const QString& library)
{
    const QFileInfo info(vaFile);
    const QString target = library.isEmpty() ? besideLibrary(info) : QFileInfo(library).absoluteFilePath();
    QString input = info.absoluteFilePath();
    if (info.isSymLink() && !info.canonicalFilePath().isEmpty()) input = info.canonicalFilePath();
    QStringList arguments{input};
    if (QDir::cleanPath(target) != QDir::cleanPath(besideLibrary(QFileInfo(input))))
        arguments << QStringLiteral("-o") << target;
    return arguments;
}

QString modelOf(const QString& vaFile, const QString& cacheDir, const QString& simulator)
{
    const QFileInfo source(vaFile);
    const QString beside = besideLibrary(source);
    QStringList candidates{beside};
    if (!cacheDir.isEmpty())
        candidates << cachedLibrary(source, cacheDir);
    QStringList loadable;
    for (const QString& file : std::as_const(candidates))
        if (QFileInfo(file).isFile() && !builtForAnotherPlatform(file, simulator))
            loadable << file;
    for (const QString& file : std::as_const(loadable))
        if (current(file, vaFile))
            return file;
    if (!loadable.isEmpty())
        return loadable.first();
    return QFileInfo(beside).isFile() ? beside : QString();
}

QList<Build> builds(const QStringList& vaFiles, const QStringList& osdiFiles, const QSet<QString>& types,
                    const QString& simulator, const QString& cacheDir)
{
    QList<Build> out;
    for (const QString& va : vaFiles) {
        Build build;
        for (const QString& module : readSource(va).modules)
            if (types.contains(module))
                build.modules << module;
        if (build.modules.isEmpty())
            continue;
        build.source = QFileInfo(va).absoluteFilePath();
        build.library = buildTarget(va, cacheDir, simulator, &build.into);
        const QString model = modelOf(va, cacheDir, simulator);
        if (!model.isEmpty()) {
            build.built = model;
            if (builtForAnotherPlatform(model, simulator)) {
                build.foreign = true;
                out << build;
            } else if (!current(model, va)) {
                out << build;   // older than the source, or than a file it includes
            }
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
