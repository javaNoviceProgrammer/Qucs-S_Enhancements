/*
 * claudecodetabs.cpp - the Claude Code dock's conversations, a tab each
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "claudecodetabs.h"

#include "apptheme.h"
#include "claudecodepanel.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QDockWidget>
#include <QHeaderView>
#include <QJsonArray>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QUuid>
#include <QEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

using qucs_s::claude::Session;
using qucs_s::claude::State;

ClaudeCodeTabs::ClaudeCodeTabs(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("claudeCodeTabs"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    a_tabs = new QTabWidget(this);
    a_tabs->setObjectName(QStringLiteral("claudeTabs"));
    a_tabs->setDocumentMode(true);
    a_tabs->setMovable(true);
    a_tabs->setUsesScrollButtons(true);
    a_tabs->setElideMode(Qt::ElideRight);
    a_tabs->tabBar()->setObjectName(QStringLiteral("claudeTabBar"));
    a_tabs->tabBar()->setExpanding(false);
    a_tabs->tabBar()->setIconSize(QSize(10, 10));
    a_tabs->tabBar()->setDrawBase(false);
    a_plus = new QToolButton(a_tabs);
    a_plus->setObjectName(QStringLiteral("claudeNewTab"));
    a_plus->setText(QStringLiteral("+"));
    a_plus->setAutoRaise(true);
    a_plus->setToolTip(tr("A new conversation, in a tab of its own"));
    a_tabs->setCornerWidget(a_plus, Qt::TopRightCorner);
    layout->addWidget(a_tabs);
    connect(a_plus, &QToolButton::clicked, this, [this] { newConversation(); });
    connect(a_tabs, &QTabWidget::currentChanged, this, [this] {
        finishRenaming(true, false);
        emit stateChanged();
    });
    connect(a_tabs->tabBar(), &QTabBar::tabMoved, this, [this] {
        finishRenaming(true, false);
        emit stateChanged();
    });
    // A right click on a tab: its menu; a double click: its name.
    a_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(a_tabs->tabBar(), &QWidget::customContextMenuRequested, this, [this](QPoint pos) {
        QTabBar* bar = a_tabs->tabBar();
        const int index = bar->tabAt(pos);
        if (index < 0) return;
        QMenu* menu = tabMenu(index);
        menu->exec(bar->mapToGlobal(pos));
        delete menu;
    });
    connect(a_tabs, &QTabWidget::tabBarDoubleClicked, this, [this](int index) {
        if (auto* panel = qobject_cast<ClaudeCodePanel*>(a_tabs->widget(index))) renameConversation(panel);
    });
    a_tabs->tabBar()->installEventFilter(this);
    // Kept a moment after they change (a reply streams in pieces).
    a_saveTimer = new QTimer(this);
    a_saveTimer->setSingleShot(true);
    a_saveTimer->setInterval(700);
    connect(a_saveTimer, &QTimer::timeout, this, &ClaudeCodeTabs::saveConversations);
    connect(a_tabs, &QTabWidget::currentChanged, a_saveTimer, qOverload<>(&QTimer::start));
    connect(a_tabs->tabBar(), &QTabBar::tabMoved, a_saveTimer, qOverload<>(&QTimer::start));
    addPanel(nullptr);
    restyle();
}

ClaudeCodeTabs::~ClaudeCodeTabs()
{
    saveConversations();   // (as Qucs-S closes: they come back as they are)
    delete a_renameEditor;
    // The panels' sessions end with them; nothing of theirs comes here then.
    for (ClaudeCodePanel* panel : panels()) {
        panel->disconnect(this);
        panel->session()->disconnect(this);
    }
}

ClaudeCodePanel* ClaudeCodeTabs::current() const
{
    return qobject_cast<ClaudeCodePanel*>(a_tabs->currentWidget());
}

QList<ClaudeCodePanel*> ClaudeCodeTabs::panels() const
{
    QList<ClaudeCodePanel*> all;
    for (int i = 0; i < a_tabs->count(); ++i)
        if (auto* panel = qobject_cast<ClaudeCodePanel*>(a_tabs->widget(i))) all << panel;
    return all;
}

int ClaudeCodeTabs::count() const
{
    return a_tabs->count();
}

ClaudeCodePanel* ClaudeCodeTabs::addPanel(ClaudeCodePanel* like)
{
    auto* panel = new ClaudeCodePanel;
    panel->setNewInTab(true);
    if (!a_defaultDir.isEmpty()) panel->setDefaultDirectory(a_defaultDir);
    // In the folder of the conversation it was opened from.
    if (like != nullptr && like->workingDirectory() != like->defaultDirectory())
        panel->setWorkingDirectory(like->workingDirectory());
    if (a_document) panel->setDocumentProvider(a_document);
    if (a_schematics) panel->setSchematicsProvider(a_schematics);
    panel->session()->setToolHost(a_host);

    connect(panel, &ClaudeCodePanel::newConversationRequested, this, [this] { newConversation(); });
    connect(panel, &ClaudeCodePanel::titleChanged, this, [this, panel] {
        updateTab(panel);
        emit stateChanged();
    });
    connect(panel, &ClaudeCodePanel::openFileRequested, this, &ClaudeCodeTabs::openFileRequested);
    connect(panel, &ClaudeCodePanel::pinChanged, this, [this, panel] {
        updateTab(panel);
        emit stateChanged();
    });
    connect(panel, &ClaudeCodePanel::conversationChanged, this, [this, panel] {
        a_unsaved.insert(panel);
        a_saveTimer->start();
    });
    connect(panel, &ClaudeCodePanel::conversationEnding, this, [this, panel] {
        if (a_unsaved.contains(panel)) saveConversation(panel);
    });
    // (From the event loop: not under the panel's own sending.)
    connect(panel, &ClaudeCodePanel::closeRequested, this, [this, panel] { closeConversation(panel); }, Qt::QueuedConnection);
    connect(panel, &ClaudeCodePanel::resumeRequested, this,
            [this, panel](const QString& query) { showResume(panel, query); }, Qt::QueuedConnection);
    connect(panel, &ClaudeCodePanel::filesChanged, this, [this, panel](const QStringList& files) {
        a_reporting = panel;
        emit filesChanged(files);
        a_reporting = nullptr;
    });
    // (After the panel's own: its mark is up to date.)
    connect(panel->session(), &Session::stateChanged, this, [this, panel] {
        updateTab(panel);
        emit stateChanged();
    });
    connect(panel->session(), &Session::permissionRequested, this, [this, panel] { permissionAsked(panel); });

    const int index = a_tabs->addTab(panel, QString());
    auto* close = new QToolButton;
    close->setObjectName(QStringLiteral("claudeCloseTab"));
    close->setText(QStringLiteral("×"));
    close->setAutoRaise(true);
    close->setToolTip(tr("Close this conversation"));
    connect(close, &QToolButton::clicked, this, [this, panel] { closeConversation(panel); });
    a_tabs->tabBar()->setTabButton(index, QTabBar::RightSide, close);
    updateTab(panel);
    return panel;
}

ClaudeCodePanel* ClaudeCodeTabs::newConversation()
{
    ClaudeCodePanel* panel = addPanel(current());
    a_tabs->setCurrentWidget(panel);
    panel->focusComposer();
    emit stateChanged();
    return panel;
}

bool ClaudeCodeTabs::closeConversation(ClaudeCodePanel* panel, bool ask)
{
    const int index = a_tabs->indexOf(panel);
    if (index < 0) return false;
    if (ask && panel->session()->isBusy()
        && QMessageBox::question(this, tr("Claude Code"),
                                 tr("Claude is at work in “%1”. Stop it and close the conversation?").arg(panel->title()))
               != QMessageBox::Yes)
        return false;
    if (panel == a_renaming) finishRenaming(false, false);
    if (a_unsaved.contains(panel)) saveConversation(panel);   // (kept: /resume brings it back)
    a_unsaved.remove(panel);
    panel->disconnect(this);
    panel->session()->disconnect(this);
    a_tabs->removeTab(index);
    panel->session()->stop();   // its input closed: the program keeps the conversation and ends
    panel->hide();
    panel->deleteLater();
    if (a_tabs->count() == 0) newConversation();
    saveOpen();
    emit stateChanged();
    return true;
}

// ----------------------------------------------------------------------
// Kept, and gone on with.

void ClaudeCodeTabs::saveConversation(ClaudeCodePanel* panel)
{
    a_unsaved.remove(panel);
    if (!panel->hasConversation()) return;
    if (panel->conversationId().isEmpty()) panel->setConversationId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    qucs_s::claude::history::save(panel->conversationId(), panel->conversationJson());
}

void ClaudeCodeTabs::saveOpen()
{
    QStringList ids;
    for (ClaudeCodePanel* panel : panels())
        if (!panel->conversationId().isEmpty() && panel->hasConversation()) ids << panel->conversationId();
    ClaudeCodePanel* front = current();
    qucs_s::claude::history::setOpen(ids, front != nullptr ? front->conversationId() : QString());
}

void ClaudeCodeTabs::saveConversations()
{
    for (ClaudeCodePanel* panel : panels())
        if (a_unsaved.contains(panel)) saveConversation(panel);
    a_unsaved.clear();
    saveOpen();
}

bool ClaudeCodeTabs::restoreConversations()
{
    if (!qucs_s::claude::history::reopenAtStart()) return false;
    QString front;
    const QStringList ids = qucs_s::claude::history::open(&front);
    ClaudeCodePanel* shown = nullptr;
    bool any = false;
    for (const QString& id : ids) {
        const QJsonObject conversation = qucs_s::claude::history::load(id);
        if (conversation.value(QLatin1String("entries")).toArray().isEmpty()) continue;
        // The empty one there at first takes the first.
        ClaudeCodePanel* panel = !any && count() == 1 && !current()->hasConversation() ? current() : addPanel(nullptr);
        panel->setConversationId(id);
        panel->restoreConversation(conversation);
        updateTab(panel);
        if (id == front) shown = panel;
        any = true;
    }
    if (shown != nullptr) showConversation(shown);
    a_unsaved.clear();   // (as they were kept)
    emit stateChanged();
    return any;
}

QList<qucs_s::claude::history::Summary> ClaudeCodeTabs::resumable(const QString& folder) const
{
    using namespace qucs_s::claude;
    QList<history::Summary> list = history::saved();
    QSet<QString> sessions;
    for (const history::Summary& s : std::as_const(list))
        if (!s.sessionId.isEmpty()) sessions << s.sessionId;
    for (ClaudeCodePanel* panel : panels())
        if (!panel->session()->sessionId().isEmpty()) sessions << panel->session()->sessionId();
    for (const history::Summary& s : history::claudeSessions(folder))
        if (!sessions.contains(s.sessionId)) list << s;
    return list;
}

ClaudeCodePanel* ClaudeCodeTabs::resume(const qucs_s::claude::history::Summary& conversation)
{
    using namespace qucs_s::claude;
    // Open already: in front.
    for (ClaudeCodePanel* panel : panels())
        if ((conversation.claudeFile.isEmpty() && panel->conversationId() == conversation.id)
            || (!conversation.sessionId.isEmpty() && panel->session()->sessionId() == conversation.sessionId)) {
            showConversation(panel);
            return panel;
        }
    QJsonObject kept;
    if (conversation.claudeFile.isEmpty()) {
        kept = history::load(conversation.id);
        if (kept.isEmpty()) return nullptr;
    } else if (!QFileInfo::exists(conversation.claudeFile)) {
        return nullptr;
    }
    ClaudeCodePanel* front = current();
    ClaudeCodePanel* panel = front != nullptr && !front->hasConversation() && !front->session()->isBusy() ? front : newConversation();
    if (conversation.claudeFile.isEmpty()) {
        panel->setConversationId(conversation.id);
        panel->restoreConversation(kept);
    } else if (!panel->importClaudeSession(conversation.claudeFile)) {
        // Nothing of it to show: its session gone on with all the same.
        const history::Summary what = history::claudeSessionSummary(conversation.claudeFile);
        if (!what.folder.isEmpty() && QFileInfo(what.folder).isDir()) panel->setWorkingDirectory(what.folder);
        panel->session()->resume(what.sessionId);
        panel->addNote(tr("Going on with Claude Code's session %1 (what was said in it is not shown here).").arg(what.sessionId));
    }
    showConversation(panel);
    updateTab(panel);
    a_unsaved.insert(panel);
    a_saveTimer->start();
    emit stateChanged();
    return panel;
}

namespace {

// When a conversation was last changed, as the list says it.
QString whenOf(const QDateTime& time)
{
    const QDateTime local = time.toLocalTime();
    const qint64 days = local.date().daysTo(QDate::currentDate());
    if (days == 0) return QLocale().toString(local.time(), QLocale::ShortFormat);
    if (days == 1) return ClaudeCodeTabs::tr("Yesterday");
    return QLocale().toString(local.date(), QLocale::ShortFormat);
}

} // namespace

QDialog* ClaudeCodeTabs::resumeDialog(const QString& folder, const QString& query)
{
    using namespace qucs_s::claude;
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("claudeResumeDialog"));
    dialog->setWindowTitle(tr("Resume a Conversation"));
    dialog->resize(680, 420);
    auto* layout = new QVBoxLayout(dialog);
    auto* filter = new QLineEdit(dialog);
    filter->setObjectName(QStringLiteral("claudeResumeFilter"));
    filter->setPlaceholderText(tr("Search the conversations"));
    filter->setClearButtonEnabled(true);
    auto* list = new QTreeWidget(dialog);
    list->setObjectName(QStringLiteral("claudeResumeList"));
    list->setHeaderLabels({tr("Conversation"), tr("Folder"), tr("When"), tr("From")});
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    const QList<history::Summary> all = resumable(folder);
    for (qsizetype i = 0; i < all.size(); ++i) {
        const history::Summary& s = all.at(i);
        auto* item = new QTreeWidgetItem(list, {s.title.isEmpty() ? tr("(no prompt)") : s.title,
                                                QDir::toNativeSeparators(s.folder), whenOf(s.updated),
                                                s.claudeFile.isEmpty() ? tr("the dock") : tr("Claude Code")});
        item->setData(0, Qt::UserRole, int(i));
        item->setToolTip(0, s.title);
        item->setToolTip(1, QDir::toNativeSeparators(s.folder));
    }
    auto* buttons = new QDialogButtonBox(dialog);
    QPushButton* open = buttons->addButton(tr("Resume"), QDialogButtonBox::AcceptRole);
    open->setObjectName(QStringLiteral("claudeResumeOpen"));
    QPushButton* forget = buttons->addButton(tr("Delete"), QDialogButtonBox::ActionRole);
    forget->setObjectName(QStringLiteral("claudeResumeDelete"));
    forget->setToolTip(tr("Forget a conversation the dock kept (Claude Code's own sessions are left as they are)"));
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(filter);
    layout->addWidget(list, 1);
    layout->addWidget(buttons);

    const auto chosen = [list]() -> int {
        const QTreeWidgetItem* item = list->currentItem();
        return item != nullptr && !item->isHidden() ? item->data(0, Qt::UserRole).toInt() : -1;
    };
    const auto refresh = [open, forget, chosen, all] {
        open->setEnabled(chosen() >= 0);
        forget->setEnabled(chosen() >= 0 && all.at(chosen()).claudeFile.isEmpty());
    };
    connect(filter, &QLineEdit::textChanged, dialog, [list, refresh](const QString& text) {
        QTreeWidgetItem* first = nullptr;
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = list->topLevelItem(i);
            const bool shown = text.trimmed().isEmpty() || item->text(0).contains(text.trimmed(), Qt::CaseInsensitive)
                               || item->text(1).contains(text.trimmed(), Qt::CaseInsensitive);
            item->setHidden(!shown);
            if (shown && first == nullptr) first = item;
        }
        if (list->currentItem() == nullptr || list->currentItem()->isHidden()) list->setCurrentItem(first);
        refresh();
    });
    connect(list, &QTreeWidget::currentItemChanged, dialog, refresh);
    connect(list, &QTreeWidget::itemDoubleClicked, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(forget, &QPushButton::clicked, dialog, [list, chosen, all, refresh] {
        const int index = chosen();
        if (index < 0 || !all.at(index).claudeFile.isEmpty()) return;
        history::remove(all.at(index).id);
        delete list->currentItem();
        refresh();
    });
    list->setCurrentItem(list->topLevelItem(0));
    filter->setText(query);
    refresh();
    return dialog;
}

void ClaudeCodeTabs::showResume(ClaudeCodePanel* from, const QString& query)
{
    using namespace qucs_s::claude;
    const QString folder = from != nullptr ? from->workingDirectory() : a_defaultDir;
    const QList<history::Summary> all = resumable(folder);
    // Named: gone on with at once.
    const QString wanted = query.trimmed();
    if (!wanted.isEmpty()) {
        for (const history::Summary& s : all)
            if (s.id == wanted || s.sessionId == wanted) {
                resume(s);
                return;
            }
        // One of Claude Code's sessions, of any folder.
        const QString file = history::claudeSessionFile(wanted);
        if (!file.isEmpty()) {
            resume(history::claudeSessionSummary(file));
            return;
        }
    }
    QDialog* dialog = resumeDialog(folder, wanted);
    auto* list = dialog->findChild<QTreeWidget*>(QStringLiteral("claudeResumeList"));
    if (dialog->exec() == QDialog::Accepted && list->currentItem() != nullptr && !list->currentItem()->isHidden()) {
        const history::Summary chosen = all.at(list->currentItem()->data(0, Qt::UserRole).toInt());
        if (resume(chosen) == nullptr) QMessageBox::warning(this, tr("Claude Code"), tr("That conversation could not be read."));
    }
    delete dialog;
}


void ClaudeCodeTabs::showConversation(ClaudeCodePanel* panel)
{
    if (a_tabs->indexOf(panel) >= 0) a_tabs->setCurrentWidget(panel);
}

QMenu* ClaudeCodeTabs::tabMenu(int index)
{
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("claudeTabMenu"));
    QPointer<ClaudeCodePanel> panel = qobject_cast<ClaudeCodePanel*>(a_tabs->widget(index));
    if (panel == nullptr) return menu;
    // (After the menu has gone: the keyboard it gave back is the editor's.)
    QAction* rename = menu->addAction(tr("Rename…"), this, [this, panel] {
        QMetaObject::invokeMethod(
            this, [this, panel] { if (panel != nullptr) renameConversation(panel); }, Qt::QueuedConnection);
    });
    rename->setObjectName(QStringLiteral("claudeRenameTab"));
    QAction* reset = menu->addAction(tr("Reset Name"), this, [panel] { if (panel != nullptr) panel->setName(QString()); });
    reset->setObjectName(QStringLiteral("claudeResetTabName"));
    reset->setEnabled(!panel->name().isEmpty());
    reset->setToolTip(tr("Name the conversation by its first prompt again"));
    menu->addSeparator();
    QAction* close = menu->addAction(tr("Close Conversation"), this, [this, panel] {
        QMetaObject::invokeMethod(
            this, [this, panel] { if (panel != nullptr) closeConversation(panel); }, Qt::QueuedConnection);
    });
    close->setObjectName(QStringLiteral("claudeCloseTabMenu"));
    return menu;
}

void ClaudeCodeTabs::renameConversation(ClaudeCodePanel* panel)
{
    if (a_tabs->indexOf(panel) < 0) return;
    finishRenaming(true, false);
    auto* editor = new QLineEdit(a_tabs->tabBar());
    editor->setObjectName(QStringLiteral("claudeTabRename"));
    editor->setMaxLength(100);
    editor->setPlaceholderText(tr("Its first prompt"));
    editor->setToolTip(tr("Enter keeps the name, Esc leaves it as it was; empty, the first prompt names the conversation"));
    editor->setText(panel->title());
    editor->selectAll();
    a_renaming = panel;
    a_renameEditor = editor;
    placeRenameEditor();
    editor->installEventFilter(this);
    connect(editor, &QLineEdit::returnPressed, this, [this] { finishRenaming(true, true); });
    editor->show();
    editor->setFocus(Qt::OtherFocusReason);
}

QLineEdit* ClaudeCodeTabs::renameEditor() const
{
    return a_renameEditor;
}

void ClaudeCodeTabs::finishRenaming(bool keep, bool refocus)
{
    QLineEdit* editor = a_renameEditor;
    ClaudeCodePanel* panel = a_renaming;
    a_renameEditor = nullptr;
    a_renaming = nullptr;
    if (editor == nullptr) return;
    editor->removeEventFilter(this);
    editor->disconnect(this);
    const bool hadFocus = editor->hasFocus();
    if (keep && panel != nullptr) {
        // Left as it was shown, a conversation named by its first prompt
        // stays so.
        const QString name = editor->text().simplified();
        if (name.isEmpty())
            panel->setName(QString());
        else if (!panel->name().isEmpty() || name != panel->title())
            panel->setName(name);
    }
    editor->hide();
    editor->deleteLater();
    if (refocus && hadFocus) focusComposer();
}

QRect ClaudeCodeTabs::renameRect() const
{
    const int index = a_tabs->indexOf(a_renaming);
    if (index < 0) return {};
    const QTabBar* bar = a_tabs->tabBar();
    // Over the mark and the text, up to the close button; wide enough to
    // write in however short the tab.
    QRect r = bar->tabRect(index).adjusted(3, 2, -2, -3);
    if (const QWidget* close = bar->tabButton(index, QTabBar::RightSide)) r.setRight(close->geometry().left() - 2);
    r.setWidth(std::max(r.width(), 160));
    if (r.right() > bar->width() - 2) r.moveRight(bar->width() - 2);
    if (r.left() < 0) r.moveLeft(0);
    return r;
}

void ClaudeCodeTabs::placeRenameEditor()
{
    if (a_renameEditor == nullptr) return;
    const QRect r = renameRect();
    if (r.isValid() && r != a_renameEditor->geometry()) a_renameEditor->setGeometry(r);
}

bool ClaudeCodeTabs::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == a_renameEditor) {
        const QEvent::Type type = event->type();
        if (type == QEvent::ShortcutOverride || type == QEvent::KeyPress) {
            if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
                // Esc is the editor's, not a shortcut of the window's.
                if (type == QEvent::ShortcutOverride) {
                    event->accept();
                    return true;
                }
                finishRenaming(false, true);
                return true;
            }
        } else if (type == QEvent::FocusOut) {
            // Its own menu (a right click in it) is still the rename.
            if (static_cast<QFocusEvent*>(event)->reason() != Qt::PopupFocusReason) finishRenaming(true, false);
        }
    } else if (watched == a_tabs->tabBar() && a_renameEditor != nullptr) {
        // The tabs moved under it (resized, scrolled, a text changed):
        // after the tab bar is drawn, where its tab is now.
        if (event->type() == QEvent::Paint || event->type() == QEvent::Resize) {
            const QRect r = renameRect();
            if (r.isValid() && r != a_renameEditor->geometry())
                QMetaObject::invokeMethod(this, &ClaudeCodeTabs::placeRenameEditor, Qt::QueuedConnection);
        }
    }
    return QWidget::eventFilter(watched, event);
}

ClaudeCodePanel* ClaudeCodeTabs::needingAttention() const
{
    ClaudeCodePanel* front = current();
    if (front != nullptr && front->session()->state() == State::Waiting) return front;
    for (ClaudeCodePanel* panel : panels())
        if (panel->session()->state() == State::Waiting) return panel;
    return nullptr;
}

ClaudeCodePanel* ClaudeCodeTabs::mostUrgent() const
{
    if (ClaudeCodePanel* waiting = needingAttention()) return waiting;
    ClaudeCodePanel* front = current();
    if (front != nullptr && front->session()->isBusy()) return front;
    for (ClaudeCodePanel* panel : panels())
        if (panel->session()->isBusy()) return panel;
    return front;
}

void ClaudeCodeTabs::permissionAsked(ClaudeCodePanel* panel)
{
    // Forward when nothing else is in front of it; else its tab says so.
    QDockWidget* dock = nullptr;
    for (QWidget* w = parentWidget(); w != nullptr && dock == nullptr; w = w->parentWidget())
        dock = qobject_cast<QDockWidget*>(w);
    const bool hidden = dock != nullptr && !dock->isVisible();
    if (panel == current() || hidden) {
        showConversation(panel);
        if (dock != nullptr) {
            dock->show();
            dock->raise();
        }
    }
    updateTab(panel);
    emit stateChanged();
}

void ClaudeCodeTabs::updateTab(ClaudeCodePanel* panel)
{
    const int index = a_tabs->indexOf(panel);
    if (index < 0) return;
    QString title = panel->title();
    a_tabs->setTabText(index, title.replace(QLatin1Char('&'), QLatin1String("&&")));
    a_tabs->setTabIcon(index, QIcon(panel->statePixmap()));
    QString tip = panel->title() + QLatin1Char('\n')
                  + tr("Claude: %1").arg(qucs_s::claude::stateText(panel->session()->state())) + QLatin1Char('\n')
                  + tr("Working in %1").arg(QDir::toNativeSeparators(panel->workingDirectory()));
    if (!panel->pinnedDocument().isEmpty())
        tip += QLatin1Char('\n') + tr("Pinned to %1").arg(QDir::toNativeSeparators(panel->pinnedDocument()));
    a_tabs->setTabToolTip(index, tip);
}

void ClaudeCodeTabs::setDefaultDirectory(const QString& dir)
{
    a_defaultDir = dir;
    for (ClaudeCodePanel* panel : panels()) {
        panel->setDefaultDirectory(dir);
        updateTab(panel);
    }
}

void ClaudeCodeTabs::setDocumentProvider(std::function<QString()> provider)
{
    a_document = std::move(provider);
    for (ClaudeCodePanel* panel : panels()) panel->setDocumentProvider(a_document);
}

void ClaudeCodeTabs::setSchematicsProvider(std::function<QStringList()> provider)
{
    a_schematics = std::move(provider);
    for (ClaudeCodePanel* panel : panels()) panel->setSchematicsProvider(a_schematics);
}

void ClaudeCodeTabs::documentRenamed(const QString& from, const QString& to)
{
    if (from.isEmpty()) return;
    for (ClaudeCodePanel* panel : panels())
        if (panel->isPinnedTo(from)) panel->pinDocument(to);
}

void ClaudeCodeTabs::setToolHost(qucs_s::claude::ToolHost* host)
{
    a_host = host;
    for (ClaudeCodePanel* panel : panels()) panel->session()->setToolHost(host);
}

void ClaudeCodeTabs::refreshDocument()
{
    for (ClaudeCodePanel* panel : panels()) panel->refreshDocument();
}

void ClaudeCodeTabs::addNote(const QString& text)
{
    ClaudeCodePanel* panel = a_reporting != nullptr ? a_reporting : current();
    if (panel != nullptr) panel->addNote(text);
}

void ClaudeCodeTabs::focusComposer()
{
    if (ClaudeCodePanel* panel = current()) panel->focusComposer();
}

void ClaudeCodeTabs::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        QTimer::singleShot(0, this, &ClaudeCodeTabs::restyle);
}

void ClaudeCodeTabs::restyle()
{
    using qucs_s::apptheme::mix;
    const QPalette pal = palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor accent = ClaudeCodePanel::accentColour(pal);
    const QString sheet =
        QStringLiteral(
            "QTabWidget#claudeTabs::pane { border: none; }"
            "QTabBar#claudeTabBar { background: %1; }"
            "QTabBar#claudeTabBar::tab { background: transparent; color: %2; border: none;"
            " border-bottom: 2px solid transparent; padding: 5px 2px 4px 8px; margin-right: 1px; max-width: 190px; }"
            "QTabBar#claudeTabBar::tab:selected { color: %3; border-bottom-color: %4; }"
            "QTabBar#claudeTabBar::tab:hover:!selected { background: %5; }"
            "QToolButton#claudeCloseTab { border: none; border-radius: 4px; padding: 0 3px; color: %2; background: transparent; }"
            "QToolButton#claudeCloseTab:hover { background: %6; color: %3; }"
            "QToolButton#claudeNewTab { border: none; border-radius: 5px; padding: 0 8px; color: %2; background: transparent;"
            " font-size: 16px; }"
            "QToolButton#claudeNewTab:hover { background: %5; color: %3; }"
            "QLineEdit#claudeTabRename { border: 1px solid %4; border-radius: 4px; padding: 0 4px; color: %3;"
            " background: %7; }"
            "QTabWidget#claudeTabs > QTabBar, QTabWidget#claudeTabs { background: %1; }")
            .arg(window.name(), mix(window, text, 0.62).name(), text.name(), accent.name(), mix(window, text, 0.07).name(),
                 mix(window, text, 0.16).name(), pal.color(QPalette::Base).name());
    if (sheet != styleSheet()) setStyleSheet(sheet);
}
