/*
 * syntaxsettings.cpp - the Source Code Editor tab of the application
 *                      settings: how each language is highlighted
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "syntaxsettings.h"

#include "main.h"
#include "textdoc.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QVBoxLayout>

using qucs_s::syntax::Format;
using qucs_s::syntax::Style;

namespace {

QIcon swatch(const QColor& colour)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setPen(QColor(0x80, 0x80, 0x80));
    painter.setBrush(colour);
    painter.drawRect(QRect(0, 0, 13, 13));
    return QIcon(pixmap);
}

QString styleObjectName(const char* what, int language, Style style)
{
    return QStringLiteral("%1_%2_%3").arg(QLatin1String(what), qucs_s::syntax::key(language), qucs_s::syntax::styleKey(style));
}

} // namespace

SyntaxSettingsPage::SyntaxSettingsPage(QWidget* parent) : QWidget(parent)
{
    auto* all = new QVBoxLayout(this);
    auto* note = new QLabel(tr("The text editor highlights these languages, each in the colours set here. A file is "
                               "highlighted as the language of its suffix; the language button in the status bar "
                               "chooses another for the files of a suffix."),
                            this);
    note->setWordWrap(true);
    all->addWidget(note);

    auto* columns = new QHBoxLayout();
    all->addLayout(columns, 1);

    auto* list = new QGroupBox(tr("Language"), this);
    auto* listLayout = new QVBoxLayout(list);
    a_languages = new QButtonGroup(this);
    a_pages = new QStackedWidget(this);
    a_pages->setObjectName(QStringLiteral("syntaxPages"));
    for (int language : qucs_s::syntax::languages()) {
        if (qucs_s::syntax::styles(language).isEmpty()) continue;   // plain text
        auto* radio = new QRadioButton(qucs_s::syntax::name(language), list);
        radio->setObjectName(QStringLiteral("syntaxLanguage_") + qucs_s::syntax::key(language));
        listLayout->addWidget(radio);
        a_languages->addButton(radio, int(a_page.size()));
        a_pages->addWidget(makePage(language));
    }
    listLayout->addStretch(1);
    columns->addWidget(list);
    columns->addWidget(a_pages, 1);
    connect(a_languages, &QButtonGroup::idToggled, this, [this](int id, bool on) {
        if (on) a_pages->setCurrentIndex(id);
    });
    if (!a_page.isEmpty()) a_languages->button(0)->setChecked(true);
}

QWidget* SyntaxSettingsPage::makePage(int language)
{
    auto* widget = new QWidget(a_pages);
    widget->setObjectName(QStringLiteral("syntaxPage_") + qucs_s::syntax::key(language));
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);

    Page page;
    page.language = language;
    const int index = int(a_page.size());

    auto* styles = new QGroupBox(tr("Highlighting of %1").arg(qucs_s::syntax::name(language)), widget);
    auto* grid = new QGridLayout(styles);
    page.suffixes = new QLabel(styles);
    page.suffixes->setObjectName(QStringLiteral("syntaxSuffixes_") + qucs_s::syntax::key(language));
    page.suffixes->setWordWrap(true);
    QStringList suffixes;
    for (const QString& suffix : qucs_s::syntax::suffixesOf(language)) suffixes << "." + suffix;
    page.suffixes->setText(suffixes.isEmpty() ? tr("Files: none (the status bar's language button chooses some)")
                                              : tr("Files: %1").arg(suffixes.join(QStringLiteral(", "))));
    grid->addWidget(page.suffixes, 0, 0, 1, 4);
    grid->addWidget(new QLabel(tr("Colour"), styles), 1, 1);
    grid->addWidget(new QLabel(tr("Bold"), styles), 1, 2);
    grid->addWidget(new QLabel(tr("Italic"), styles), 1, 3);
    for (Style style : qucs_s::syntax::styles(language)) {
        const int line = int(page.rows.size()) + 2;
        auto* name = new QLabel(qucs_s::syntax::styleName(language, style) + ":", styles);
        Row row{style, new QPushButton(styles), new QCheckBox(styles), new QCheckBox(styles)};
        row.colour->setObjectName(styleObjectName("syntaxColour", language, style));
        row.bold->setObjectName(styleObjectName("syntaxBold", language, style));
        row.italic->setObjectName(styleObjectName("syntaxItalic", language, style));
        row.colour->setToolTip(tr("The colour of %1 in %2 (on white paper; fitted to a dark one)")
                                   .arg(qucs_s::syntax::styleName(language, style), qucs_s::syntax::name(language)));
        name->setBuddy(row.colour);
        grid->addWidget(name, line, 0);
        grid->addWidget(row.colour, line, 1);
        grid->addWidget(row.bold, line, 2, Qt::AlignHCenter);
        grid->addWidget(row.italic, line, 3, Qt::AlignHCenter);
        setRow(row, qucs_s::syntax::format(language, style));
        const int rowIndex = int(page.rows.size());
        connect(row.colour, &QPushButton::clicked, this, [this, index, rowIndex] { pickColour(index, rowIndex); });
        connect(row.bold, &QCheckBox::toggled, this, [this, index] { updatePreview(a_page[index]); });
        connect(row.italic, &QCheckBox::toggled, this, [this, index] { updatePreview(a_page[index]); });
        page.rows.append(row);
    }
    grid->setColumnStretch(4, 1);
    layout->addWidget(styles);

    layout->addWidget(new QLabel(tr("Preview:"), widget));
    page.preview = new QPlainTextEdit(widget);
    page.preview->setObjectName(QStringLiteral("syntaxPreview_") + qucs_s::syntax::key(language));
    page.preview->setReadOnly(true);
    page.preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    page.preview->setFont(QucsSettings.textFont);
    const auto [paper, ink] = TextDoc::paperAndInk();
    page.preview->setStyleSheet(QStringLiteral("QPlainTextEdit { background-color: %1; color: %2; }")
                                    .arg(paper.name(QColor::HexRgb), ink.name(QColor::HexRgb)));
    page.preview->setPlainText(qucs_s::syntax::sample(language));
    page.highlighter = new SyntaxHighlighter(page.preview);
    page.highlighter->setPaper(paper);
    page.highlighter->setLanguage(language);
    layout->addWidget(page.preview, 1);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* restore = new QPushButton(tr("Restore Defaults of %1").arg(qucs_s::syntax::name(language)), widget);
    restore->setObjectName(QStringLiteral("syntaxRestore_") + qucs_s::syntax::key(language));
    connect(restore, &QPushButton::clicked, this, [this, language] { restoreDefaults(language); });
    buttons->addWidget(restore);
    layout->addLayout(buttons);

    a_page.append(page);
    updatePreview(a_page.last());
    return widget;
}

void SyntaxSettingsPage::setRow(const Row& row, const Format& format)
{
    row.colour->setProperty("colour", format.colour);
    row.colour->setIcon(swatch(format.colour));
    row.colour->setText(format.colour.name(QColor::HexRgb));
    const QSignalBlocker boldQuiet(row.bold), italicQuiet(row.italic);
    row.bold->setChecked(format.bold);
    row.italic->setChecked(format.italic);
}

Format SyntaxSettingsPage::formatOf(const Row& row) const
{
    return Format{row.colour->property("colour").value<QColor>(), row.bold->isChecked(), row.italic->isChecked()};
}

void SyntaxSettingsPage::updatePreview(const Page& page)
{
    QHash<int, Format> formats;
    for (const Row& row : page.rows) formats.insert(int(row.style), formatOf(row));
    page.highlighter->setFormats(formats);
}

void SyntaxSettingsPage::pickColour(int page, int row)
{
    const Page& p = a_page[page];
    const Row& r = p.rows[row];
    const QColor colour = QColorDialog::getColor(
        formatOf(r).colour, this,
        tr("%1 in %2").arg(qucs_s::syntax::styleName(p.language, r.style), qucs_s::syntax::name(p.language)));
    if (!colour.isValid()) return;
    Format format = formatOf(r);
    format.colour = colour;
    setRow(r, format);
    updatePreview(p);
}

void SyntaxSettingsPage::select(int language)
{
    for (int i = 0; i < a_page.size(); ++i)
        if (a_page[i].language == language) a_languages->button(i)->setChecked(true);
}

int SyntaxSettingsPage::selected() const
{
    const int id = a_languages->checkedId();
    return id >= 0 && id < a_page.size() ? a_page[id].language : LANG_NONE;
}

void SyntaxSettingsPage::apply()
{
    for (const Page& page : std::as_const(a_page))
        for (const Row& row : page.rows) qucs_s::syntax::setFormat(page.language, row.style, formatOf(row));
}

void SyntaxSettingsPage::restoreDefaults()
{
    for (const Page& page : std::as_const(a_page)) restoreDefaults(page.language);
}

void SyntaxSettingsPage::restoreDefaults(int language)
{
    for (const Page& page : std::as_const(a_page)) {
        if (page.language != language) continue;
        for (const Row& row : page.rows) setRow(row, qucs_s::syntax::defaultFormat(language, row.style));
        updatePreview(page);
    }
}
