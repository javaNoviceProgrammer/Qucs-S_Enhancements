/*
 * vamodule.cpp - a Verilog-A module as a Qucs component describes it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "vamodule.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibrary>
#include <QRegularExpression>

#include <cstdlib>
#include <memory>

#include "osdi/osdi_0_3.h"

namespace qucs_s::vamodule {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("VerilogA", text);
}

// Models may log while they are set up; there is no simulator to tell.
void quietLog(void* /*handle*/, char* /*message*/, uint32_t /*level*/) {}

// Verilog-A without its comments ("//" to the end of the line, "/* */"),
// strings and attributes ("(* *)") kept.
QString withoutComments(const QString& source)
{
    QString out;
    out.reserve(source.size());
    for (qsizetype i = 0; i < source.size(); ++i) {
        const QChar c = source.at(i);
        const QChar next = i + 1 < source.size() ? source.at(i + 1) : QChar();
        if (c == '"') {   // a string, escapes and all
            qsizetype j = i + 1;
            while (j < source.size() && source.at(j) != '"') j += source.at(j) == '\\' ? 2 : 1;
            out += source.mid(i, j - i + 1);
            i = j;
        } else if (c == '/' && next == '/') {
            while (i < source.size() && source.at(i) != '\n') ++i;
            out += '\n';
        } else if (c == '/' && next == '*') {
            const qsizetype end = source.indexOf(QLatin1String("*/"), i + 2);
            i = end < 0 ? source.size() : end + 1;
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

// The attributes of "(* desc="...", units="Ohm" *)".
QHash<QString, QString> attributes(const QString& text)
{
    static const QRegularExpression pair(QStringLiteral(R"re(([A-Za-z_]\w*)\s*=\s*"((?:[^"\\]|\\.)*)")re"));
    QHash<QString, QString> found;
    for (auto it = pair.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        QString value = m.captured(2);
        value.replace(QLatin1String("\\\""), QLatin1String("\"")).replace(QLatin1String("\\\\"), QLatin1String("\\"));
        found.insert(m.captured(1).toLower(), value);
    }
    return found;
}

// Splits at the commas that are not inside brackets or strings.
QStringList splitTopLevel(const QString& text)
{
    QStringList parts;
    int depth = 0;
    bool quoted = false;
    qsizetype start = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == '\\') ++i;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            --depth;
        } else if (c == ',' && depth == 0) {
            parts << text.mid(start, i - start);
            start = i + 1;
        }
    }
    parts << text.mid(start);
    return parts;
}

} // namespace

bool readOsdi(const QString& osdiFile, const QString& wanted, VerilogModule* module, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    const QString shown = QDir::toNativeSeparators(osdiFile);
    QLibrary library(osdiFile);
    if (!library.load()) return fail(tr("%1 cannot be loaded: %2").arg(shown, library.errorString()));
    struct Unload {
        QLibrary& library;
        ~Unload() { library.unload(); }
    } unload{library};

    const auto* major = reinterpret_cast<const uint32_t*>(library.resolve("OSDI_VERSION_MAJOR"));
    const auto* minor = reinterpret_cast<const uint32_t*>(library.resolve("OSDI_VERSION_MINOR"));
    if (major != nullptr && minor != nullptr && (*major != 0 || *minor < 3))
        return fail(tr("%1 is OSDI %2.%3; OSDI 0.3 and later are read.").arg(shown).arg(*major).arg(*minor));
    const auto* count = reinterpret_cast<const uint32_t*>(library.resolve("OSDI_NUM_DESCRIPTORS"));
    const auto* first = reinterpret_cast<const char*>(library.resolve("OSDI_DESCRIPTORS"));
    if (count == nullptr || first == nullptr || *count == 0)
        return fail(tr("%1 describes no module: it is not an OSDI library.").arg(shown));
    // OSDI 0.4 descriptors are larger: step by the size the library gives.
    const auto* size = reinterpret_cast<const uint32_t*>(library.resolve("OSDI_DESCRIPTOR_SIZE"));
    const size_t stride = size != nullptr && *size >= sizeof(OsdiDescriptor) ? *size : sizeof(OsdiDescriptor);
    const auto descriptorAt = [first, stride](uint32_t i) {
        return reinterpret_cast<const OsdiDescriptor*>(first + i * stride);
    };
    const OsdiDescriptor* d = descriptorAt(0);
    for (uint32_t i = 0; i < *count; ++i)
        if (descriptorAt(i)->name != nullptr
            && QString::fromUtf8(descriptorAt(i)->name).compare(wanted, Qt::CaseInsensitive) == 0) {
            d = descriptorAt(i);
            break;
        }
    if (d->access == nullptr || d->setup_model == nullptr || d->setup_instance == nullptr || d->param_opvar == nullptr)
        return fail(tr("%1 describes its module incompletely.").arg(shown));

    // The defaults are what setting a model and an instance up gives them.
    auto** log = reinterpret_cast<void**>(library.resolve("osdi_log"));
    void* savedLog = log != nullptr ? *log : nullptr;
    if (log != nullptr) *log = reinterpret_cast<void*>(&quietLog);
    char* noNames[] = {nullptr};
    OsdiSimParas params{};
    params.names = noNames;
    params.names_str = noNames;
    const std::unique_ptr<void, decltype(&std::free)> model(std::calloc(1, d->model_size + 1), &std::free);
    const std::unique_ptr<void, decltype(&std::free)> instance(std::calloc(1, d->instance_size + 1), &std::free);
    OsdiInitInfo info{};
    d->setup_model(nullptr, model.get(), &params, &info);
    std::free(info.errors);
    info = OsdiInitInfo{};
    d->setup_instance(nullptr, instance.get(), model.get(), 300.15, d->num_terminals, &params, &info);
    std::free(info.errors);

    VerilogModule read;
    read.name = QString::fromUtf8(d->name);
    for (uint32_t i = 0; i < d->num_params; ++i) {
        const OsdiParamOpvar& p = d->param_opvar[i];
        const QString name = p.name != nullptr && p.name[0] != nullptr ? QString::fromUtf8(p.name[0]) : QString();
        if (name.isEmpty() || name.startsWith('$')) continue;   // $mfactor: the simulator's
        Parameter out;
        out.name = name;
        out.description = QString::fromUtf8(p.description != nullptr ? p.description : "");
        out.units = QString::fromUtf8(p.units != nullptr ? p.units : "");
        out.instance = i < d->num_instance_params;
        void* value = d->access(instance.get(), model.get(), i,
                                out.instance ? ACCESS_FLAG_INSTANCE : ACCESS_FLAG_READ);
        if (value != nullptr && p.len == 0) {   // an array has no one default to show
            switch (p.flags & PARA_TY_MASK) {
            case PARA_TY_REAL:
                out.value = QString::number(*static_cast<const double*>(value), 'g', 15);
                break;
            case PARA_TY_INT:
                out.value = QString::number(*static_cast<const int32_t*>(value));
                break;
            case PARA_TY_STR: {   // the parameter holds a pointer to the text
                // Quoted, as in Verilog-A: the netlist's .model card needs
                // kind="nmos" (ngspice refuses kind=nmos).
                const char* text = *static_cast<char* const*>(value);
                QString quoted = text != nullptr ? QString::fromUtf8(text) : QString();
                quoted.replace('"', QLatin1String("\\\""));
                out.value = '"' + quoted + '"';
                break;
            }
            default:
                break;
            }
        }
        read.parameters << out;
    }
    if (log != nullptr) *log = savedLog;
    *module = read;
    return true;
}

VerilogModule readSource(const QString& source, const QString& wanted)
{
    const QString text = withoutComments(source);
    // "module NAME (ports);", "module NAME;", "module NAME #(...)".
    static const QRegularExpression moduleHead(QStringLiteral(R"(\b(?:macro)?module\s+([A-Za-z_]\w*)\s*(?=[(;#]))"));
    VerilogModule module;
    qsizetype bodyStart = -1;
    for (auto it = moduleHead.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        if (bodyStart < 0 || m.captured(1).compare(wanted, Qt::CaseInsensitive) == 0) {
            module.name = m.captured(1);
            bodyStart = m.capturedEnd();
            if (m.captured(1).compare(wanted, Qt::CaseInsensitive) == 0) break;
        }
    }
    if (bodyStart < 0) return module;
    static const QRegularExpression moduleEnd(QStringLiteral(R"(\bendmodule\b)"));
    const QRegularExpressionMatch end = moduleEnd.match(text, bodyStart);
    const QString body = text.mid(bodyStart, end.hasMatch() ? end.capturedStart() - bodyStart : -1);

    // "(* attributes *) parameter [real|integer|string] a = 1 [from ...], b = 2;"
    static const QRegularExpression declaration(
        QStringLiteral(R"((?:\(\*((?:(?!\*\)).)*)\*\)\s*)?(?<![\w`$])parameter\b\s*((?:[^;"]|"(?:[^"\\]|\\.)*")*);)"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression type(QStringLiteral(R"(^\s*(?:real|integer|string)\b)"));
    static const QRegularExpression assignment(QStringLiteral(R"(^\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)?=\s*(.*)$)"),
                                               QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression range(QStringLiteral(R"(\s+\b(?:from|exclude)\b.*$)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    for (auto it = declaration.globalMatch(body); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const QHash<QString, QString> attrs = attributes(m.captured(1));
        QString declared = m.captured(2);
        declared.remove(type);
        for (const QString& part : splitTopLevel(declared)) {
            const QRegularExpressionMatch a = assignment.match(part);
            if (!a.hasMatch()) continue;
            Parameter p;
            p.name = a.captured(1);
            p.value = a.captured(2).remove(range).simplified();
            p.description = attrs.value(QStringLiteral("desc"));
            p.units = attrs.value(QStringLiteral("units"));
            p.instance = attrs.value(QStringLiteral("type")).compare(QLatin1String("instance"), Qt::CaseInsensitive) == 0;
            module.parameters << p;
        }
    }
    return module;
}

QJsonObject propsObject(const VerilogModule& module)
{
    QJsonArray properties;
    for (const Parameter& p : module.parameters) {
        QString description = p.description.simplified();
        const QString units = p.units.simplified();
        if (!units.isEmpty()) description += (description.isEmpty() ? QString() : QStringLiteral(" ")) + '[' + units + ']';
        if (description.isEmpty()) description = QStringLiteral("-");
        properties.append(QJsonObject{{QStringLiteral("name"), p.name},
                                      {QStringLiteral("value"), p.value},
                                      {QStringLiteral("display"), p.instance ? QStringLiteral("true") : QStringLiteral("false")},
                                      {QStringLiteral("desc"), description}});
    }
    return QJsonObject{{QStringLiteral("description"), QStringLiteral("%1 verilog device").arg(module.name)},
                       {QStringLiteral("property"), properties},
                       {QStringLiteral("tx"), 4},
                       {QStringLiteral("ty"), 4},
                       {QStringLiteral("Model"), module.name},
                       {QStringLiteral("NetName"), QStringLiteral("T")},
                       {QStringLiteral("SymName"), module.name},
                       {QStringLiteral("BitmapFile"), module.name}};
}

QJsonObject merged(const QJsonObject& props, const QJsonObject& symbol)
{
    QJsonObject all = props;
    for (auto it = symbol.constBegin(); it != symbol.constEnd(); ++it) all.insert(it.key(), it.value());
    return all;
}

QJsonObject parseJson(const QByteArray& data, QString* error)
{
    // Older versions wrote "..., ]" and "..., }": a comma before a closing
    // bracket goes - never one inside a string.
    QByteArray clean;
    clean.reserve(data.size());
    bool quoted = false, escaped = false;
    qsizetype comma = -1;
    for (const char c : data) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
            comma = -1;
        } else if (c == ',') {
            comma = clean.size();
        } else if (c == ']' || c == '}') {
            if (comma >= 0) clean[comma] = ' ';
            comma = -1;
        } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            comma = -1;
        }
        clean += c;
    }
    QJsonParseError parsed;
    const QJsonDocument document = QJsonDocument::fromJson(clean, &parsed);
    if (parsed.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = parsed.error != QJsonParseError::NoError
                         ? tr("%1 at character %2").arg(parsed.errorString()).arg(parsed.offset)
                         : tr("it is not a JSON object");
        return QJsonObject();
    }
    return document.object();
}

QString jsonString(const QString& text)
{
    const QByteArray array = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);   // ["..."]
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}

bool setIcon(const QString& file, const QString& icon, QString* error)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadWrite)) {
        if (error) *error = tr("%1 cannot be opened.").arg(QDir::toNativeSeparators(file));
        return false;
    }
    QString why;
    QJsonObject json = parseJson(f.readAll(), &why);
    if (json.isEmpty()) {
        if (error) *error = tr("%1 cannot be read: %2").arg(QDir::toNativeSeparators(file), why);
        return false;
    }
    json.insert(QStringLiteral("BitmapFile"), icon);
    const QByteArray written = QJsonDocument(json).toJson(QJsonDocument::Indented);
    if (!f.resize(0) || f.write(written) != written.size()) {
        if (error) *error = tr("%1 cannot be written.").arg(QDir::toNativeSeparators(file));
        return false;
    }
    return true;
}

} // namespace qucs_s::vamodule
