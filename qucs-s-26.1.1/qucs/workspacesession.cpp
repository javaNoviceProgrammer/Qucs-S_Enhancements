/*
 * workspacesession.cpp - the workspace as it was when Qucs-S closed
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "workspacesession.h"

#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace qucs_s::session {

namespace {

const char *const kSessionKey = "Workspace/Session";
const char *const kWindowStateKey = "Workspace/WindowState";

QString tr(const char *text) { return QCoreApplication::translate("WorkspaceSession", text); }

QJsonArray numbers(const QList<int> &values)
{
    QJsonArray a;
    for (int v : values) a.append(v);
    return a;
}

QList<int> numbersOf(const QJsonValue &v)
{
    QList<int> out;
    for (const QJsonValue &n : v.toArray()) {
        if (!n.isDouble() || n.toInt() < 0) return {};
        out << n.toInt();
    }
    return out;
}

} // namespace

bool Workspace::isEmpty() const
{
    return project.isEmpty() && documentCount() == 0;
}

int Workspace::documentCount() const
{
    int n = 0;
    for (const Pane &p : panes) n += int(p.documents.size());
    return n;
}

QJsonObject Workspace::toJson() const
{
    QJsonArray paneList;
    for (const Pane &p : panes)
        paneList.append(QJsonObject{{QStringLiteral("row"), p.row},
                                    {QStringLiteral("column"), p.column},
                                    {QStringLiteral("documents"), QJsonArray::fromStringList(p.documents)},
                                    {QStringLiteral("current"), p.current}});
    QJsonArray columns;
    for (const QList<int> &row : columnSizes) columns.append(numbers(row));
    return QJsonObject{{QStringLiteral("version"), 1},
                       {QStringLiteral("project"), project},
                       {QStringLiteral("panes"), paneList},
                       {QStringLiteral("active pane"), activePane},
                       {QStringLiteral("row sizes"), numbers(rowSizes)},
                       {QStringLiteral("column sizes"), columns},
                       {QStringLiteral("side tab"), sideTab},
                       {QStringLiteral("saved"), saved.toString(Qt::ISODate)}};
}

Workspace Workspace::fromJson(const QJsonObject &o)
{
    Workspace w;
    if (o.value(QLatin1String("version")).toInt() != 1) return w;
    w.project = o.value(QLatin1String("project")).toString();
    for (const QJsonValue &v : o.value(QLatin1String("panes")).toArray()) {
        const QJsonObject p = v.toObject();
        Pane pane;
        pane.row = p.value(QLatin1String("row")).toInt(-1);
        pane.column = p.value(QLatin1String("column")).toInt(-1);
        // (Two rows of two panes at most, as the window has them.)
        if (pane.row < 0 || pane.row > 1 || pane.column < 0 || pane.column > 1) continue;
        for (const QJsonValue &d : p.value(QLatin1String("documents")).toArray())
            if (d.isString() && !d.toString().isEmpty() && !pane.documents.contains(d.toString())) pane.documents << d.toString();
        pane.current = p.value(QLatin1String("current")).toString();
        w.panes << pane;
    }
    w.activePane = o.value(QLatin1String("active pane")).toInt();
    w.rowSizes = numbersOf(o.value(QLatin1String("row sizes")));
    for (const QJsonValue &row : o.value(QLatin1String("column sizes")).toArray()) w.columnSizes << numbersOf(row);
    w.sideTab = o.value(QLatin1String("side tab")).toInt(-1);
    w.saved = QDateTime::fromString(o.value(QLatin1String("saved")).toString(), Qt::ISODate);
    return w;
}

QString Workspace::summary() const
{
    QStringList parts;
    if (!project.isEmpty()) parts << tr("project %1").arg(QDir(project).dirName());
    const int documents = documentCount();
    if (documents > 0) {
        const QString docs = documents == 1 ? tr("1 document") : tr("%1 documents").arg(documents);
        parts << (panes.size() > 1 ? tr("%1 in %2 panes").arg(docs).arg(panes.size()) : docs);
    }
    if (parts.isEmpty()) parts << tr("no project or document");
    return parts.join(QStringLiteral(", "));
}

void save(const Workspace &workspace, const QByteArray &windowState)
{
    QucsSettingsFile settings;
    settings.setValue(QLatin1String(kSessionKey),
                      QString::fromUtf8(QJsonDocument(workspace.toJson()).toJson(QJsonDocument::Compact)));
    settings.setValue(QLatin1String(kWindowStateKey), windowState);
}

Workspace saved()
{
    QucsSettingsFile settings;
    const QByteArray text = settings.value(QLatin1String(kSessionKey)).toString().toUtf8();
    return Workspace::fromJson(QJsonDocument::fromJson(text).object());
}

QByteArray savedWindowState()
{
    return QucsSettingsFile().value(QLatin1String(kWindowStateKey)).toByteArray();
}

void forget()
{
    QucsSettingsFile settings;
    settings.remove(QLatin1String(kSessionKey));
    settings.remove(QLatin1String(kWindowStateKey));
}

} // namespace qucs_s::session
