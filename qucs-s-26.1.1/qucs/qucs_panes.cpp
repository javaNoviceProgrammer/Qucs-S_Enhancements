/*
 * qucs_panes.cpp - editor panes: documents side by side, up to 2x2
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qucs.h"
#include "findbar.h"
#include "ink.h"
#include "main.h"
#include "schematic.h"
#include "textdoc.h"
#include "misc.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QSplitter>
#include <QTabBar>
#include <QVBoxLayout>

/*!
 * \brief A pane's frame: the tab widget with a thin bar above it that is
 *        coloured while the pane is the active one (and hidden while
 *        there is only one pane, when it would say nothing).
 */
class PaneWidget : public QWidget
{
public:
  explicit PaneWidget(ContextMenuTabWidget *tabs)
      : QWidget(), a_tabs(tabs), a_marker(new QFrame(this)),
        a_findBar(new FindBar(tabs, this))
  {
    a_marker->setFixedHeight(3);
    a_marker->setAutoFillBackground(true);
    a_marker->setFrameStyle(QFrame::NoFrame);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(a_marker);
    layout->addWidget(a_tabs, 1);
    layout->addWidget(a_findBar);   // hidden until Edit > Find
    setActive(false);
  }
  ContextMenuTabWidget *tabs() const { return a_tabs; }
  FindBar *findBar() const { return a_findBar; }
  void setActive(bool on)
  {
    QPalette pal = a_marker->palette();
    pal.setColor(QPalette::Window, on ? palette().color(QPalette::Highlight)
                                      : palette().color(QPalette::Window));
    a_marker->setPalette(pal);
  }
  void setMarkerShown(bool on) { a_marker->setVisible(on); }

private:
  ContextMenuTabWidget *a_tabs;
  QFrame *a_marker;
  FindBar *a_findBar;
};

// ---------------------------------------------------------------------
void QucsApp::initPaneArea()
{
  a_paneArea = new QSplitter(Qt::Vertical, this);
  a_paneArea->setChildrenCollapsible(false);
  auto *row = new QSplitter(Qt::Horizontal);
  row->setChildrenCollapsible(false);
  a_paneArea->addWidget(row);
  DocumentTab = createPane();
  row->addWidget(frameOf(DocumentTab));
  frameOf(DocumentTab)->setActive(true);
  frameOf(DocumentTab)->setMarkerShown(false);
  setCentralWidget(a_paneArea);
  connect(qApp, &QApplication::focusChanged, this, &QucsApp::slotFocusChanged);
}

ContextMenuTabWidget *QucsApp::createPane()
{
  auto *tabs = new ContextMenuTabWidget(this);
#if __APPLE__
  tabs->setDocumentMode(true);
#endif
  // The UI follows the current document - of the active pane.
  connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int) {
    if (tabs == DocumentTab) slotChangeView();
  });
  // Every tab has a close button; the pane it is in becomes active first,
  // as closeFile() works on the active pane.
  tabs->setTabsClosable(true);
  connect(tabs, &QTabWidget::tabCloseRequested, this, [this, tabs](int index) {
    setActivePane(tabs);
    slotFileClose(index);
  });
  tabs->setMovable(true);
  tabs->tabBar()->installEventFilter(this);   // a click on the tabs activates the pane
  new PaneWidget(tabs);
  return tabs;
}

QList<ContextMenuTabWidget *> QucsApp::panes() const
{
  QList<ContextMenuTabWidget *> result;
  if (a_paneArea == nullptr) {
    if (DocumentTab != nullptr) result << DocumentTab;
    return result;
  }
  for (int r = 0; r < a_paneArea->count(); ++r) {
    auto *row = qobject_cast<QSplitter *>(a_paneArea->widget(r));
    if (row == nullptr) continue;
    for (int c = 0; c < row->count(); ++c)
      if (auto *frame = dynamic_cast<PaneWidget *>(row->widget(c)))
        result << frame->tabs();
  }
  return result;
}

QSplitter *QucsApp::rowOf(ContextMenuTabWidget *pane) const
{
  PaneWidget *frame = frameOf(pane);
  return frame != nullptr ? qobject_cast<QSplitter *>(frame->parentWidget()) : nullptr;
}

PaneWidget *QucsApp::frameOf(ContextMenuTabWidget *pane) const
{
  return pane != nullptr ? dynamic_cast<PaneWidget *>(pane->parentWidget()) : nullptr;
}

ContextMenuTabWidget *QucsApp::paneOf(QWidget *document) const
{
  if (document == nullptr) return nullptr;
  for (ContextMenuTabWidget *pane : panes())
    if (pane->indexOf(document) >= 0) return pane;
  return nullptr;
}

QWidget *QucsApp::documentWidget(QucsDoc *doc)
{
  if (doc == nullptr) return nullptr;
  if (auto *sch = dynamic_cast<Schematic *>(doc)) return sch;
  if (auto *text = dynamic_cast<TextDoc *>(doc)) return text;
  return nullptr;
}

QList<QucsDoc *> QucsApp::allDocuments() const
{
  QList<QucsDoc *> docs;
  for (ContextMenuTabWidget *pane : panes())
    for (int i = 0; i < pane->count(); ++i) {
      QWidget *w = pane->widget(i);
      if (isTextDocument(w))
        docs << static_cast<QucsDoc *>(static_cast<TextDoc *>(w));
      else
        docs << static_cast<QucsDoc *>(static_cast<Schematic *>(w));
    }
  return docs;
}

void QucsApp::setActivePane(ContextMenuTabWidget *pane)
{
  if (pane == nullptr || pane == DocumentTab) return;
  ContextMenuTabWidget *old = DocumentTab;
  DocumentTab = pane;
  if (PaneWidget *frame = frameOf(old)) frame->setActive(false);
  if (PaneWidget *frame = frameOf(pane)) frame->setActive(true);
  slotChangeView();
  updatePaneActions();
  // The keyboard focus comes along: were it left in the old pane, the
  // next focus event (a menu closing, say) would hand the activity back.
  if (QWidget *w = pane->currentWidget()) {
    if (!w->isAncestorOf(QApplication::focusWidget()) && w != QApplication::focusWidget())
      w->setFocus(Qt::OtherFocusReason);
  }
}

void QucsApp::activatePaneOf(QWidget *widget)
{
  for (QWidget *w = widget; w != nullptr; w = w->parentWidget()) {
    if (auto *pane = qobject_cast<ContextMenuTabWidget *>(w)) {
      if (panes().contains(pane)) setActivePane(pane);
      return;
    }
  }
}

void QucsApp::showDocument(QWidget *document)
{
  if (document == nullptr || paneOf(document) == nullptr) return;
  if (DocumentTab->indexOf(document) < 0) activatePaneOf(document);
  if (DocumentTab->currentWidget() != document) {
    DocumentTab->setCurrentWidget(document);
    slotChangeView();
  }
}

void QucsApp::applyPaper()
{
  const QColor paper = misc::paperColor();
  for (QucsDoc *doc : allDocuments())
    if (auto *sch = qobject_cast<Schematic *>(documentWidget(doc))) {
      misc::setWidgetBackgroundColor(sch->viewport(), paper);
      sch->viewport()->update();
    }
  if (editText != nullptr) {
    QPalette p = editText->palette();
    p.setColor(editText->backgroundRole(), paper);
    p.setColor(editText->foregroundRole(),
               qucs_s::ink::isDark(paper) ? QColor(235, 235, 235) : QColor(Qt::black));
    editText->setPalette(p);
  }
}

void QucsApp::applyGridSetting()
{
  for (QucsDoc *doc : allDocuments())
    if (auto *sch = qobject_cast<Schematic *>(documentWidget(doc)))
      sch->viewport()->update();
  updateGridAction();
}

void QucsApp::updateGridAction()
{
  if (showGrid == nullptr) return;
  if (QucsSettings.GridMode != 0) {
    showGrid->setText(tr("Show Grid (all schematics)"));
    showGrid->setStatusTip(tr("Show or hide the grid of every schematic (Application Settings > Appearance)."));
    showGrid->setChecked(QucsSettings.GridMode == 2);
    return;
  }
  showGrid->setText(tr("Show Grid (current document)"));
  showGrid->setStatusTip(tr("Show or hide the grid for the current document."));
  const Schematic *doc = currentSchematic();
  showGrid->setChecked(doc != nullptr && doc->getGridOn());
}

FindBar *QucsApp::findBarOf(ContextMenuTabWidget *pane) const
{
  PaneWidget *frame = frameOf(pane);
  return frame != nullptr ? frame->findBar() : nullptr;
}

void QucsApp::slotFocusChanged(QWidget *, QWidget *now)
{
  // The keyboard focus landing in a document (or on a pane's tabs) makes
  // that pane the active one.
  if (now != nullptr && a_paneArea != nullptr && a_paneArea->isAncestorOf(now))
    activatePaneOf(now);
}

bool QucsApp::eventFilter(QObject *watched, QEvent *event)
{
  if (event->type() == QEvent::MouseButtonPress) {
    if (auto *bar = qobject_cast<QTabBar *>(watched)) activatePaneOf(bar);
  }
  return QMainWindow::eventFilter(watched, event);
}

bool QucsApp::canSplitRight() const
{
  QSplitter *row = rowOf(DocumentTab);
  return row != nullptr && row->count() < 2;
}

bool QucsApp::canSplitDown() const
{
  return a_paneArea != nullptr && a_paneArea->count() < 2;
}

void QucsApp::updatePaneActions()
{
  if (splitPaneRight == nullptr) return;   // before the actions exist
  const int n = panes().size();
  splitPaneRight->setEnabled(canSplitRight());
  splitPaneDown->setEnabled(canSplitDown());
  closePaneAction->setEnabled(n > 1);
  nextPaneAction->setEnabled(n > 1);
  moveDocumentToNextPane->setEnabled(n > 1 || canSplitRight() || canSplitDown());
  for (ContextMenuTabWidget *pane : panes())
    if (PaneWidget *frame = frameOf(pane)) frame->setMarkerShown(n > 1);
}

// A new pane starts, as the application does, with an untitled schematic,
// which gotoPage() closes again once a file opens in it.
void QucsApp::startPaneWithUntitled(ContextMenuTabWidget *pane)
{
  auto *d = new Schematic(this, "");
  setActivePane(pane);
  addDocumentTab(d);
  pane->setCurrentIndex(0);
}

void QucsApp::slotSplitPaneRight()
{
  if (!canSplitRight()) return;
  QSplitter *row = rowOf(DocumentTab);
  ContextMenuTabWidget *pane = createPane();
  const int at = row->indexOf(frameOf(DocumentTab)) + 1;
  row->insertWidget(at, frameOf(pane));
  const int half = qMax(1, row->width() / 2);
  row->setSizes({half, half});
  startPaneWithUntitled(pane);
  updatePaneActions();
}

void QucsApp::slotSplitPaneDown()
{
  if (!canSplitDown()) return;
  auto *row = new QSplitter(Qt::Horizontal);
  row->setChildrenCollapsible(false);
  a_paneArea->addWidget(row);
  ContextMenuTabWidget *pane = createPane();
  row->addWidget(frameOf(pane));
  const int half = qMax(1, a_paneArea->height() / 2);
  a_paneArea->setSizes({half, half});
  startPaneWithUntitled(pane);
  updatePaneActions();
}

void QucsApp::removePane(ContextMenuTabWidget *pane)
{
  const QList<ContextMenuTabWidget *> all = panes();
  if (all.size() < 2 || !all.contains(pane) || pane->count() > 0) return;
  const int at = all.indexOf(pane);
  ContextMenuTabWidget *neighbour = at > 0 ? all.at(at - 1) : all.at(1);
  QSplitter *row = rowOf(pane);
  PaneWidget *frame = frameOf(pane);
  frame->setParent(nullptr);     // out of the splitter, and of panes()
  frame->deleteLater();
  if (row != nullptr && row->count() == 0) {
    row->setParent(nullptr);
    row->deleteLater();
  }
  if (DocumentTab == pane) {
    DocumentTab = nullptr;
    setActivePane(neighbour);
  }
  updatePaneActions();
}

void QucsApp::slotClosePane()
{
  const QList<ContextMenuTabWidget *> all = panes();
  if (all.size() < 2) return;
  ContextMenuTabWidget *pane = DocumentTab;
  const int at = all.indexOf(pane);
  ContextMenuTabWidget *neighbour = at > 0 ? all.at(at - 1) : all.at(1);
  // An untitled, unchanged document was only the pane's placeholder.
  while (pane->count() > 0) {
    QWidget *w = pane->widget(0);
    QucsDoc *doc = getDoc(0);
    if (doc != nullptr && doc->getDocName().isEmpty() && !doc->getDocChanged()) {
      pane->removeTab(0);
      delete doc;
      continue;
    }
    moveDocument(w, neighbour);   // removes the pane once it is empty
  }
  removePane(pane);               // in case only placeholders were in it
  if (neighbour->count() == 0) startPaneWithUntitled(neighbour);
  setActivePane(neighbour);
}

void QucsApp::moveDocument(QWidget *document, ContextMenuTabWidget *to)
{
  ContextMenuTabWidget *from = paneOf(document);
  if (from == nullptr || to == nullptr || from == to) return;
  const int index = from->indexOf(document);
  const QString title = from->tabText(index);
  QucsDoc *doc = isTextDocument(document) ? static_cast<QucsDoc *>(static_cast<TextDoc *>(document))
                                          : static_cast<QucsDoc *>(static_cast<Schematic *>(document));
  from->removeTab(index);
  const int at = addDocumentTabTo(to, static_cast<QFrame *>(document), title);
  to->setCurrentIndex(at);
  setDocumentChanged(document, doc->getDocChanged());
  dropPlaceholder(to, document);
  // The pane it left is empty: it goes, unless it is the only other one.
  if (from->count() == 0 && panes().size() > 1) removePane(from);
  setActivePane(to);
}

// An untitled, unchanged document is a pane's placeholder: once a real
// document is in the pane (as gotoPage() does when a file opens), it goes.
void QucsApp::dropPlaceholder(ContextMenuTabWidget *pane, QWidget *keep)
{
  for (int i = pane->count() - 1; i >= 0; --i) {
    QWidget *w = pane->widget(i);
    if (w == keep) continue;
    QucsDoc *doc = isTextDocument(w) ? static_cast<QucsDoc *>(static_cast<TextDoc *>(w))
                                     : static_cast<QucsDoc *>(static_cast<Schematic *>(w));
    if (doc->getDocName().isEmpty() && !doc->getDocChanged() && pane->count() > 1) {
      pane->removeTab(i);
      delete doc;
    }
  }
}

void QucsApp::slotMoveDocumentToNextPane()
{
  QWidget *document = DocumentTab->currentWidget();
  if (document == nullptr) return;
  QList<ContextMenuTabWidget *> all = panes();
  if (all.size() < 2) {
    if (canSplitRight())
      slotSplitPaneRight();
    else if (canSplitDown())
      slotSplitPaneDown();
    else
      return;
    // The new pane is active now, with a placeholder, which the document
    // replaces.
    moveDocument(document, DocumentTab);
    return;
  }
  const int at = all.indexOf(DocumentTab);
  moveDocument(document, all.at((at + 1) % all.size()));
}

void QucsApp::slotNextPane()
{
  const QList<ContextMenuTabWidget *> all = panes();
  if (all.size() < 2) return;
  ContextMenuTabWidget *next = all.at((all.indexOf(DocumentTab) + 1) % all.size());
  setActivePane(next);
  if (QWidget *w = next->currentWidget()) w->setFocus();
}
