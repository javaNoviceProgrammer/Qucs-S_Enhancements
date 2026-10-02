/*
 * namefilter.h - what a "Filter by name" box finds: the File Browser's,
 *                the Projects and Content panels', a ZIP archive's
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_NAMEFILTER_H
#define QUCS_NAMEFILTER_H

#include <QRegularExpression>
#include <QString>

class QLineEdit;

namespace qucs_s::files {

/// A filter's text, as the "Filter by name" boxes take it: a regular
/// expression, looked for anywhere in a name whatever its case (^amp,
/// \.sch$, amp|filter, r\d+) - and a name that holds the text as typed
/// is found too (file(1), C++). A text that is no regular expression
/// (*.sch, "amp(") is only the text. Empty: every name.
class NameFilter
{
public:
    NameFilter() = default;
    explicit NameFilter(const QString& text);
    QString text() const { return a_text; }
    bool isEmpty() const { return a_text.isEmpty(); }
    /// Whether it finds \a name.
    bool matches(const QString& name) const;
    /// Whether it finds \a path (folder/name, "folder/" for a folder) or
    /// the name at its end: ^amp finds models/amp.sch.
    bool matchesPath(const QString& path) const;
    /// Why the text is no regular expression; empty when it is one.
    QString error() const;

private:
    QString a_text;
    QRegularExpression a_pattern;
};

/// Has \a edit say how its text is taken - in its tooltip - and, while
/// the text is no regular expression, a warning in it that says why.
void explainNameFilter(QLineEdit* edit);

} // namespace qucs_s::files

#endif // QUCS_NAMEFILTER_H
