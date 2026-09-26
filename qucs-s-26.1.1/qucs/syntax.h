/***************************************************************************
                                syntax.h
                               ----------
    begin                : Sat Mar 11 2006
    copyright            : (C) 2006 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef SYNTAX_H
#define SYNTAX_H

#include <QColor>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

// The languages the text editor highlights. The first four are those it
// always had (the numbers are kept); LANG_NONE is plain text.
enum language_type {
  LANG_NONE = 0,
  LANG_VHDL,
  LANG_VERILOG,
  LANG_VERILOGA,
  LANG_OCTAVE,
  LANG_SPICE,
  LANG_QUCS_NETLIST,
  LANG_PYTHON,
  LANG_CPP,
  LANG_SHELL,
  LANG_MARKDOWN,
  LANG_JSON,
  LANG_XML,
  LANG_COUNT
};

namespace qucs_s::syntax {

/// The kinds of text a language picks out; each language highlights some
/// of them, under names of its own ("Dot command" for a Directive of SPICE).
enum class Style {
  Keyword, Type, Builtin, Directive, Element, Variable, Number, String, Unit,
  Comment, Heading, Emphasis, Code, Link, Tag, Attribute, Key,
  Count
};

/// How a style is drawn: its colour (meant for white paper, fitted to a
/// dark one), bold, italic.
struct Format {
  QColor colour;
  bool bold = false;
  bool italic = false;
  bool operator==(const Format& other) const
  {
    return colour.rgb() == other.colour.rgb() && bold == other.bold && italic == other.italic;
  }
  bool operator!=(const Format& other) const { return !(*this == other); }
};

/// Every language, plain text first, then by name: the order of the menus.
QList<int> languages();
/// What a language is saved under ("python"; plain text: "plain").
QString key(int language);
/// -1 when no language has that key.
int byKey(const QString& key);
/// Its name, as the menus show it ("Python").
QString name(int language);

/// The suffixes a language has by default (lower case, without the dot).
QStringList defaultSuffixes(int language);
/// The language of files with \a suffix by default; LANG_NONE for none.
int defaultLanguageFor(const QString& suffix);
/// The language of a file: the one chosen for its suffix (in the status
/// bar, QucsSettings.SyntaxForSuffix), else its default one.
int languageFor(const QString& fileName);
/// Files with \a suffix are highlighted as \a language from now on (its
/// default language: the choice is forgotten); the choice is saved at once.
void chooseFor(const QString& suffix, int language);
/// Whether a language was chosen for \a suffix.
bool chosenFor(const QString& suffix);
/// The suffixes whose files are highlighted as \a language: its default
/// ones less those chosen for another, and those chosen for it.
QStringList suffixesOf(int language);

/// The styles a language highlights, in the order the settings list them.
QList<Style> styles(int language);
/// A style's name, in a language ("Dot command"); in general ("Directive")
/// for LANG_NONE.
QString styleName(int language, Style style);
/// What a style is saved under ("Keyword").
QString styleKey(Style style);

Format defaultFormat(int language, Style style);
/// As the settings have it (QucsSettings.SyntaxFormats), else the default.
Format format(int language, Style style);
/// Kept when it differs from the default, forgotten otherwise.
void setFormat(int language, Style style, const Format& format);
/// "#0000ff bold italic", and back (\a fallback where it cannot be read).
QString formatText(const Format& format);
Format parseFormat(const QString& text, const Format& fallback);

/// Some text of the language, with a line of each of its styles: the
/// settings' preview.
QString sample(int language);

/// The language chosen for each suffix, written to the settings file.
void saveChoices();

} // namespace qucs_s::syntax

class SyntaxHighlighter : public QSyntaxHighlighter {
public:
  /// On a text editor (QPlainTextEdit): its document; on a QTextDocument:
  /// that one. Otherwise setDocument() gives it one.
  explicit SyntaxHighlighter(QObject* parent);
  ~SyntaxHighlighter() override;

  void setLanguage(int language);
  int language() const { return a_language; }
  /// The colours, meant for white, fitted to the editor's paper
  /// (ink::on()); the text highlighted again.
  void setPaper(const QColor& paper);
  /// Formats of its own (the settings' preview) rather than the settings';
  /// the text highlighted again.
  void setFormats(const QHash<int, qucs_s::syntax::Format>& formats);
  /// The formats taken again from the settings, the text highlighted again.
  void reload();

protected:
  void highlightBlock(const QString& text) override;

private:
  int a_language = LANG_NONE;
  QColor a_paper = Qt::white;
  QHash<int, qucs_s::syntax::Format> a_own;   // setFormats()
  QTextCharFormat a_formats[int(qucs_s::syntax::Style::Count)];

  void makeFormats();
  void scanSpans(const QString& text);
};

#endif
