/*
 * schematicdiff.h - what changed in a schematic, part by part: the parts
 *                   added, removed, moved, turned, their properties; the
 *                   wires, labels, diagrams, paintings and settings - for
 *                   a diff, a commit, a conflict's sides, where git's own
 *                   diff gives lines
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_SCHEMATICDIFF_H
#define QUCS_SCHEMATICDIFF_H

#include "gitrepo.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace qucs_s::git {

/// Whether \a path is a schematic, a data display or a symbol (.sch,
/// .dpl, .sym): a file a schematic's changes are told of.
bool isSchematicFile(const QString& path);

/// What changed from \a before to \a after - two texts of one schematic
/// (absent: the file did not exist) - one line each: a part added,
/// removed, moved, turned, mirrored, (de)activated, its type or
/// properties changed (by their names, as the part's type has them); the
/// wires added and removed, the labels by name; the diagrams and
/// paintings, the symbol's; the settings, and the version that wrote it.
/// Its view (View=, the scroll and zoom) is left out. Empty when nothing
/// else changed.
QStringList schematicChanges(const std::optional<QString>& before, const std::optional<QString>& after);

/// The changes of the schematics not committed in \a root - under \a path
/// alone when given - as \a of has them (DiffOf): a block of text, a
/// heading per file, empty when none changed.
QString schematicChangesOf(const QString& root, const QString& path = {}, DiffOf of = DiffOf::Head);
/// The changes a commit made to its schematics (under \a path alone when
/// given), the same way.
QString schematicChangesIn(const QString& root, const QString& commit, const QString& path = {});
/// A conflict's sides, each against where both started: what mine changed,
/// what theirs changed.
QString schematicConflictChanges(const ConflictVersions& versions);

} // namespace qucs_s::git

#endif // QUCS_SCHEMATICDIFF_H
