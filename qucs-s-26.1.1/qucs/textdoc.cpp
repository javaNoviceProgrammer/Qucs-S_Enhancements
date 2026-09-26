/***************************************************************************
                               textdoc.cpp
                              -------------
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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <QAction>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QTextStream>
#include <QPainter>
#include <qpalette.h>

#include "main.h"
#include "misc.h"
#include "qucs.h"
#include "textdoc.h"
#include "syntax.h"
#include "apptheme.h"
#include "components/vhdlfile.h"
#include "components/verilogfile.h"
#include "components/vafile.h"

/*!
 * \file textdoc.cpp
 * \brief Implementation of the TextDoc class.
 */

/*!
 * \brief TextDoc::TextDoc Text document constructor
 * \param App_ is the parent object
 * \param Name_ is the initial text document name
 */
TextDoc::TextDoc(QucsApp *App_, const QString& Name_) : QPlainTextEdit(), QucsDoc(App_, Name_)
{
  setFont(QucsSettings.textFont);

  simulation = true;
  Library = "";
  Libraries = "";
  SetChanged = false;
  recreate = false;   // written with the settings; read back only when a file has it
  devtype = DEV_DEF;

  a_tmpPosX = a_tmpPosY = 1;  // set to 1 to trigger line highlighting
  setLanguage (Name_);

  viewport()->setFocus();

  setWordWrapMode(QTextOption::NoWrap);
  applyDocumentColors();
  connect(this, SIGNAL(textChanged()), SLOT(slotSetChanged()));
  connect(this, SIGNAL(cursorPositionChanged()),
          SLOT(slotCursorPosChanged()));
  if (App_) {
    connect(this, SIGNAL(signalCursorPosChanged(int, int, QString)),
        App_, SLOT(printCursorPosition(int, int, QString)));
    connect(this, SIGNAL(signalUndoState(bool)),
        App_, SLOT(slotUpdateUndo(bool)));
    connect(this, SIGNAL(signalRedoState(bool)),
        App_, SLOT(slotUpdateRedo(bool)));
    connect(this, SIGNAL(signalFileChanged(bool)),
        App_, SLOT(slotFileChanged(bool)));
  }

  syntaxHighlight = new SyntaxHighlighter(this);   // on its document
  syntaxHighlight->setLanguage(language);
  syntaxHighlight->setPaper(a_paper);

  connect(this, SIGNAL(cursorPositionChanged()), this, SLOT(highlightCurrentLine()));
  highlightCurrentLine();

  // Line numbering
  lineNumberArea = new LineNumberArea(this);

  connect(this, SIGNAL(blockCountChanged(int)), this, SLOT(updateLineNumberAreaWidth(int)));
  connect(this, SIGNAL(updateRequest(QRect,int)), this, SLOT(updateLineNumberArea(QRect,int)));
  connect(this, SIGNAL(cursorPositionChanged()), SLOT(highlightCurrentLine()));

  updateLineNumberAreaWidth(0);
  // A new document: what is typed from now on. (One read from a file
  // counts from the end of its load().)
  if (a_DocName.isEmpty()) {
    a_textRevision = document()->revision();
    a_countsEdits = true;
  }
}

/*!
 * \brief TextDoc::~TextDoc Text document destructor
 */
TextDoc::~TextDoc()
{
  // Detaching the highlighter edits the QTextDocument, which would emit
  // textChanged() -> slotSetChanged() -> signalFileChanged() into the
  // application while this object is half destroyed.
  disconnect(this, SIGNAL(textChanged()), this, SLOT(slotSetChanged()));
  delete syntaxHighlight;
}

/*!
 * \brief TextDoc::setLanguage(const QString&)
 * \param FileName Text document file name
 * Extract the file name suffix and assign a language_type to it.
 */
void TextDoc::setLanguage (const QString& FileName)
{
  // The language chosen for this document alone, else the one of its
  // suffix (chosen in the status bar, or the default one).
  setLanguage (a_chosenLanguage >= 0 ? a_chosenLanguage : qucs_s::syntax::languageFor (FileName));
}

void TextDoc::chooseLanguage (int lang)
{
  const QString suffix = QFileInfo (a_DocName).suffix ();
  if (suffix.isEmpty ()) {
    a_chosenLanguage = lang;
  } else {
    a_chosenLanguage = -1;
    qucs_s::syntax::chooseFor (suffix, lang);
  }
  refreshLanguage ();
}

/*!
 * \brief TextDoc::setLanguage(int)
 * \param lang is a language_type
 * Assign value to text document object language variable
 */
void TextDoc::setLanguage (int lang)
{
  language = lang;
}

/*!
 * \brief TextDoc::saveSettings saves the text document settings .cfg
 * \return true/false if settings file opened with success
 */
bool TextDoc::saveSettings (void)
{
  QFile file (a_DocName + ".cfg");
  if (!file.open (QIODevice::WriteOnly))
    return false;

  QTextStream stream (&file);
  stream << "Textfile settings file, Qucs " PACKAGE_VERSION "\n"
    << "Simulation=" << simulation << "\n"
    << "Duration=" << a_SimTime << "\n"
    << "Module=" << (!simulation) << "\n"
    << "Library=" << Library << "\n"
    << "Libraries=" << Libraries << "\n"
    << "ShortDesc=" << ShortDesc << "\n"
    << "LongDesc=" << LongDesc << "\n"
    << "Icon=" << Icon << "\n"
    << "Recreate=" << recreate << "\n"
    << "DeviceType=" << devtype << "\n";

  file.close ();
  SetChanged = false;
  return true;
}

/*!
 * \brief TextDoc::loadSettings loads the text document settings
 * \return true/false if settings file opened with success
 */
bool TextDoc::loadSettings (void)
{
  QFile file (a_DocName + ".cfg");
  if (!file.open (QIODevice::ReadOnly))
    return false;

  QTextStream stream (&file);
  QString Line, Setting;

  bool ok;
  while (!stream.atEnd ()) {
    Line = stream.readLine ();
    Setting = Line.section ('=', 0, 0);
    Line = Line.section ('=', 1).trimmed ();
    if (Setting == "Simulation") {
      simulation = Line.toInt (&ok);
    } else if (Setting == "Duration") {
      a_SimTime = Line;
    } else if (Setting == "Module") {
    } else if (Setting == "Library") {
      Library = Line;
    } else if (Setting == "Libraries") {
      Libraries = Line;
    } else if (Setting == "ShortDesc") {
      ShortDesc = Line;
    } else if (Setting == "LongDesc") {
      LongDesc = Line;
    } else if (Setting == "Icon") {
      Icon = Line;
    } else if (Setting == "Recreate") {
      recreate = Line.toInt (&ok);
    } else if (Setting == "DeviceType") {
      devtype = Line.toInt (&ok);
    }
  }

  file.close ();
  return true;
}

/*!
 * \brief TextDoc::setName sets the text file name on its tab
 * \param Name_ text file name to be set
 */
void TextDoc::setName (const QString& Name_)
{
  a_DocName = Name_;
  setLanguage (a_DocName);

  QFileInfo Info (a_DocName);

  a_DataSet = Info.baseName () + ".dat";
  a_DataDisplay = Info.baseName () + ".dpl";
  if(Info.suffix() == "m" || Info.suffix() == "oct")
    a_SimTime = "1";
}

/*!
 * \brief TextDoc::becomeCurrent sets text document as current
 *
 * \detail Make sure the menu options are adjusted.
 */
void TextDoc::becomeCurrent (bool)
{
  slotCursorPosChanged();
  viewport()->setFocus ();

  emit signalUndoState(document()->isUndoAvailable());
  emit signalRedoState(document()->isRedoAvailable());

  // update appropriate menu entries
  a_App->symEdit->setText (tr("Edit Text Symbol"));
  a_App->symEdit->setStatusTip (tr("Edits the symbol for this text document"));
  a_App->symEdit->setWhatsThis (
    tr("Edit Text Symbol\n\nEdits the symbol for this text document"));

  if (language == LANG_VHDL) {
    a_App->insEntity->setText (tr("VHDL entity"));
    a_App->insEntity->setStatusTip (tr("Inserts skeleton of VHDL entity"));
    a_App->insEntity->setWhatsThis (
      tr("VHDL entity\n\nInserts the skeleton of a VHDL entity"));
  }
  else if (language == LANG_VERILOG || language == LANG_VERILOGA) {
    a_App->insEntity->setText (tr("Verilog module"));
    a_App->insEntity->setStatusTip (tr("Inserts skeleton of Verilog module"));
    a_App->insEntity->setWhatsThis (
      tr("Verilog module\n\nInserts the skeleton of a Verilog module"));
    a_App->buildModule->setEnabled(true);
  }
  else if (language == LANG_OCTAVE) {
    a_App->insEntity->setText (tr("Octave function"));
    a_App->insEntity->setStatusTip (tr("Inserts skeleton of Octave function"));
    a_App->insEntity->setWhatsThis (
      tr("Octave function\n\nInserts the skeleton of a Octave function"));
  }
  a_App->simulate->setEnabled (true);
  a_App->editActivate->setEnabled (true);
}

bool TextDoc::baseSearch(const QString &str, bool CaseSensitive, bool wordOnly, bool backward)
{
  QTextDocument::FindFlags flag = QTextDocument::FindFlags();
  bool finded;

  if (CaseSensitive) {
    flag = QTextDocument::FindCaseSensitively;
  }
  if (backward) {
    flag = flag | QTextDocument::FindBackward;
  }
  if (wordOnly) {
    flag = flag | QTextDocument::FindWholeWords;
  }

  finded = find(str, flag);
  if (!finded) {
    if (!backward) {
      moveCursor(QTextCursor::Start);
    } else {
      moveCursor(QTextCursor::End);
    }
    finded = find(str, flag);
    if (!finded) {
      QMessageBox::information(this,
          tr("Find..."), tr("Cannot find target: %1").arg(str),
          QMessageBox::Ok | QMessageBox::Default | QMessageBox::Escape);
    }
  }
  return finded;
}

// implement search function
// if target not find, auto revert to head, find again
// if still not find, pop out message
void TextDoc::search(const QString &str, bool CaseSensitive, bool wordOnly, bool backward)
{
  baseSearch(str, CaseSensitive, wordOnly, backward);
}

// implement replace function
void TextDoc::replace(const QString &str, const QString &str2, bool needConfirmed,
                      bool CaseSensitive, bool wordOnly, bool backward)
{
  bool finded = baseSearch(str, CaseSensitive, wordOnly, backward);
  int i;

  if(finded) {
    i = QMessageBox::Yes;
    if (needConfirmed) {
      i = QMessageBox::information(this,
          tr("Replace..."), tr("Replace occurrence ?"),
          QMessageBox::Yes|QMessageBox::No);
    }
    if(i == QMessageBox::Yes) {
      insertPlainText(str2);
    }
  }
}


/*!
 * \brief TextDoc::slotCursorPosChanged update status bar with line:column
 */
void TextDoc::slotCursorPosChanged()
{
  QTextCursor pos = textCursor();
  int x = pos.blockNumber();
  int y = pos.columnNumber();
  emit signalCursorPosChanged(x+1, y+1, "");
  a_tmpPosX = x;
  a_tmpPosY = y;
}

/*!
 * \brief TextDoc::slotSetChanged togles tab icon to indicate unsaved changes
 */
void TextDoc::slotSetChanged()
{
  // An edit of the text (typed, undone) - not a highlighting, nor what
  // setting the document up and loading it do (load() counts itself).
  if (a_countsEdits && document()->revision() != a_textRevision) {
    a_textRevision = document()->revision();
    edited();
  }
  if((document()->isModified() && !a_DocChanged) || SetChanged) {
    a_DocChanged = true;
  }
  else if((!document()->isModified() && a_DocChanged)) {
    a_DocChanged = false;
  }
  emit signalFileChanged(a_DocChanged);
  emit signalUndoState(document()->isUndoAvailable());
  emit signalRedoState(document()->isRedoAvailable());
}

/*!
 * \brief TextDoc::createStandardContextMenu creates the standard context menu
 * \param pos
 * \return
 *
 *  \todo \fixme is this working?
 */
QMenu *TextDoc::createStandardContextMenu()
{
  QMenu *popup = QPlainTextEdit::createStandardContextMenu();

   if (language != LANG_OCTAVE) {
       ((QWidget *) popup)->addAction(a_App->fileSettings);
   }
   return popup;
}

/*!
 * \brief TextDoc::load loads a text document
 * \return true/false if the document was opened with success
 */
bool TextDoc::load ()
{
  QFile file (a_DocName);
  if (!file.open (QIODevice::ReadOnly))
    return false;
  setLanguage (a_DocName);

  QTextStream stream (&file);
  a_countsEdits = false;
  insertPlainText(stream.readAll());
  // Store timestamp
  QFileInfo fileInfo(a_DocName);
  lastLoadModTime = fileInfo.lastModified();
  document()->setModified(false);
  slotSetChanged ();
  file.close ();
  a_lastSaved = QDateTime::currentDateTime ();
  loadSettings ();
  a_SimOpenDpl = simulation ? true : false;
  refreshLanguage();
  // Its content: the first, or, read again, an edit.
  a_textRevision = document()->revision();
  a_countsEdits = true;
  edited();
  return true;
}

/*!
 * \brief TextDoc::clears and re-loads a text document
 * \return true/false if the document was opened with success
 */
bool TextDoc::reload()
{
  clear();
  return load();
}


/*!
 * \brief TextDoc::save saves the current document and it settings
 * \return true/false if the document was opened with success
 */
bool TextDoc::writeTo(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  QTextStream stream(&file);
  stream << toPlainText();
  return true;
}

int TextDoc::save ()
{
  saveSettings ();

  QFile file (a_DocName);
  if (!file.open (QIODevice::WriteOnly))
    return -1;
  setLanguage (a_DocName);

  QTextStream stream (&file);
  stream << toPlainText();
  document()->setModified (false);
  slotSetChanged ();
  file.close ();

  QFileInfo Info (a_DocName);
  a_lastSaved = Info.lastModified ();

  /// clear highlighted lines on save \see MessageDock::slotCursor()
  QList<QTextEdit::ExtraSelection> extraSelections;
  this->setExtraSelections(extraSelections);
  refreshLanguage();

  return 0;
}

/*!
 * \brief Zooms the document in and out. Note, the zoom amount is fixed by Qt and the
 *        amount passed is ignored.
 */
double TextDoc::zoomBy(double zoom)
{
  // qucs_actions defines zooming in as > 1.
  if (zoom > 1.0) {
    zoomIn();
  }

  else {
    zoomOut();
  }

  return zoom;
}

/*!
 * \brief Resets the font scaling to default.
 */
void TextDoc::showNoZoom()
{
  // Get a copy of this editor's existing font and apply the default font size to it.
  QFont tempFont = font();
  tempFont.setPointSize(QucsSettings.font.pointSize());
  setFont(tempFont);
}

/*!
 * \brief TextDoc::loadSimulationTime set a_SimTime member variable
 * \param Time string  with simulation time
 * \return true if a_SimTime is set
 */
bool TextDoc::loadSimulationTime(QString& Time)
{
  if(!a_SimTime.isEmpty()) {
    Time = a_SimTime;
    return true;
  }
  return false;
}

/*!
 * \brief TextDoc::commentSelected toggles the comment of selected text
 * See also QucsApp::slotEditActivate
 */
void TextDoc::commentSelected ()
{
  QTextCursor cursor = this->textCursor();

  if(!cursor.hasSelection())
      return; // No selection available

  // get range of selection
  int start = cursor.selectionStart();
  int end = cursor.selectionEnd();

  cursor.setPosition(start);
  int firstLine = cursor.blockNumber();
  cursor.setPosition(end, QTextCursor::KeepAnchor);
  int lastLine = cursor.blockNumber();

  // use comment string indicator depending on language
  QString co;

  switch (language) {
  case LANG_VHDL:
    co = "--";
    break;
  case LANG_VERILOG:
  case LANG_VERILOGA:
  case LANG_CPP:
    co = "//";
    break;
  case LANG_OCTAVE:
    co = "%";
    break;
  case LANG_PYTHON:
  case LANG_SHELL:
  case LANG_QUCS_NETLIST:
    co = "#";
    break;
  case LANG_SPICE:
    co = "*";
    break;
  default:
    co = "";
    break;
  }

  QStringList newlines;
  for (int i=firstLine; i<=lastLine; i++) {
      QString line = document()->findBlockByLineNumber(i).text();
      if (line.startsWith(co)){
          // uncomment
          line.remove(0,co.length());
          newlines << line;
      }
      else {
          // comment
          line = line.insert(0, co);
          newlines << line;
      }
  }
  insertPlainText(newlines.join("\n"));
}

/*!
 * \brief TextDoc::insertSkeleton adds a basic skeleton for type of text file
 */
void TextDoc::insertSkeleton ()
{
  if (language == LANG_VHDL)
    appendPlainText("entity  is\n  port ( : in bit);\nend;\n"
      "architecture  of  is\n  signal : bit;\nbegin\n\nend;\n\n");
  else if (language == LANG_VERILOG)
    appendPlainText ("module  ( );\ninput ;\noutput ;\nbegin\n\nend\n"
      "endmodule\n\n");
  else if (language == LANG_OCTAVE)
    appendPlainText ("function  =  ( )\n"
      "endfunction\n\n");
}

/*!
 * \brief TextDoc::getModuleName parse the module name ou of the text file contents
 * \return the module name
 */
QString TextDoc::getModuleName (void)
{
  switch (language) {
  case LANG_VHDL:
    {
      VHDL_File_Info VInfo (toPlainText());
      return VInfo.EntityName;
    }
  case LANG_VERILOG:
    {
      Verilog_File_Info VInfo (toPlainText());
      return VInfo.ModuleName;
    }
  case LANG_VERILOGA:
    {
      VerilogA_File_Info VInfo (toPlainText());
      return VInfo.ModuleName;
    }
  case LANG_OCTAVE:
    {
      QFileInfo Info (a_DocName);
      return Info.baseName ();
    }
  default:
    return "";
  }
}

/*!
 * \brief Handle mouse wheel events
 *        Used to 'zoom' i.e., increase / reduce the font size.
 */
void TextDoc::wheelEvent(QWheelEvent* event)
{
  if (event->modifiers() & Qt::CTRL) {
    if (event->angleDelta().y() > 0) {
      zoomIn();
    }

    else {
      zoomOut();
    }
  }

  else {
    QPlainTextEdit::wheelEvent(event);
  }
}

void TextDoc::dragEnterEvent(QDragEnterEvent* event)
{
  if (misc::localFiles(event->mimeData()).isEmpty()) {
    QPlainTextEdit::dragEnterEvent(event);
    return;
  }
  event->setDropAction(Qt::CopyAction);
  event->accept();
}

void TextDoc::dragMoveEvent(QDragMoveEvent* event)
{
  if (misc::localFiles(event->mimeData()).isEmpty()) {
    QPlainTextEdit::dragMoveEvent(event);
    return;
  }
  event->setDropAction(Qt::CopyAction);
  event->accept();
}

void TextDoc::dropEvent(QDropEvent* event)
{
  const QStringList files = misc::localFiles(event->mimeData());
  if (files.isEmpty() || a_App == nullptr) {
    QPlainTextEdit::dropEvent(event);
    return;
  }
  event->setDropAction(Qt::CopyAction);
  event->accept();
  a_App->openDroppedFiles(files, this);
}

/*!
 * \brief TextDoc::highlightCurrentLine mark the current line
 */
void TextDoc::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> extraSelections;

    if (!isReadOnly()) {
        QTextEdit::ExtraSelection selection;

        selection.format.setBackground(a_currentLine);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        extraSelections.append(selection);
    }

    setExtraSelections(extraSelections);
}

/*!
 * \brief The editor's colours. Under the platform's themes black text on
 * white, in the light and the dark theme alike (the syntax colours are
 * meant for white), and independent of the schematic's document
 * background (which earlier versions meant to use here but never showed:
 * see below). Under a designed theme its base and text colours, the
 * syntax colours fitted to them (ink::on()). As a style sheet, not a
 * palette: the main window has a style sheet, and Qt's style-sheet style
 * puts the application palette back on every widget it polishes (each
 * time the editor is shown), which took a palette set here away and made
 * the editor follow the theme's base colour.
 */
std::pair<QColor, QColor> TextDoc::paperAndInk()
{
  if (const auto *theme = qucs_s::apptheme::designedTheme(qucs_s::apptheme::current()))
    return {theme->colours.base, theme->colours.text};
  return {QColor(Qt::white), QColor(Qt::black)};
}

void TextDoc::applyDocumentColors()
{
  const auto [paper, text] = paperAndInk();
  a_currentLine = QColor(Qt::blue).lighter(195);
  a_margin = Qt::lightGray;
  a_marginText = Qt::black;
  if (const auto *theme = qucs_s::apptheme::designedTheme(qucs_s::apptheme::current())) {
    const qucs_s::apptheme::Colours &c = theme->colours;
    a_currentLine = qucs_s::apptheme::mix(c.base, c.accent, theme->dark ? 0.16 : 0.10);
    a_margin = c.surface;
    a_marginText = c.muted;
  }
  a_paper = paper;
  setStyleSheet(QStringLiteral("QPlainTextEdit { background-color: %1; color: %2; }")
                    .arg(paper.name(QColor::HexRgb), text.name(QColor::HexRgb)));
  if (syntaxHighlight != nullptr) syntaxHighlight->setPaper(paper);
  highlightCurrentLine();
  if (lineNumberArea != nullptr) lineNumberArea->update();
}

void TextDoc::refreshLanguage()
{
    this->setLanguage(a_DocName);
    syntaxHighlight->setLanguage(language);   // highlighted again, in the formats of the settings
}

// Returns true if file on disk has a lastModified timestamp newer than the object's
// last load modified time
bool TextDoc::hasFileChangedOnDisk() const
{
  QFileInfo fileInfo(a_DocName);
  if (!fileInfo.exists()) {
    return true; // File is removed -> has changed
  }
  return fileInfo.lastModified() > lastLoadModTime;
}


int TextDoc::lineNumberAreaWidth() const
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }

    int space = 3 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    return space;
}

void TextDoc::updateLineNumberAreaWidth(int /* newBlockCount */)
{
    const QMargins extra = extraMargins();
    setViewportMargins(lineNumberAreaWidth() + extra.left(), extra.top(), extra.right(), extra.bottom());
}

void TextDoc::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy){
        lineNumberArea->scroll(0, dy);
    } else {
        lineNumberArea->update(0, rect.y(), lineNumberArea->width(), rect.height());
    }

    if (rect.contains(viewport()->rect())) {
        updateLineNumberAreaWidth(0);
    }
}

void TextDoc::resizeEvent(QResizeEvent *e)
{
    QPlainTextEdit::resizeEvent(e);

    const QMargins extra = extraMargins();
    QRect cr = contentsRect();
    lineNumberArea->setGeometry(QRect(cr.left(), cr.top() + extra.top(),
        lineNumberAreaWidth(), cr.height() - extra.top() - extra.bottom()));
}

void TextDoc::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(lineNumberArea);
    painter.fillRect(event->rect(), a_margin);

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            QString number = QString::number(blockNumber + 1);
            painter.setPen(a_marginText);
            painter.drawText(0, top, lineNumberArea->width(), fontMetrics().height(),
                Qt::AlignRight, number);
        }

        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}
