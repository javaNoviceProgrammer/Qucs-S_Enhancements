/*
 * mcpserver.h - the host's tools as an MCP server: JSON-RPC messages in,
 *               answers and the server's own messages out, whatever
 *               carries them (the Claude Code dock's stream, stdio)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MCPSERVER_H
#define QUCS_MCPSERVER_H

#include "claudecode.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

class QTimer;

namespace qucs_s::mcp {

/// What an MCP server's tools/call result says, structured
/// (structuredContent): the object its text is, or {summary} of a text
/// that is not one, {error} of an error - and what changed since the
/// conversation's last call, when it was told.
QJsonObject structuredOf(const QJsonObject& result);

/// max_chars as a call gives it: a whole number of characters, 200 or
/// more (\a most then holds it); false for anything else.
bool readMaxChars(const QJsonValue& value, int* most);

/// A tools/call result cut to \a most characters of text: JSON as JSON, its
/// biggest lists, maps and texts halved until it fits, then its items and
/// fields, with 'trimmed' saying what was cut; a text cut at its end.
/// Several texts (a batch's answers): the longest cut first, together to
/// \a most. Images are left as they are.
QJsonObject trimmedTo(const QJsonObject& result, int most);

/*!
 * An MCP server over a ToolHost (the Model Context Protocol, 2025-06-18):
 * initialize, ping, tools/list, tools/call, resources/list, resources/
 * templates/list, resources/read, resources/subscribe and unsubscribe.
 * Whoever carries the messages hands each one to handle() and sends what
 * it answers; the server's own messages - notifications/resources/updated
 * when a subscribed resource changes, notifications/resources/list_changed
 * when what there is changes, elicitation/create to ask the user - go out
 * through setSend()'s function, and the answers to its requests come back
 * through handle() like any message.
 */
class Server : public QObject
{
    Q_OBJECT

public:
    /// A JSON-RPC response to send (id, and result or error).
    using Reply = std::function<void(const QJsonObject& response)>;
    /// How tools/call reaches the host: the conversation's own way (its
    /// number, the document it is pinned to). The host's callTool() when
    /// not set.
    using Call = std::function<void(const QString& tool, const QJsonObject& arguments,
                                    std::function<void(const QJsonObject& result)> done)>;

    explicit Server(claude::ToolHost* host, QObject* parent = nullptr);
    ~Server() override;

    void setCall(Call call) { a_call = std::move(call); }
    /// Where the server's own messages go: without it, it sends none.
    void setSend(std::function<void(const QJsonObject& message)> send) { a_send = std::move(send); }
    /// The conversation it serves, for the host (ToolHost::setAsker()).
    void setCaller(quint64 caller);

    /// Takes one message. A request is answered through \a reply, now or
    /// later; a notification or a response to one of the server's own
    /// requests is answered at once with an empty result (a stream that
    /// needs an answer to everything takes it; stdio drops it).
    void handle(const QJsonObject& message, const Reply& reply);
    /// Whether \a message is a JSON-RPC notification (nothing to answer).
    static bool isNotification(const QJsonObject& message);

    /// Whether the client said it can ask its user (elicitation).
    bool canElicit() const { return a_canElicit; }
    /// Asks the user through the client (elicitation/create): \a done gets
    /// its result - {action, content} - or {action: "cancel"} when the
    /// client cannot or the question goes unanswered.
    void elicit(const QString& message, const QJsonObject& schema, std::function<void(const QJsonObject& result)> done);

    QSet<QString> subscriptions() const { return a_subscribed; }
    /// Looks at the subscribed resources (and at what there is) now, as
    /// the timer does every second: those that changed are told of.
    void poll();

private:
    QJsonObject callResult(const QJsonObject& result) const;
    void send(const QJsonObject& message) const;

    claude::ToolHost* a_host;
    Call a_call;
    std::function<void(const QJsonObject&)> a_send;
    quint64 a_caller = 0;
    bool a_canElicit = false;
    bool a_initialized = false;
    QSet<QString> a_subscribed;
    QHash<QString, QString> a_versions;   // a subscribed resource: its version when last told
    QString a_listVersion;                // what there is: the resources' URIs, when last told
    QTimer* a_poll = nullptr;
    int a_nextRequest = 1;
    QHash<QString, std::function<void(const QJsonObject&)>> a_pending;   // the server's requests: by id
};

} // namespace qucs_s::mcp

#endif // QUCS_MCPSERVER_H
