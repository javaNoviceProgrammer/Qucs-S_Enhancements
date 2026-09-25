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

#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QIcon>
#include <QMessageBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

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
    connect(a_tabs, &QTabWidget::currentChanged, this, [this] { emit stateChanged(); });
    connect(a_tabs->tabBar(), &QTabBar::tabMoved, this, [this] { emit stateChanged(); });
    addPanel(nullptr);
    restyle();
}

ClaudeCodeTabs::~ClaudeCodeTabs()
{
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

    connect(panel, &ClaudeCodePanel::newConversationRequested, this, [this] { newConversation(); });
    connect(panel, &ClaudeCodePanel::titleChanged, this, [this, panel] { updateTab(panel); });
    connect(panel, &ClaudeCodePanel::openFileRequested, this, &ClaudeCodeTabs::openFileRequested);
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
    panel->disconnect(this);
    panel->session()->disconnect(this);
    a_tabs->removeTab(index);
    panel->session()->stop();   // its input closed: the program keeps the conversation and ends
    panel->hide();
    panel->deleteLater();
    if (a_tabs->count() == 0) newConversation();
    emit stateChanged();
    return true;
}

void ClaudeCodeTabs::showConversation(ClaudeCodePanel* panel)
{
    if (a_tabs->indexOf(panel) >= 0) a_tabs->setCurrentWidget(panel);
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
    a_tabs->setTabToolTip(index, panel->title() + QLatin1Char('\n')
                                     + tr("Claude: %1").arg(qucs_s::claude::stateText(panel->session()->state()))
                                     + QLatin1Char('\n')
                                     + tr("Working in %1").arg(QDir::toNativeSeparators(panel->workingDirectory())));
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
            "QTabWidget#claudeTabs > QTabBar, QTabWidget#claudeTabs { background: %1; }")
            .arg(window.name(), mix(window, text, 0.62).name(), text.name(), accent.name(), mix(window, text, 0.07).name(),
                 mix(window, text, 0.16).name());
    if (sheet != styleSheet()) setStyleSheet(sheet);
}
