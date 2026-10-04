/*
 * projectlibraries.h - the Verilog-A of the library devices a project's
 * schematics use, in the project: a folder for each library, named after
 * it, with a link to each of those sources (a copy where links are not
 * made: Windows) - shown in the Content panel, opened read-only, compiled
 * beside the link - kept by Qucs-S and taken away when no schematic of the
 * project uses the device any more
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PROJECTLIBRARIES_H
#define QUCS_PROJECTLIBRARIES_H

#include <QList>
#include <QString>
#include <QStringList>

class Schematic;

namespace qucs_s::projectlibraries {

/// The record of what Qucs-S put into a library's folder in a project,
/// a file in that folder.
inline constexpr char RecordName[] = ".qucs-library.json";

/// How a library's source is put into a project: a symbolic link to it,
/// or - where links cannot be made by anyone (Windows) - a copy of it and
/// of the files it includes, renewed when the original changes.
enum class Mode { Link, Copy };
Mode defaultMode();

/// A Verilog-A source of a library device a schematic uses.
struct Use {
    QString library;   ///< the library's name: its .lib file's base name, its folder's name
    QString folder;    ///< the library's folder (where its .lib is, the name appended), real path
    QString source;    ///< the source, real path
};

/// What the project's schematics use: those saved in it (not in its
/// Scratch folder) and \a open, schematics in memory (unsaved changes) -
/// with the subcircuits outside the project they place. \a uses: each
/// library device's Verilog-A sources (a library inside the project is
/// not one: its files are the project's already); \a unresolved: the
/// names of the libraries parts name that are not found here.
void usedSources(const QString& projectDir, const QList<Schematic*>& open, QList<Use>* uses,
                 QStringList* unresolved);

/// What sync() did, as paths relative to the project.
struct Report {
    QStringList made;       ///< linked (or copied) in
    QStringList removed;    ///< taken away: no schematic uses it any more
    QStringList conflicts;  ///< a file of the user's in the way: left as it is
    bool changed() const { return !made.isEmpty() || !removed.isEmpty(); }
};

/// Brings the project's library folders up to date with usedSources():
/// for each library a schematic uses a device of, <project>/<library>/
/// (another name when that one is taken by another library: _2, _3...)
/// with a link to - or a copy of - each source at its path in the
/// library's folder, a link that leads elsewhere (the library moved, or
/// another computer) made again; what no schematic uses any more is taken
/// away: the link or copy, the model compiled beside it (NAME.osdi), the
/// record, and the folder when Qucs-S made it and nothing else is in it.
/// Only what Qucs-S put there, as the folder's record says: a file of the
/// user's is never written over or taken away. A library a part names
/// that is not found here keeps what it has.
Report sync(const QString& projectDir, const QList<Schematic*>& open = {}, Mode mode = defaultMode());

/// The library's source \a path is a link to or a copy of (sync() put it
/// there): the library's name and the original file. Empty when it is none.
struct Entry {
    QString library;
    QString original;
    bool isEmpty() const { return library.isEmpty(); }
};
Entry entryOf(const QString& path);

} // namespace qucs_s::projectlibraries

#endif
