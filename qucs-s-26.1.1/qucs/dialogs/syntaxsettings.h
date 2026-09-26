/*
 * syntaxsettings.h - the Source Code Editor tab of the application
 *                    settings: how each language is highlighted
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef SYNTAXSETTINGS_H
#define SYNTAXSETTINGS_H

#include <QList>
#include <QWidget>

#include "syntax.h"

class QButtonGroup;
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;

/*!
 * Every language the text editor highlights, each with a radio button;
 * the one chosen shows its styles - a colour, bold, italic - on a sample
 * of the language highlighted as they are set. The changes of every
 * language are kept until apply() puts them in the settings.
 */
class SyntaxSettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SyntaxSettingsPage(QWidget* parent = nullptr);

    /// Shows \a language (language_type) and checks its radio button.
    void select(int language);
    int selected() const;
    /// The formats set for every language into the settings
    /// (QucsSettings.SyntaxFormats, saved with them).
    void apply();
    /// Every language's styles back to their defaults (not yet applied).
    void restoreDefaults();
    /// \a language's styles back to their defaults.
    void restoreDefaults(int language);

private:
    struct Row {
        qucs_s::syntax::Style style;
        QPushButton* colour;
        QCheckBox* bold;
        QCheckBox* italic;
    };
    struct Page {
        int language;
        QList<Row> rows;
        QLabel* suffixes;
        QPlainTextEdit* preview;
        SyntaxHighlighter* highlighter;
    };

    QButtonGroup* a_languages;
    QStackedWidget* a_pages;
    QList<Page> a_page;

    QWidget* makePage(int language);
    void setRow(const Row& row, const qucs_s::syntax::Format& format);
    qucs_s::syntax::Format formatOf(const Row& row) const;
    void updatePreview(const Page& page);
    void pickColour(int page, int row);
};

#endif // SYNTAXSETTINGS_H
