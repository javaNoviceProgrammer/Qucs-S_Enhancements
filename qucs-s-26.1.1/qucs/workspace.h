/*
 * workspace.h - projects coming into the workspace from elsewhere: copied
 * in (import) or linked in (a symbolic link, on Windows a junction when
 * links need privileges), and links taken out again without touching
 * what they point to
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_WORKSPACE_H
#define QUCS_WORKSPACE_H

#include <QString>

namespace qucs_s::workspace {

/// What bringing a project into the workspace came to.
struct Result {
    enum Status {
        Done,      ///< path is the project in the workspace
        Exists,    ///< the workspace has something of that name: path
        Invalid,   ///< not a project, or not one that can come in (message)
        Failed     ///< the file system said no (message)
    };
    Status status = Failed;
    QString path;
    QString message;
};

/// Whether \a path is a link - a symbolic link, or a Windows junction -
/// rather than the directory itself.
bool isLink(const QString& path);
/// Where the link \a path points; empty for anything else.
QString linkTarget(const QString& path);

/// Checks that the project directory \a source can come into \a workspace
/// as \a name (its own name if empty): a directory whose name, and \a name,
/// end in "_prj", that is not in the workspace already, and that does not
/// hold the workspace; nothing of that name in the workspace yet.
Result check(const QString& source, const QString& workspace, const QString& name = QString());

/// Copies the project \a source - everything in it, links inside it as
/// links - into \a workspace as \a name. A copy that fails half-way is
/// removed.
Result importProject(const QString& source, const QString& workspace, const QString& name = QString());

/// Links the project \a source into \a workspace as \a name: nothing is
/// copied, the workspace has a link to it (on Windows a directory
/// symbolic link, or a junction without the privilege for one).
Result linkProject(const QString& source, const QString& workspace, const QString& name = QString());

/// The first name in \a workspace free for a project like \a name:
/// "amp_2_prj", "amp_3_prj", ...
QString freeName(const QString& workspace, const QString& name);

/// Removes the link \a path and nothing it points to; false (and why, in
/// \a error) for anything that is not a link.
bool removeLink(const QString& path, QString* error = nullptr);

} // namespace qucs_s::workspace

#endif
