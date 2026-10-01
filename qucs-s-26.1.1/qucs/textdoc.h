/***************************************************************************
                                textdoc.h
                               -----------
Copyright (C) 2006 by Michael Margraf <michael.margraf@alumni.tu-berlin.de>
Copyright (C) 2014 by Guilherme Brondani Torri <guitorri@gmail.com>

 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef TEXTDOC_H
#define TEXTDOC_H

#include <QPlainTextEdit>
#include <QFont>
#include <QMargins>
#include <QColor>
#include <utility>
#include <qdatetime.h>

#include "qucsdoc.h"
#include "textcodec.h"

/*!
 * \file textdoc.h
 * \brief Definition of the TextDoc class.
 */

class SyntaxHighlighter;
class QString;
class LineNumberArea;

// device type flags
#define DEV_BJT      0x0001
#define DEV_MOS      0x0002
#define DEV_MASK_DEV 0x00FF
#define DEV_DIG      0x0100
#define DEV_ANA      0x0200
#define DEV_ALL      0x0300
#define DEV_MASK_TYP 0xFF00
#define DEV_DEF      0x0200 // default value

/*!
 * \brief The TextDoc class definition
 */
class TextDoc : public QPlainTextEdit, public QucsDoc {
  Q_OBJECT
public:
  TextDoc (QucsApp *, const QString&);
 ~TextDoc ();

  void  setName (const QString&);
  bool  load ();
  bool  reload ();
  bool  hasFileChangedOnDisk() const;
  int   save ();
  bool  writeTo(const QString& path) override;
  /// How its file's bytes are characters (textcodec.h): found when it was
  /// read, and how save() writes it back - UTF-8 once a character it has
  /// no bytes for was typed and the user agreed.
  const qucs_s::textcodec::Encoding& encoding() const { return a_encoding; }
  /// Its file's lines end in CR LF (and are written back so).
  bool crlf() const { return a_crlf; }

  /// A finding shown in the text (build_verilog_a's errors and warnings):
  /// its line and column (1-based; column 0: the whole line), its message,
  /// and whether it is an error (else a warning).
  struct Diagnostic {
    int line = 0;
    int column = 0;
    QString message;
    bool error = true;
  };
  /// Shows \a list in the text until the next list (an empty one takes
  /// them away): a wavy underline from the column to the line's end - red
  /// for an error, amber for a warning -, a dot in the line numbers'
  /// margin, and the message as the line's tooltip. They move with the
  /// text as it is edited.
  void setDiagnostics(const QList<Diagnostic>& list);
  /// Those shown, at their lines now.
  QList<Diagnostic> diagnostics() const;
  /// The messages shown for the line at \a y (the margin's or the
  /// viewport's coordinates, as the margin is level with the viewport),
  /// one a line; empty for none.
  QString diagnosticsAtY(int y) const;
  virtual double zoomBy (double zoom) override;
  virtual void showNoZoom () override;
  void  becomeCurrent (bool);
  bool  loadSimulationTime (QString&);
  void  commentSelected ();
  void  insertSkeleton ();
  void  setLanguage (int);
  void  setLanguage (const QString&);
  /// Highlights it as \a language from now on: every file with its suffix
  /// (qucs_s::syntax::chooseFor()), or this document alone when it has none.
  void  chooseLanguage (int language);
  /// The paper and the ink of the editor under the current theme.
  static std::pair<QColor, QColor> paperAndInk();
  QString getModuleName (void);

  virtual void wheelEvent(QWheelEvent* event) override;
  // Files dropped on the editor are opened in their own tabs; other drags
  // (text) are handled by the editor as usual.
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dropEvent(QDropEvent* event) override;

  bool simulation;   // simulation or module
  QString Library;   // library this document belongs to
  QString Libraries; // libraries to be linked with
  QString ShortDesc; // icon description
  QString LongDesc;  // component description
  QString Icon;      // icon file
  bool recreate;     // recreate output file
  int devtype;       // device type

  bool SetChanged;
  QString a_defaultSettings;   // settingsText() of a new document, as made

  int a_textRevision = -1;   // the document's revision() last counted as an edit
  bool a_countsEdits = false;   // set up (and loaded): its changes are edits
  int language;   // language_type (syntax.h): its highlighting, comments, skeletons
  int a_chosenLanguage = -1;   // chosen for it alone (it has no suffix), -1: its file's
  struct ShownDiagnostic {
    QTextCursor at;   // where it is: moves with the text
    Diagnostic diagnostic;
  };
  QList<ShownDiagnostic> a_diagnostics;   // setDiagnostics()'s

  bool loadSettings (void);
  bool saveSettings (void);
  QString settingsText () const;   // the settings file's lines below its header
  /// Whether saving writes the settings file (name.cfg): always when the
  /// user wants it (QucsSettings.WriteTextDocSettings), else when the
  /// settings are not a new document's or were changed since the last save.
  bool writesSettings () const;
  void refreshLanguage(void);
  void applyDocumentColors();

  QMenu* createStandardContextMenu();

  void lineNumberAreaPaintEvent(QPaintEvent *event);
  int lineNumberAreaWidth() const;

signals:
  void signalCursorPosChanged(int, int, QString);
  void signalFileChanged(bool);
  void signalUndoState(bool);
  void signalRedoState(bool);

public slots:
  void search(const QString &str, bool CaseSensitive, bool wordOnly, bool backward);
  void replace(const QString &str, const QString &str2, bool needConfirmed,
               bool CaseSensitive, bool wordOnly, bool backward);
  void slotCursorPosChanged ();
  void slotSetChanged ();

protected:
      void resizeEvent(QResizeEvent *event) override;
      /// A line's diagnostics as its tooltip.
      bool viewportEvent(QEvent *event) override;
      /// Room around the text beside the line numbers' (a subclass puts
      /// widgets of its own there: MarkdownDoc its bar and preview).
      virtual QMargins extraMargins() const { return {}; }
      /// The text's margins set again (after extraMargins() changed).
      void updateMargins() { updateLineNumberAreaWidth(0); }

private:
  SyntaxHighlighter * syntaxHighlight = nullptr;
  // The editor's colours (applyDocumentColors()): its paper, the current
  // line, the line numbers' margin and their colour.
  QColor a_paper = Qt::white;
  QColor a_currentLine;
  QColor a_margin = Qt::lightGray;
  QColor a_marginText = Qt::black;
  QDateTime lastLoadModTime; // Timestamp of last successful load
  qucs_s::textcodec::Encoding a_encoding;   // of its file (encoding())
  bool a_crlf = false;                      // its file's lines end in CR LF
  /// The text as its file's bytes (encoding(), crlf()). A character the
  /// encoding has no bytes for (Ω in a Windows-1252 file): with \a ask,
  /// UTF-8 once the user agrees (false when they do not: not saved);
  /// without, UTF-8 for this writing alone.
  bool encodedText(QByteArray* bytes, bool ask);
  LineNumberArea *lineNumberArea = nullptr;

private slots:
  void highlightCurrentLine();
  bool baseSearch(const QString &, bool, bool, bool);

  // Line numbering
  void updateLineNumberAreaWidth(int newBlockCount);
  void updateLineNumberArea(const QRect &rect, int dy);
};

// Line numbering widget
class LineNumberArea : public QWidget
{
public:
    LineNumberArea(TextDoc *editor) : QWidget(editor), codeEditor(editor)
    {}

    QSize sizeHint() const override
    {
        return QSize(codeEditor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        codeEditor->lineNumberAreaPaintEvent(event);
    }
    // A line's diagnostics over its dot.
    bool event(QEvent *event) override;

private:
    TextDoc *codeEditor;
};

#endif
