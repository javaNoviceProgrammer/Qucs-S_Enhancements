/*
 * systemopen.cpp - documents the system asks the application to open
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "systemopen.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QTimer>
#include <QUrl>

#include <utility>

namespace qucs_s::systemopen {

QString localPath(const QString& item)
{
    QString path = item;
    // Only "file:" is taken for a URL: "C:/x.sch" parses as one of the
    // scheme "c".
    if (item.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        const QUrl url(item);
        if (!url.isLocalFile()) return QString();
        path = url.toLocalFile();
    } else if (item.contains(QLatin1String("://"))) {
        return QString();
    }
    if (path.isEmpty()) return QString();
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

Receiver::Receiver(QObject* parent) : QObject(parent)
{
    if (qApp) qApp->installEventFilter(this);
}

Receiver::~Receiver()
{
    if (qApp) qApp->removeEventFilter(this);
}

void Receiver::setTarget(std::function<void(const QStringList&)> open)
{
    m_open = std::move(open);
    if (!m_pending.isEmpty()) handOverSoon();
}

bool Receiver::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::FileOpen) return QObject::eventFilter(watched, event);
    const auto* request = static_cast<QFileOpenEvent*>(event);
    const QString item = request->file().isEmpty() ? request->url().toString() : request->file();
    if (!item.isEmpty()) {
        m_pending << item;
        handOverSoon();
    }
    return true;
}

// Several files open as one batch (the Finder sends an event for each), and
// never under a modal dialog - a question at start, the crash recovery -
// which a document opening behind it would leave in a muddle.
void Receiver::handOverSoon(int delay)
{
    if (!m_open || m_scheduled) return;
    m_scheduled = true;
    QTimer::singleShot(delay, this, [this] {
        m_scheduled = false;
        if (QApplication::activeModalWidget() != nullptr) {
            handOverSoon(200);
            return;
        }
        const QStringList files = std::exchange(m_pending, {});
        if (!files.isEmpty() && m_open) m_open(files);
    });
}

} // namespace qucs_s::systemopen
