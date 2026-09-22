/*
 * componentsearch.h - finding components (by name, net label or property
 * value) in a schematic, and replacing property values
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_COMPONENTSEARCH_H
#define QUCS_COMPONENTSEARCH_H

#include <QList>
#include <QPoint>
#include <QString>

class Component;
class Schematic;

namespace qucs_s::search {

/// What to look for, and where.
struct Query {
    QString text;              ///< empty: anything (every value of the property asked for)
    bool names = false;        ///< component names (refdes) and net labels
    bool values = true;        ///< property values
    bool matchCase = false;
    bool wholeValue = false;   ///< the name or value is the text, rather than contains it
    bool regex = false;        ///< the text is a regular expression
    QString model;             ///< only components of this model (empty: any)
    QString namePattern;       ///< only components whose name fits this wildcard (empty: any)
    QString property;          ///< only this property (empty: any)

    /// Whether the text can be used (a regular expression that compiles);
    /// \a error gets the reason when it cannot.
    bool isValid(QString* error = nullptr) const;

    bool operator==(const Query&) const = default;
};

/// One place a query matched.
struct Match {
    enum Kind { Name, Label, Value };
    Kind kind = Value;
    QString component;   ///< the component's name; for a label, the label's text
    QString model;       ///< the component's model (empty for a label)
    QString property;    ///< for a value: the property's name
    QString value;       ///< what matched: the name, the label or the value
    QPoint where;        ///< model coordinates: the component's centre, the label's root

    bool operator==(const Match&) const = default;
};

/// Every match of \a query in \a doc: component names (the exact ones
/// first), then net labels, then property values, each in document order.
/// Nothing in symbol-editing mode, where the components are not on show.
QList<Match> find(const Schematic* doc, const Query& query);

/// \a value with what \a query matches in it replaced by \a with: the
/// whole value when the query has no text or asks for whole values, the
/// matching parts otherwise (a regular expression's replacement may refer
/// to its groups as \1 ...).
QString replaced(const QString& value, const Query& query, const QString& with);

/// Sets the value of each of \a matches (values only) to what replaced()
/// makes of it, recreating the component. A match whose component or
/// value is no longer as it was found, or whose new value would be empty
/// or contain a double quote (which the file format cannot hold), is left
/// alone and counted in \a skipped. Returns how many values changed; the
/// caller records the undo step.
int replace(Schematic* doc, const QList<Match>& matches, const Query& query, const QString& with,
            int* skipped = nullptr);

/// The component a match is about: the one of that name at that place,
/// or the only one of that name. nullptr for a label or when it is gone.
Component* componentOf(const Schematic* doc, const Match& match);

/// Selects what \a match is about (and nothing else) and puts it in the
/// middle of the view: a component; a label together with a wire of its
/// net, so that the net lights up.
void reveal(Schematic* doc, const Match& match);

} // namespace qucs_s::search

#endif
