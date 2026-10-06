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
#include <QSet>
#include <QString>
#include <QStringList>

class Schematic;

namespace qucs_s::projectlibraries {

/// Where in the project: Libraries/<library>/ - apart from the project's
/// own libraries (NAME.lib and its folder NAME/).
inline constexpr char FolderName[] = "Libraries";
/// The mark of a Libraries/ folder Qucs-S made: taken away when it holds
/// nothing else (one of the user's is never).
inline constexpr char MarkerName[] = ".qucs-libraries";
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
/// not one: its files are the project's already) - and those of the parts
/// of its libraries marked to be loaded in all its circuits
/// (LibComp::alwaysLoaded()), placed or not; \a unresolved: the names of
/// the libraries parts name that are not found here.
void usedSources(const QString& projectDir, const QList<Schematic*>& open, QList<Use>* uses,
                 QStringList* unresolved);

/// The modules, in lower case, of the Verilog-A of the library parts marked
/// to be loaded in every circuit of the project, placed or not
/// (LibComp::alwaysLoaded(): their subcircuit's Document Settings >
/// Library): of the project's own libraries (a NAME.lib in it), and of the
/// libraries whose parts its schematics place - usedSources() counts the
/// marked parts of those as used, so sync() links their sources into
/// Libraries/, whose records name them here. Read from each part's .va (or
/// .osdi) in its library.
QSet<QString> alwaysLoadedModules(const QString& projectDir);

/// What sync() did, as paths relative to the project.
struct Report {
    QStringList made;       ///< linked (or copied) in
    QStringList removed;    ///< taken away: no schematic uses it any more
    QStringList conflicts;  ///< a file of the user's in the way: left as it is
    bool changed() const { return !made.isEmpty() || !removed.isEmpty(); }
};

/// Brings the project's library folders up to date with usedSources():
/// for each library a schematic uses a device of, Libraries/<library>/
/// (another name when that one is taken by another library: _2, _3...)
/// with a link to - or a copy of - each source at its path in the
/// library's folder. A link is relative (it holds when the project and the
/// library move together, or are another user's in the same places), made
/// again when it leads elsewhere (the library moved, another computer), a
/// copy where no link can be made (a disk without them). What no schematic
/// uses any more is taken away: the link or copy, the model compiled beside
/// it (NAME.osdi), the record, the folder when Qucs-S made it and nothing
/// else is in it, and Libraries/ so. Only what Qucs-S put there, as the
/// records say: a file of the user's is never written over or taken away.
/// A record's file is one in its folder - not by "..", an absolute path or
/// a link to a folder elsewhere - and a copy is taken away only while it
/// is the copy Qucs-S made (the record has its sum): a record edited by
/// hand, or come with a project from elsewhere, takes nothing else away.
/// Nothing is written through a folder that leads elsewhere, Libraries/
/// itself a link included. A library a part names that is not found here -
/// or none of that name has the part - keeps what it has. The
/// folders made in the project's folder itself before Libraries/ are moved.
Report sync(const QString& projectDir, const QList<Schematic*>& open = {}, Mode mode = defaultMode());

/// The folders of the libraries named \a library (a .lib's base name) the
/// project \a projectDir has the Verilog-A of in Libraries/ - its records
/// say where each was linked from -, real paths. Of two libraries of one
/// name that have a part, the part takes the one of these
/// (LibComp::libraryFileOf()).
QStringList linkedFolders(const QString& projectDir, const QString& library);
/// The folder of the library \a libraryFile as a record names it: NAME/
/// beside NAME.lib, its real path.
QString folderOf(const QString& libraryFile);

/// For the tests: how a symbolic link is made (QFile::link; none: that) -
/// one that fails stands for a disk that has no links.
void setLinkMaker(bool (*make)(const QString& target, const QString& link));

/// The library's source \a path is a link to or a copy of (sync() put it
/// there): the library's name and the original file. Empty when it is none.
struct Entry {
    QString library;
    QString original;
    bool isEmpty() const { return library.isEmpty(); }
};
Entry entryOf(const QString& path);

/// Why \a path must not be written, for a message: it is a library's
/// Verilog-A that a project keeps (sync()) - a link, which would write the
/// library's own file, shared by every project using it; or a copy, kept
/// in step with it, what is written lost. Told by the file itself, however
/// its path is spelled. Empty when it may be written.
QString notToWrite(const QString& path);

} // namespace qucs_s::projectlibraries

#endif
