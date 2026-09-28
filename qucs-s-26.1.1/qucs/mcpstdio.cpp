/*
 * mcpstdio.cpp - Qucs-S's tools as an MCP server on stdin and stdout,
 *                with no window on screen (qucs-s --mcp-server)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "mcpstdio.h"

#include "main.h"
#include "mcpserver.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "qucsdoc.h"
#include "schematic.h"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QTimer>

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
#include <io.h>
#define QUCS_DUP _dup
#define QUCS_DUP2 _dup2
#define QUCS_FILENO _fileno
#else
#include <unistd.h>
#define QUCS_DUP dup
#define QUCS_DUP2 dup2
#define QUCS_FILENO fileno
#endif

namespace qucs_s::mcp {

bool askedFor(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--mcp-server") == 0 || qstrcmp(argv[i], "-mcp-server") == 0) return true;
    return false;
}

void prepareHeadless()
{
    // No window on screen, and no claude or gh run by the dock of this
    // instance (the client is what talks to Claude).
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QUCS_CLAUDE")) qputenv("QUCS_CLAUDE", "/nonexistent/claude");
    if (qEnvironmentVariableIsEmpty("QUCS_GH")) qputenv("QUCS_GH", "/nonexistent/gh");
}

QTimer* closeDialogsWhileStarting()
{
    // What would ask at the start (no ngspice found, a first run) waits
    // for no one: closed, and said on stderr.
    auto* timer = new QTimer;
    timer->setInterval(100);
    QObject::connect(timer, &QTimer::timeout, [] {
        if (QWidget* dialog = QApplication::activeModalWidget()) {
            fprintf(stderr, "qucs-s --mcp-server: closed \"%s\" at the start\n", qPrintable(dialog->windowTitle()));
            dialog->close();
        }
    });
    timer->start();
    return timer;
}

int runStdio(QApplication& app, QucsApp* window, QTimer* startupCloser)
{
    auto* control = window->findChild<QucsControl*>();
    if (control == nullptr) {
        fprintf(stderr, "qucs-s --mcp-server: Claude's tools are not in this build\n");
        return 1;
    }
    // Messages go out on a copy of stdout; stdout itself is stderr from
    // here on, so that nothing else can write into the stream.
    fflush(stdout);
    const int fd = QUCS_DUP(QUCS_FILENO(stdout));
    QUCS_DUP2(QUCS_FILENO(stderr), QUCS_FILENO(stdout));
    auto* out = new QFile(&app);
    if (fd < 0 || !out->open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        fprintf(stderr, "qucs-s --mcp-server: stdout cannot be written\n");
        return 1;
    }
    auto* server = new Server(control, &app);
    // (One conversation: what changed since its last call is told, as the
    // dock's are told.)
    constexpr quint64 caller = 1;
    server->setCaller(caller);
    server->setCall([control](const QString& tool, const QJsonObject& arguments, std::function<void(const QJsonObject&)> done) {
        QTimer::singleShot(0, control, [control, tool, arguments, done] { control->callToolFor(caller, tool, arguments, done); });
    });
    const auto write = [out](const QJsonObject& message) {
        out->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        out->flush();
    };
    server->setSend(write);
    QPointer<QTimer> closer(startupCloser);
    // The requests being answered, and whether stdin has ended: the server
    // ends when both are over, not before the last answer is written.
    struct Pending {
        int open = 0;
        bool ended = false;
    };
    auto* pending = new Pending;
    const auto finish = [pending] {
        if (!pending->ended || pending->open > 0) return;
        // Unsaved changes are the client's to save: left, and said.
        int unsaved = 0;
        if (QucsMain != nullptr)
            for (QucsDoc* d : QucsMain->allDocuments())
                if (d->getDocChanged()) {
                    ++unsaved;
                    if (auto* sch = dynamic_cast<Schematic*>(d)) sch->setChanged(false);
                    d->setDocChanged(false);
                }
        if (unsaved > 0) fprintf(stderr, "qucs-s --mcp-server: %d document(s) with unsaved changes left as they were saved\n", unsaved);
        QCoreApplication::quit();
    };
    const auto take = [server, write, closer, pending, finish](const QByteArray& line) {
        if (line.trimmed().isEmpty()) return;
        if (closer) closer->deleteLater();   // (the client is here: dialogs from now on are the tools')
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
        if (!doc.isObject()) {
            write({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                   {QStringLiteral("id"), QJsonValue()},
                   {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32700},
                                                         {QStringLiteral("message"), QStringLiteral("Parse error: %1").arg(error.errorString())}}}});
            return;
        }
        const QJsonObject message = doc.object();
        // A request is answered; a notification or an answer to the
        // server's own request is not.
        const bool answered = message.contains(QLatin1String("method")) && message.contains(QLatin1String("id"));
        if (answered) ++pending->open;
        server->handle(message, [write, answered, pending, finish](const QJsonObject& response) {
            if (!answered) return;
            write(response);
            --pending->open;
            finish();
        });
    };
    // Lines in on a thread of their own (stdin signals on no platform's
    // event loop alike), each handled on the main thread; at its end, the
    // server ends.
    std::thread reader([server, take, pending, finish] {
        std::string line;
        while (std::getline(std::cin, line)) {
            const QByteArray bytes = QByteArray::fromStdString(line);
            QMetaObject::invokeMethod(server, [take, bytes] { take(bytes); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(server, [pending, finish] {
            pending->ended = true;
            finish();
        }, Qt::QueuedConnection);
    });
    reader.detach();
    return app.exec();
}

} // namespace qucs_s::mcp
