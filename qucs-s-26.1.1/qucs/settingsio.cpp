/*
 * The settings as a file: settingsio.h says what it holds and leaves out.
 */
#include "settingsio.h"

#include <QColor>
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>

#include <cmath>

#include "config.h"
#include "settings.h"

namespace qucs_s::settingsio {

namespace {

QString tr(const char* s) { return QCoreApplication::translate("SettingsIO", s); }

const QString kFormat = QStringLiteral("Qucs-S settings");
const QString kSettings = QStringLiteral("settings");
const QString kClaudeMode = QStringLiteral("ClaudeCode/permissionMode");
constexpr int kBackupsKept = 10;
constexpr qint64 kLargestFile = 16 * 1024 * 1024;

QString g_backupDirectory;

// The programs Qucs-S runs, as the settings name them: an import that
// changes them says so first.
struct Program {
    const char* key;
    const char* label;
};
const Program kPrograms[] = {
    {"NgspiceExecutable", QT_TRANSLATE_NOOP("SettingsIO", "ngspice")},
    {"XyceExecutable", QT_TRANSLATE_NOOP("SettingsIO", "Xyce")},
    {"XyceParExecutable", QT_TRANSLATE_NOOP("SettingsIO", "Xyce (parallel)")},
    {"SpiceOpusExecutable", QT_TRANSLATE_NOOP("SettingsIO", "SPICE OPUS")},
    {"Qucsator", QT_TRANSLATE_NOOP("SettingsIO", "Qucsator")},
    {"OctaveExecutable", QT_TRANSLATE_NOOP("SettingsIO", "Octave")},
    {"OpenVAFExecutable", QT_TRANSLATE_NOOP("SettingsIO", "OpenVAF")},
    {"PythonExecutable", QT_TRANSLATE_NOOP("SettingsIO", "Python")},
    {"RFLayoutExecutable", QT_TRANSLATE_NOOP("SettingsIO", "RF Layout")},
    {"AdmsXmlBinDir", QT_TRANSLATE_NOOP("SettingsIO", "ADMS folder")},
    {"AscoBinDir", QT_TRANSLATE_NOOP("SettingsIO", "ASCO folder")},
    {"Editor", QT_TRANSLATE_NOOP("SettingsIO", "Text editor")},
    {"ClaudeCode/program", QT_TRANSLATE_NOOP("SettingsIO", "Claude Code")},
    {"FileTypes", QT_TRANSLATE_NOOP("SettingsIO", "File types")},
};

// The store, this application's alone: on macOS the native store also
// hands out the keys of the system's global domain unless told not to.
struct Store : QucsSettingsFile {
    Store() { setFallbacksEnabled(false); }
};

// ---- values ----------------------------------------------------------

QJsonValue encoded(const QVariant& value)
{
    switch (value.typeId()) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int: case QMetaType::UInt: case QMetaType::LongLong: case QMetaType::ULongLong:
    case QMetaType::Short: case QMetaType::UShort: case QMetaType::Long: case QMetaType::ULong:
        return value.toLongLong();
    case QMetaType::Double: case QMetaType::Float:
        return value.toDouble();
    case QMetaType::QString:
        return value.toString();
    case QMetaType::QStringList:
        return QJsonArray::fromStringList(value.toStringList());
    case QMetaType::QByteArray:
        return QJsonObject{{QStringLiteral("bytes"), QString::fromLatin1(value.toByteArray().toBase64())}};
    case QMetaType::QColor:
        return QJsonObject{{QStringLiteral("color"), value.value<QColor>().name(QColor::HexArgb)}};
    case QMetaType::QVariantList: {
        // A list of text (some stores give a QStringList back so).
        const QVariantList list = value.toList();
        QJsonArray texts;
        for (const QVariant& each : list) {
            if (each.typeId() != QMetaType::QString) {
                texts = {};
                break;
            }
            texts.append(each.toString());
        }
        if (texts.size() == list.size()) return texts;
        break;
    }
    default:
        break;
    }
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << value;
    return QJsonObject{{QStringLiteral("variant"), QString::fromLatin1(bytes.toBase64())}};
}

bool decoded(const QJsonValue& json, QVariant& value)
{
    switch (json.type()) {
    case QJsonValue::Bool:
        value = json.toBool();
        return true;
    case QJsonValue::Double: {
        const double d = json.toDouble();
        if (d == std::floor(d) && std::abs(d) <= 2147483647.0) value = int(d);
        else if (d == std::floor(d) && std::abs(d) <= 9007199254740992.0) value = qlonglong(d);
        else value = d;
        return true;
    }
    case QJsonValue::String:
        value = json.toString();
        return true;
    case QJsonValue::Array: {
        QStringList texts;
        for (const QJsonValue& each : json.toArray()) {
            if (!each.isString()) return false;
            texts << each.toString();
        }
        value = texts;
        return true;
    }
    case QJsonValue::Object: {
        const QJsonObject o = json.toObject();
        if (o.size() != 1) return false;
        if (o.contains(QStringLiteral("bytes"))) {
            const auto result = QByteArray::fromBase64Encoding(o.value(QStringLiteral("bytes")).toString().toLatin1(),
                                                               QByteArray::AbortOnBase64DecodingErrors);
            if (!result) return false;
            value = *result;
            return true;
        }
        if (o.contains(QStringLiteral("color"))) {
            const QColor color(o.value(QStringLiteral("color")).toString());
            if (!color.isValid()) return false;
            value = color;
            return true;
        }
        if (o.contains(QStringLiteral("variant"))) {
            const auto bytes = QByteArray::fromBase64Encoding(o.value(QStringLiteral("variant")).toString().toLatin1(),
                                                              QByteArray::AbortOnBase64DecodingErrors);
            if (!bytes) return false;
            QDataStream in(*bytes);
            in.setVersion(QDataStream::Qt_6_0);
            QVariant v;
            in >> v;
            if (in.status() != QDataStream::Ok || !v.isValid() || !in.atEnd()) return false;
            value = v;
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

// ---- this computer ---------------------------------------------------

// A path of the computer the file was made on: "/usr/bin/x", "C:\x", "\\server\x".
bool isAbsolutePath(const QString& text)
{
    static const QRegularExpression drive(QStringLiteral("^[A-Za-z]:[\\\\/]"));
    return text.startsWith(QLatin1Char('/')) || text.startsWith(QStringLiteral("\\\\"))
        || drive.match(text).hasMatch();
}

bool here(const QString& path)
{
    // A Windows path elsewhere is not one here (QFileInfo would take
    // "C:\x" for a relative file name).
    if (!QDir::isAbsolutePath(QDir::fromNativeSeparators(path))) return false;
    return QFileInfo::exists(path);
}

QString describe(const QJsonObject& doc)
{
    QStringList parts;
    const QString by = doc.value(QStringLiteral("exported by")).toString();
    if (!by.isEmpty()) parts << by;
    const QString platform = doc.value(QStringLiteral("platform")).toString();
    if (!platform.isEmpty()) parts << platform;
    const QDateTime when = QDateTime::fromString(doc.value(QStringLiteral("exported")).toString(), Qt::ISODate);
    if (when.isValid()) parts << QLocale().toString(when.toLocalTime(), QLocale::ShortFormat);
    return parts.join(QStringLiteral(", "));
}

QString shown(const QVariant& value)
{
    return value.typeId() == QMetaType::QStringList ? value.toStringList().join(QStringLiteral(", "))
                                                     : value.toString();
}

} // namespace

// ---- what a file holds -------------------------------------------------

bool isState(const QString& key)
{
    static const QSet<QString> keys = {
        QStringLiteral("firstRun"),
        QStringLiteral("RecentDocs"),
        QStringLiteral("RecentProjects"),
        QStringLiteral("ClaudeCode/exportFolder"),   // the folder a conversation went to last
        QStringLiteral("ClaudeCode/models"),         // what the program here offers
        QStringLiteral("ClaudeCode/otherModels"),    // the models chosen by name last
        QStringLiteral("ClaudeCode/slashCommands"),  // the program's commands, as it last said
    };
    if (keys.contains(key)) return true;
    // What the file browser showed last.
    if (key.startsWith(QStringLiteral("FileBrowser/"))) return true;
    // Where the windows were.
    const QString last = key.section(QLatin1Char('/'), -1);
    return last.endsWith(QStringLiteral("geometry"), Qt::CaseInsensitive)
        || last.compare(QStringLiteral("state"), Qt::CaseInsensitive) == 0;
}

QJsonObject toJson()
{
    const Store store;
    QJsonObject settings;
    for (const QString& key : store.allKeys()) {
        if (isState(key)) continue;
        const QVariant value = store.value(key);
        if (value.isValid()) settings.insert(key, encoded(value));
    }
    QJsonObject doc;
    doc.insert(kFormat, FormatVersion);
    doc.insert(QStringLiteral("exported by"), QStringLiteral("Qucs-S " PACKAGE_VERSION));
    doc.insert(QStringLiteral("exported"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    doc.insert(QStringLiteral("platform"),
               QStringLiteral("%1 (%2)").arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture()));
    doc.insert(kSettings, settings);
    return doc;
}

bool exportTo(const QString& path, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

// ---- taking one in ---------------------------------------------------

bool read(const QString& path, Import& out, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    if (file.size() > kLargestFile) {
        if (error) *error = tr("It is too large for a settings file.");
        return false;
    }
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = tr("It is not a Qucs-S settings file: it is not JSON (%1).").arg(parse.errorString());
        return false;
    }
    return fromJson(doc.object(), out, error);
}

bool fromJson(const QJsonObject& doc, Import& out, QString* error)
{
    out = Import();
    if (!doc.value(kFormat).isDouble() || !doc.value(kSettings).isObject()) {
        if (error) *error = tr("It is not a Qucs-S settings file (made by File > Export Settings).");
        return false;
    }
    const int version = doc.value(kFormat).toInt();
    if (version < 1 || version > FormatVersion) {
        if (error)
            *error = tr("It was made by a newer Qucs-S (%1), in a format this one does not know. "
                        "Import it with that version or a later one.")
                         .arg(doc.value(QStringLiteral("exported by")).toString());
        return false;
    }
    out.source = describe(doc);

    const QJsonObject settings = doc.value(kSettings).toObject();
    QMap<QString, QVariant> wanted;
    QSet<QString> left;   // the file's settings not taken: this computer's stay
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (isState(it.key())) continue;   // a file edited by hand may carry some
        QVariant value;
        if (!decoded(it.value(), value)) {
            out.kept << tr("%1: its value is not one Qucs-S understands").arg(it.key());
            left.insert(it.key());
            continue;
        }
        wanted.insert(it.key(), value);
    }
    out.count = int(wanted.size() + left.size());

    // The folders searched for subcircuits: those on this computer, in
    // their order.
    static const QRegularExpression pathEntry(QStringLiteral("^Paths/(\\d+)/path$"));
    if (wanted.contains(QStringLiteral("Paths/size"))) {
        const int size = wanted.value(QStringLiteral("Paths/size")).toInt();
        QStringList folders;
        for (int i = 1; i <= size; ++i) {
            const QString folder = wanted.value(QStringLiteral("Paths/%1/path").arg(i)).toString();
            if (folder.isEmpty()) continue;
            if (here(folder)) folders << folder;
            else out.kept << tr("Paths: %1 is not on this computer, left out").arg(QDir::toNativeSeparators(folder));
        }
        for (auto it = wanted.begin(); it != wanted.end();) {
            if (pathEntry.match(it.key()).hasMatch()) it = wanted.erase(it);
            else ++it;
        }
        wanted.insert(QStringLiteral("Paths/size"), int(folders.size()));
        for (int i = 0; i < folders.size(); ++i)
            wanted.insert(QStringLiteral("Paths/%1/path").arg(i + 1), folders.at(i));
    }

    // Programs and folders of the computer it was made on: taken when
    // they are on this one too.
    for (auto it = wanted.begin(); it != wanted.end();) {
        const QString text = it.value().typeId() == QMetaType::QString ? it.value().toString() : QString();
        if (!pathEntry.match(it.key()).hasMatch() && isAbsolutePath(text) && !here(text)) {
            out.kept << tr("%1: %2 is not on this computer").arg(it.key(), QDir::toNativeSeparators(text));
            left.insert(it.key());
            it = wanted.erase(it);
        } else {
            ++it;
        }
    }

    // Claude allowed to do anything without asking: chosen here, after a
    // warning, not brought in by a file.
    if (wanted.value(kClaudeMode).toString() == QLatin1String("bypassPermissions")) {
        out.kept << tr("%1: Bypass Permissions - Claude would do anything without asking; choose it in the "
                       "Claude Code panel if you want it")
                        .arg(kClaudeMode);
        left.insert(kClaudeMode);
        wanted.remove(kClaudeMode);
    }

    const Store store;
    for (const Program& program : kPrograms) {
        const QString key = QString::fromLatin1(program.key);
        if (!wanted.contains(key)) continue;
        const QString value = shown(wanted.value(key));
        if (!value.isEmpty() && value != shown(store.value(key)))
            out.programs << QStringLiteral("%1: %2").arg(tr(program.label), value);
    }
    out.set = wanted;
    for (const QString& key : store.allKeys())
        if (!isState(key) && !wanted.contains(key) && !left.contains(key)) out.remove << key;
    return true;
}

void apply(const Import& import)
{
    Store store;
    for (const QString& key : import.remove) store.remove(key);
    for (auto it = import.set.cbegin(); it != import.set.cend(); ++it) store.setValue(it.key(), it.value());
    store.sync();
}

// ---- backups ---------------------------------------------------------

QString backupDirectory()
{
    if (!g_backupDirectory.isEmpty()) return g_backupDirectory;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.qucs-s");
    return dir + QStringLiteral("/settings-backups");
}

void setBackupDirectory(const QString& dir)
{
    g_backupDirectory = dir;
}

QString backup(QString* error)
{
    const QDir dir(backupDirectory());
    if (!QDir().mkpath(dir.path())) {
        if (error) *error = tr("%1 cannot be made.").arg(QDir::toNativeSeparators(dir.path()));
        return {};
    }
    // Named by the time, to the millisecond: the names sort as the times.
    QDateTime when = QDateTime::currentDateTime();
    const auto nameAt = [&dir](const QDateTime& t) {
        return dir.filePath(QStringLiteral("settings-%1.json").arg(t.toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))));
    };
    QString path = nameAt(when);
    while (QFileInfo::exists(path)) path = nameAt(when = when.addMSecs(1));
    if (!exportTo(path, error)) return {};
    // The newest kept.
    QStringList all = dir.entryList({QStringLiteral("settings-*.json")}, QDir::Files, QDir::Name);
    while (all.size() > kBackupsKept) QFile::remove(dir.filePath(all.takeFirst()));
    return path;
}

} // namespace qucs_s::settingsio
