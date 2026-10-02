/*
 * namefilter.cpp - what a "Filter by name" box finds: the File Browser's,
 *                  the Projects and Content panels', a ZIP archive's
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "namefilter.h"

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QStyle>

#include <algorithm>

namespace qucs_s::files {

namespace {

QString trn(const char* text)
{
    return QCoreApplication::translate("NameFilter", text);
}

} // namespace

NameFilter::NameFilter(const QString& text)
    : a_text(text),
      a_pattern(text, QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption)
{
}

bool NameFilter::matches(const QString& name) const
{
    if (a_text.isEmpty() || name.contains(a_text, Qt::CaseInsensitive)) return true;
    return a_pattern.isValid() && a_pattern.match(name).hasMatch();
}

bool NameFilter::matchesPath(const QString& path) const
{
    if (matches(path)) return true;
    QString name = path;
    while (name.endsWith(QLatin1Char('/'))) name.chop(1);
    name = name.section(QLatin1Char('/'), -1);
    return name != path && matches(name);
}

QString NameFilter::error() const
{
    if (a_text.isEmpty() || a_pattern.isValid()) return {};
    // (The offset is just past the character at fault: that character's, counted from 1.)
    return trn("%1 at character %2").arg(a_pattern.errorString()).arg(std::max<qsizetype>(1, a_pattern.patternErrorOffset()));
}

void explainNameFilter(QLineEdit* edit)
{
    const QString how = trn("The names that hold the text, or in which it finds a match as a regular expression - ^amp, "
                            "\\.sch$, amp|filter -, whatever their case");
    edit->setToolTip(how);
    QAction* warning = edit->addAction(QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning), QLineEdit::TrailingPosition);
    warning->setObjectName(QStringLiteral("nameFilterNoRegex"));
    warning->setVisible(false);
    QObject::connect(edit, &QLineEdit::textChanged, warning, [edit, warning, how](const QString& text) {
        const QString error = NameFilter(text.trimmed()).error();
        const QString why = error.isEmpty() ? QString()
            : trn("Not a regular expression (%1): the names that hold the text as typed are found").arg(error);
        warning->setToolTip(why);
        warning->setVisible(!error.isEmpty());
        edit->setToolTip(error.isEmpty() ? how : why);
    });
}

} // namespace qucs_s::files
