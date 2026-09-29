/*
 * mcpserver.cpp - the host's tools as an MCP server
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "mcpserver.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace qucs_s::mcp {

namespace {

// (The request's id as it came - null too, which was answered as 0.)
QJsonObject response(const QJsonValue& id, const QJsonObject& result)
{
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id.isUndefined() ? QJsonValue(QJsonValue::Null) : id},
            {QStringLiteral("result"), result}};
}

QJsonObject failure(const QJsonValue& id, int code, const QString& text)
{
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id.isUndefined() ? QJsonValue(QJsonValue::Null) : id},
            {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), text}}}};
}

QString idKey(const QJsonValue& id)
{
    return id.isString() ? id.toString() : QString::number(id.toDouble(), 'g', 17);
}

// ---- max_chars: an answer no longer than the caller wants.

QByteArray compact(const QJsonValue& v)
{
    if (v.isObject()) return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    if (v.isArray()) return QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    return QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
}

// A list already cut ends in "… n more"; a text in "… (n more characters)".
const QRegularExpression& moreItems()
{
    static const QRegularExpression re(QStringLiteral("^… (\\d+) more$"));
    return re;
}
const QRegularExpression& moreText()
{
    static const QRegularExpression re(QStringLiteral("^(.*)… \\((\\d+) more characters\\)$"), QRegularExpression::DotMatchesEverythingOption);
    return re;
}
int moreOf(const QJsonArray& a)
{
    const QRegularExpressionMatch m = a.isEmpty() || !a.last().isString() ? QRegularExpressionMatch() : moreItems().match(a.last().toString());
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

// Where in an answer the most is to cut: a list of more than one item, or
// a text of more than 80 characters, the biggest - its path of keys and
// indices.
struct Spot {
    QList<QJsonValue> path;
    qsizetype size = 0;
};
void biggest(const QJsonValue& v, QList<QJsonValue>& path, Spot& best)
{
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        const qsizetype items = a.size() - (moreOf(a) >= 0 ? 1 : 0);
        if (items > 1) {
            if (const qsizetype size = compact(v).size(); size > best.size) best = Spot{path, size};
        }
        for (int i = 0; i < a.size(); ++i) {
            path.append(i);
            biggest(a.at(i), path, best);
            path.removeLast();
        }
    } else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            path.append(it.key());
            biggest(it.value(), path, best);
            path.removeLast();
        }
    } else if (v.isString() && v.toString().size() > 80 && v.toString().size() > best.size) {
        best = Spot{path, v.toString().size()};
    }
}

// \a v with the list or text at \a path halved.
QJsonValue halvedAt(const QJsonValue& v, const QList<QJsonValue>& path, int depth = 0)
{
    if (depth == path.size()) {
        if (v.isArray()) {
            QJsonArray a = v.toArray();
            int more = moreOf(a);
            if (more >= 0) a.removeLast();
            else more = 0;
            const qsizetype keep = std::max<qsizetype>(1, a.size() / 2);
            more += int(a.size() - keep);
            while (a.size() > keep) a.removeLast();
            a.append(QStringLiteral("… %1 more").arg(more));
            return a;
        }
        QString s = v.toString();
        int more = 0;
        if (const QRegularExpressionMatch m = moreText().match(s); m.hasMatch()) {
            s = m.captured(1);
            more = m.captured(2).toInt();
        }
        const qsizetype keep = s.size() / 2;
        return s.left(keep) + QStringLiteral("… (%1 more characters)").arg(more + s.size() - keep);
    }
    const QJsonValue key = path.at(depth);
    if (v.isObject()) {
        QJsonObject o = v.toObject();
        o.insert(key.toString(), halvedAt(o.value(key.toString()), path, depth + 1));
        return o;
    }
    QJsonArray a = v.toArray();
    a.replace(key.toInt(), halvedAt(a.at(key.toInt()), path, depth + 1));
    return a;
}

// The answer cut to \a most characters: its biggest lists (to their first
// items and "… n more") and texts (to their start and "… (n more
// characters)") halved, the biggest first, until it fits; its 'trimmed'
// says what was cut. A text answer is cut at its end.
QJsonObject trimmedTo(const QJsonObject& result, int most)
{
    QJsonArray content = result.value(QLatin1String("content")).toArray();
    if (content.isEmpty() || content.first().toObject().value(QLatin1String("type")).toString() != QLatin1String("text")) return result;
    QJsonObject first = content.first().toObject();
    const QString text = first.value(QLatin1String("text")).toString();
    if (text.size() <= most) return result;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    QString out;
    if (doc.isObject() || doc.isArray()) {
        QJsonValue value = doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array());
        QStringList cut;
        const int room = most - (doc.isObject() ? 260 : 0);   // (for 'trimmed' itself)
        for (int round = 0; round < 400 && compact(value).size() > room; ++round) {
            QList<QJsonValue> path;
            Spot best;
            biggest(value, path, best);
            if (best.size == 0) break;
            value = halvedAt(value, best.path);
            QStringList where;
            for (const QJsonValue& k : std::as_const(best.path)) where << (k.isString() ? k.toString() : QStringLiteral("[]"));
            const QString at = where.isEmpty() ? QStringLiteral("the list") : where.join(QLatin1Char('.')).replace(QStringLiteral(".[]"), QStringLiteral("[]"));
            if (!cut.contains(at)) cut << at;
        }
        if (value.isObject()) {
            QJsonObject o = value.toObject();
            o.insert(QStringLiteral("trimmed"), QStringLiteral("to %1 characters or less of %2, as max_chars asks: %3 cut - the tool's own "
                                                               "filters ask for less, a larger max_chars for more")
                                                    .arg(most)
                                                    .arg(text.size())
                                                    .arg(cut.mid(0, 6).join(QStringLiteral(", "))));
            value = o;
        }
        out = QString::fromUtf8(compact(value));
    }
    // A text, or JSON that would not fit even so: cut at its end.
    if (out.isEmpty() || out.size() > most) {
        const QString base = out.isEmpty() ? text : out;
        const qsizetype keep = std::max(0, most - 70);
        out = base.left(keep) + QStringLiteral("\n… (%1 more characters: a larger max_chars gives them)").arg(base.size() - keep);
    }
    first.insert(QStringLiteral("text"), out);
    content.replace(0, first);
    QJsonObject r = result;
    r.insert(QStringLiteral("content"), content);
    return r;
}

} // namespace

QJsonObject structuredOf(const QJsonObject& result)
{
    QStringList texts;
    for (const QJsonValue& v : result.value(QLatin1String("content")).toArray())
        if (v.toObject().value(QLatin1String("type")).toString() == QLatin1String("text"))
            texts << v.toObject().value(QLatin1String("text")).toString();
    QJsonObject structured;
    if (result.value(QLatin1String("isError")).toBool()) {
        // (An error told as JSON - a script's, with its line - as its fields.)
        const QJsonDocument doc = QJsonDocument::fromJson(texts.value(0).toUtf8());
        if (doc.isObject() && doc.object().contains(QLatin1String("error"))) structured = doc.object();
        else structured.insert(QStringLiteral("error"), texts.isEmpty() ? QString() : texts.first());
    } else if (!texts.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(texts.first().toUtf8());
        if (doc.isObject()) structured = doc.object();
        else if (doc.isArray()) structured.insert(QStringLiteral("items"), doc.array());
        else structured.insert(QStringLiteral("summary"), texts.first());
    }
    // What follows the first text: what changed since the conversation's
    // last call, and notes (a document switched from its symbol, ...).
    QJsonArray notes;
    for (int i = 1; i < texts.size(); ++i) {
        static const QString since = QStringLiteral("Since your last call: ");
        if (texts.at(i).startsWith(since)) structured.insert(QStringLiteral("since_last_call"), texts.at(i).mid(since.size()));
        else notes.append(texts.at(i));
    }
    if (!notes.isEmpty()) structured.insert(QStringLiteral("notes"), notes);
    return structured;
}

Server::Server(claude::ToolHost* host, QObject* parent) : QObject(parent), a_host(host)
{
    a_poll = new QTimer(this);
    a_poll->setInterval(1000);
    connect(a_poll, &QTimer::timeout, this, &Server::poll);
}

Server::~Server()
{
    // (The host may be gone already - the window closing: its asker for
    // this conversation finds the server gone and cancels, so it is left.)
    // The questions still open: unanswered.
    const auto pending = std::exchange(a_pending, {});
    for (const auto& done : pending) done(QJsonObject{{QStringLiteral("action"), QStringLiteral("cancel")}});
}

void Server::setCaller(quint64 caller)
{
    a_caller = caller;
    if (a_host == nullptr || caller == 0) return;
    QPointer<Server> self(this);
    a_host->setAsker(caller, [self](const QString& message, const QJsonObject& schema, std::function<void(const QJsonObject&)> done) {
        if (!self) {
            done(QJsonObject{{QStringLiteral("action"), QStringLiteral("cancel")}});
            return;
        }
        self->elicit(message, schema, std::move(done));
    });
}

bool Server::isNotification(const QJsonObject& message)
{
    return message.contains(QLatin1String("method")) && !message.contains(QLatin1String("id"));
}

void Server::send(const QJsonObject& message) const
{
    if (a_send) a_send(message);
}

QJsonObject Server::callResult(const QJsonObject& result) const
{
    QJsonObject r = result;
    if (!r.contains(QLatin1String("structuredContent"))) r.insert(QStringLiteral("structuredContent"), structuredOf(result));
    return r;
}

void Server::handle(const QJsonObject& message, const Reply& reply)
{
    const QJsonValue id = message.value(QLatin1String("id"));
    const QString method = message.value(QLatin1String("method")).toString();
    // An answer to one of the server's own requests (a question asked).
    if (method.isEmpty() && message.contains(QLatin1String("id"))) {
        if (auto done = a_pending.take(idKey(id))) {
            if (message.contains(QLatin1String("error"))) done(QJsonObject{{QStringLiteral("action"), QStringLiteral("cancel")}});
            else done(message.value(QLatin1String("result")).toObject());
        }
        reply(response(QJsonValue(0), QJsonObject()));
        return;
    }
    if (method == QLatin1String("initialize")) {
        const QJsonObject params = message.value(QLatin1String("params")).toObject();
        a_canElicit = params.value(QLatin1String("capabilities")).toObject().contains(QLatin1String("elicitation"));
        a_initialized = true;
        QJsonObject result{
            {QStringLiteral("protocolVersion"), params.value(QLatin1String("protocolVersion")).toString(QStringLiteral("2025-06-18"))},
            {QStringLiteral("capabilities"),
             QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}},
                         {QStringLiteral("resources"), QJsonObject{{QStringLiteral("subscribe"), true}, {QStringLiteral("listChanged"), true}}}}},
            {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), a_host->serverName()}, {QStringLiteral("version"), QStringLiteral("1")}}}};
        const QString instructions = a_host->instructions();
        if (!instructions.isEmpty()) result.insert(QStringLiteral("instructions"), instructions);
        // What there is now: a change of it is told from here on.
        a_listVersion.clear();
        for (const QJsonValue& r : a_host->resources()) a_listVersion += r.toObject().value(QLatin1String("uri")).toString() + QLatin1Char('\n');
        if (a_send) a_poll->start();
        reply(response(id, result));
        return;
    }
    // (A request's id is a string or a number; its params, when given, an
    // object: [1, 2] was read as no tool at all.)
    if (message.contains(QLatin1String("id")) && !id.isString() && !id.isDouble() && !id.isNull()) {
        reply(failure(QJsonValue(QJsonValue::Null), -32600, QStringLiteral("Invalid Request: 'id' is a string or a number")));
        return;
    }
    if (isNotification(message) || method == QLatin1String("ping")) {
        reply(response(id, QJsonObject()));
        return;
    }
    if (message.contains(QLatin1String("params")) && !message.value(QLatin1String("params")).isObject()) {
        reply(failure(id, -32602, QStringLiteral("Invalid params: 'params' is an object")));
        return;
    }
    const QJsonObject params = message.value(QLatin1String("params")).toObject();
    if (method == QLatin1String("tools/list")) {
        reply(response(id, {{QStringLiteral("tools"), a_host->tools()}}));
    } else if (method == QLatin1String("tools/call")) {
        if (!params.value(QLatin1String("name")).isString()
            || (params.contains(QLatin1String("arguments")) && !params.value(QLatin1String("arguments")).isObject())) {
            reply(failure(id, -32602, QStringLiteral("Invalid params: 'name' is the tool's name, 'arguments' an object")));
            return;
        }
        const QString tool = params.value(QLatin1String("name")).toString();
        QJsonObject arguments = params.value(QLatin1String("arguments")).toObject();
        // max_chars: the longest answer the caller wants, for every tool -
        // the server's, not the tool's: taken out before the tool sees it.
        int most = 0;
        if (arguments.contains(QLatin1String("max_chars"))) {
            const QJsonValue v = arguments.take(QLatin1String("max_chars"));
            if (!v.isDouble() || v.toDouble() < 200 || v.toDouble() != std::floor(v.toDouble())) {
                reply(response(id, QJsonObject{{QStringLiteral("content"),
                                                QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                       {QStringLiteral("text"), QStringLiteral("max_chars is a whole number of "
                                                                                                               "characters, 200 or more. Nothing was done.")}}}},
                                               {QStringLiteral("isError"), true}}));
                return;
            }
            most = int(std::min(v.toDouble(), 1e9));
        }
        QPointer<Server> self(this);
        auto done = [self, id, reply, most](const QJsonObject& result) {
            if (self) reply(response(id, self->callResult(most > 0 ? trimmedTo(result, most) : result)));
        };
        if (a_call) a_call(tool, arguments, done);
        else a_host->callTool(tool, arguments, done);
    } else if (method == QLatin1String("resources/list")) {
        reply(response(id, {{QStringLiteral("resources"), a_host->resources()}}));
    } else if (method == QLatin1String("resources/templates/list")) {
        reply(response(id, {{QStringLiteral("resourceTemplates"), a_host->resourceTemplates()}}));
    } else if (method == QLatin1String("resources/read")) {
        const QString uri = params.value(QLatin1String("uri")).toString();
        QString error;
        const QJsonArray contents = a_host->readResource(uri, &error);
        if (contents.isEmpty()) reply(failure(id, -32002, error.isEmpty() ? QStringLiteral("Resource not found: %1").arg(uri) : error));
        else reply(response(id, {{QStringLiteral("contents"), contents}}));
    } else if (method == QLatin1String("resources/subscribe")) {
        const QString uri = params.value(QLatin1String("uri")).toString();
        a_subscribed.insert(uri);
        a_versions.insert(uri, a_host->resourceVersion(uri));
        if (a_send) a_poll->start();
        reply(response(id, QJsonObject()));
    } else if (method == QLatin1String("resources/unsubscribe")) {
        const QString uri = params.value(QLatin1String("uri")).toString();
        a_subscribed.remove(uri);
        a_versions.remove(uri);
        reply(response(id, QJsonObject()));
    } else {
        reply(failure(id, -32601, QStringLiteral("Method not found: %1").arg(method)));
    }
}

void Server::poll()
{
    if (!a_send) return;
    for (const QString& uri : std::as_const(a_subscribed)) {
        const QString version = a_host->resourceVersion(uri);
        if (version == a_versions.value(uri)) continue;
        a_versions.insert(uri, version);
        send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
              {QStringLiteral("method"), QStringLiteral("notifications/resources/updated")},
              {QStringLiteral("params"), QJsonObject{{QStringLiteral("uri"), uri}}}});
    }
    if (!a_initialized) return;
    QString list;
    for (const QJsonValue& r : a_host->resources()) list += r.toObject().value(QLatin1String("uri")).toString() + QLatin1Char('\n');
    if (list != a_listVersion) {
        a_listVersion = list;
        send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), QStringLiteral("notifications/resources/list_changed")}});
    }
}

void Server::elicit(const QString& message, const QJsonObject& schema, std::function<void(const QJsonObject&)> done)
{
    if (!a_canElicit || !a_send) {
        done(QJsonObject{{QStringLiteral("action"), QStringLiteral("cancel")}});
        return;
    }
    const QString id = QStringLiteral("qucs-ask-%1").arg(a_nextRequest++);
    a_pending.insert(id, std::move(done));
    // Unanswered for ten minutes: taken as cancelled.
    QPointer<Server> self(this);
    QTimer::singleShot(10 * 60 * 1000, this, [self, id] {
        if (!self) return;
        if (auto d = self->a_pending.take(id)) d(QJsonObject{{QStringLiteral("action"), QStringLiteral("cancel")}});
    });
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
          {QStringLiteral("id"), id},
          {QStringLiteral("method"), QStringLiteral("elicitation/create")},
          {QStringLiteral("params"), QJsonObject{{QStringLiteral("message"), message}, {QStringLiteral("requestedSchema"), schema}}}});
}

} // namespace qucs_s::mcp
