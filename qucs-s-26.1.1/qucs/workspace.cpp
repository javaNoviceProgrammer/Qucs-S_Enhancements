/*
 * workspace.cpp - projects coming into the workspace from elsewhere
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "workspace.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#include <filesystem>
#include <system_error>

namespace qucs_s::workspace {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Workspace", text);
}

QString shown(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

// A path for std::filesystem, spelled as the file system has it.
std::filesystem::path fsPath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(QDir::toNativeSeparators(path).toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).toStdString());
#endif
}

// A folder as the file dialog gives it or a user types it - "…/amp_prj/",
// "…//amp_prj", "C:\\x\\amp_prj" - spelled once: "/" separators, none at
// the end, no "." or "..". A trailing slash made the folder's name empty.
QString clean(const QString& path)
{
    return path.isEmpty() ? path : QDir::cleanPath(path);
}

bool isProjectName(const QString& name)
{
    return name.size() > 4 && name.endsWith(QLatin1String("_prj"));
}

// Two paths of the same place, as the platform compares file names.
bool samePath(const QString& a, const QString& b)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

bool isInside(const QString& path, const QString& folder)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return path.startsWith(folder + '/', Qt::CaseInsensitive);
#else
    return path.startsWith(folder + '/');
#endif
}

bool linkLike(const QFileInfo& info)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    return info.isSymbolicLink() || info.isJunction();
#else
    return info.isSymLink();
#endif
}

} // namespace

bool isLink(const QString& path)
{
    // "…/link_prj/" is read through the link, as the folder it leads to.
    return linkLike(QFileInfo(clean(path)));
}

QString linkTarget(const QString& path)
{
    const QFileInfo info(clean(path));
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    if (info.isJunction()) return info.junctionTarget();
#endif
    return linkLike(info) ? info.symLinkTarget() : QString();
}

Result check(const QString& sourceGiven, const QString& workspaceGiven, const QString& name)
{
    const QString source = clean(sourceGiven);
    const QString workspace = clean(workspaceGiven);
    Result r;
    r.status = Result::Invalid;
    const QFileInfo from(source);
    const QString wanted = name.isEmpty() ? from.fileName() : name;
    if (!from.isDir()) {
        r.message = tr("%1 is not a folder.").arg(shown(source));
        return r;
    }
    if (!isProjectName(from.fileName())) {
        r.message = tr("%1 is not a project: the name of a project's folder ends in \"_prj\".").arg(shown(source));
        return r;
    }
    if (!isProjectName(wanted) || wanted.contains('/') || wanted.contains('\\') || wanted.startsWith('.')) {
        r.message = tr("\"%1\" is no name for a project: it ends in \"_prj\" and has no \"/\".").arg(wanted);
        return r;
    }
    const QString space = QFileInfo(workspace).canonicalFilePath();
    if (space.isEmpty()) {
        r.message = tr("The workspace %1 does not exist.").arg(shown(workspace));
        return r;
    }
    const QString real = from.canonicalFilePath();
    // In the workspace already: by its own place, or as the link there.
    if (samePath(QFileInfo(real).absolutePath(), space)
        || samePath(QFileInfo(from.absolutePath()).canonicalFilePath(), space)) {
        r.message = tr("%1 is in the workspace already.").arg(shown(source));
        return r;
    }
    if (samePath(real, space) || isInside(space, real)) {
        r.message = tr("The workspace is inside %1: a project cannot come into itself.").arg(shown(source));
        return r;
    }
    r.path = QDir(workspace).absoluteFilePath(wanted);
    if (QFileInfo::exists(r.path) || isLink(r.path)) {
        r.status = Result::Exists;
        r.message = tr("The workspace has a %1 already.").arg(wanted);
        return r;
    }
    r.status = Result::Done;
    return r;
}

Result importProject(const QString& sourceGiven, const QString& workspaceGiven, const QString& name)
{
    const QString source = clean(sourceGiven);
    const QString workspace = clean(workspaceGiven);
    Result r = check(source, workspace, name);
    if (r.status != Result::Done) return r;
    std::error_code ec;
    std::filesystem::copy(fsPath(QFileInfo(source).canonicalFilePath()), fsPath(r.path),
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks,
                          ec);
    if (ec) {
        // Nothing was there before (check()): what is there is the half copy.
        QDir(r.path).removeRecursively();
        r.status = Result::Failed;
        r.message = tr("Copying %1 into the workspace failed: %2")
                        .arg(shown(source), QString::fromLocal8Bit(ec.message().c_str()));
        return r;
    }
    return r;
}

Result linkProject(const QString& sourceGiven, const QString& workspaceGiven, const QString& name)
{
    const QString source = clean(sourceGiven);
    const QString workspace = clean(workspaceGiven);
    Result r = check(source, workspace, name);
    if (r.status != Result::Done) return r;
    const QString target = QFileInfo(source).canonicalFilePath();
    std::error_code ec;
    std::filesystem::create_directory_symlink(fsPath(target), fsPath(r.path), ec);
    QString why = ec ? QString::fromLocal8Bit(ec.message().c_str()) : QString();
#ifdef Q_OS_WIN
    if (ec) {
        // A directory symbolic link needs Developer Mode or an administrator;
        // a junction needs neither, and does the same for a folder on a
        // local disk.
        QProcess mklink;
        mklink.start(QStringLiteral("cmd.exe"),
                     {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"), shown(r.path), shown(target)});
        if (mklink.waitForFinished(30000) && mklink.exitStatus() == QProcess::NormalExit && mklink.exitCode() == 0)
            why.clear();
        else
            why += QStringLiteral("; mklink /J: ") + QString::fromLocal8Bit(mklink.readAllStandardError()).trimmed();
    }
#endif
    if (!why.isEmpty() || !isLink(r.path)) {
        r.status = Result::Failed;
        r.message = tr("Linking %1 into the workspace failed: %2").arg(shown(source), why);
    }
    return r;
}

QString freeName(const QString& workspace, const QString& name)
{
    QString base = name;
    if (base.endsWith(QLatin1String("_prj"))) base.chop(4);
    const QDir dir(workspace);
    for (int n = 2; n < 100000; ++n) {
        const QString candidate = QStringLiteral("%1_%2_prj").arg(base).arg(n);
        const QString path = dir.absoluteFilePath(candidate);
        if (!QFileInfo::exists(path) && !isLink(path)) return candidate;
    }
    return QString();
}

bool removeLink(const QString& pathGiven, QString* error)
{
    const QString path = clean(pathGiven);   // unlink("…/link_prj/") fails
    if (!isLink(path)) {
        if (error) *error = tr("%1 is not a link.").arg(shown(path));
        return false;
    }
#ifdef Q_OS_WIN
    // A directory link or a junction goes as a directory would
    // (RemoveDirectory), which never follows it.
    const bool removed = QDir().rmdir(path) || QFile::remove(path);
#else
    const bool removed = QFile::remove(path);   // unlink(): the link itself
#endif
    if (!removed && error) *error = tr("%1 could not be removed.").arg(shown(path));
    return removed;
}

} // namespace qucs_s::workspace
