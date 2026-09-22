/*
 * systemopen.h - documents the system asks the application to open
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_SYSTEMOPEN_H
#define QUCS_SYSTEMOPEN_H

#include <QObject>
#include <QStringList>

#include <functional>

namespace qucs_s::systemopen {

/// A file or directory as the system names it: a path, relative to the
/// current directory or not, or a file: URL (a desktop's file manager may
/// give one). Empty for a URL of any other scheme.
QString localPath(const QString& item);

/// Collects what the system asks the application to open while it runs -
/// on macOS a QFileOpenEvent for every document double-clicked in the
/// Finder, dropped on the Dock icon or opened with "Open With" - from the
/// moment the application object exists, and hands it over in batches
/// once there is somewhere to open it and no modal dialog is up.
class Receiver : public QObject
{
    Q_OBJECT

public:
    explicit Receiver(QObject* parent = nullptr);   ///< filters qApp's events
    ~Receiver() override;

    /// Where the files go; what arrived before is handed over now.
    void setTarget(std::function<void(const QStringList&)> open);
    /// What has arrived and not been handed over yet.
    QStringList pending() const { return m_pending; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void handOverSoon(int delay = 0);

    QStringList m_pending;
    std::function<void(const QStringList&)> m_open;
    bool m_scheduled = false;
};

} // namespace qucs_s::systemopen

#endif
