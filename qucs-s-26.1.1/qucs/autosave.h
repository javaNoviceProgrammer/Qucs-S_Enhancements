/***************************************************************************
                                 autosave.h
                                ------------
    Periodic and emergency copies of open documents, and their recovery.
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QUCS_AUTOSAVE_H
#define QUCS_AUTOSAVE_H

#include <QDateTime>
#include <QList>
#include <QString>

class QucsDoc;

/*!
 * Autosave keeps a copy of every modified document in a private directory
 * (see directory()), written by a timer while the application runs and,
 * best effort, by the crash handler when it is about to die. Each copy is
 * <key>.<suffix> plus a <key>.meta sidecar that records where it came from.
 * The copy is removed when the document is saved or closed on purpose;
 * whatever is left on the next start is offered for recovery.
 *
 * A document is identified by its file name, or, for untitled documents,
 * by an id the caller provides (e.g. the tab index) so that several
 * untitled documents do not overwrite each other.
 */
namespace qucs_s::autosave {

struct Entry {
    QString key;          // file name stem in directory()
    QString path;         // the autosaved copy
    QString original;     // the document's file name, empty if untitled
    bool    untitled = false;
    bool    schematic = true;   // false: text document
    QDateTime when;
};

/// Where the copies live. Created on first use.
QString directory();

/// Override the directory (tests). An empty string restores the default.
void setDirectory(const QString& dir);

/// Writes a copy of \a doc. \a untitledId distinguishes untitled documents.
/// Returns the path of the copy, or an empty string on failure.
QString write(QucsDoc* doc, int untitledId = 0);

/// Removes the copy belonging to a named document. No-op if there is none.
void remove(const QString& originalPath);

/// Removes the copy of an untitled document.
void removeUntitled(int untitledId, bool schematic);

/// Removes every copy (after a recovery, or when the user declines it).
void clear();

/// The copies currently on disk, oldest first.
QList<Entry> pending();

} // namespace qucs_s::autosave

#endif // QUCS_AUTOSAVE_H
