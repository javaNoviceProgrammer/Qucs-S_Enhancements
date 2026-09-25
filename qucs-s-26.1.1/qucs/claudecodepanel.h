/*
 * claudecodepanel.h - the Claude Code dock: a conversation with Claude
 *                     Code, working in the workspace folder
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_CLAUDECODEPANEL_H
#define QUCS_CLAUDECODEPANEL_H

#include "claudecode.h"

#include <QList>
#include <QWidget>

#include <functional>

class QAction;
class QActionGroup;
class QFrame;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QTextBrowser;
class QTextCursor;
class QTimer;
class QToolButton;
class QUrl;

/*!
 * The Claude Code dock. At the top, the state of the session, the model,
 * a new conversation and a menu (who decides what Claude may do, the
 * model, the folder, the program); under it the folder Claude works in -
 * the workspace unless another is chosen. Then the conversation: the
 * prompts, the replies as they are written (Markdown), each tool Claude
 * uses and how it went, how each turn ended. A tool that needs permission
 * is asked about in a card above the prompt, which is written at the
 * bottom: Enter sends, Shift+Enter starts a line, and the document in
 * front can go along with it.
 *
 * The program is the one the settings name, else claude as installed
 * (qucs_s::claude::findProgram()); QUCS_CLAUDE in the environment names
 * it over both.
 */
class ClaudeCodePanel : public QWidget
{
    Q_OBJECT

public:
    explicit ClaudeCodePanel(QWidget* parent = nullptr);
    ~ClaudeCodePanel() override;

    qucs_s::claude::Session* session() const { return a_session; }

    /// Where Claude works unless the user chose another folder: the
    /// workspace. A panel working there moves along (a new conversation).
    void setDefaultDirectory(const QString& dir);
    QString defaultDirectory() const { return a_defaultDir; }
    /// Where Claude works now.
    QString workingDirectory() const;
    /// Works in \a dir from now on - a new conversation; empty: the
    /// default directory.
    void setWorkingDirectory(const QString& dir);

    /// What names the document in front (its file, empty for none).
    void setDocumentProvider(std::function<QString()> provider);
    /// The document in front changed.
    void refreshDocument();

    /// A line in the conversation from the application (a document
    /// reloaded, say).
    void addNote(const QString& text);

    // For the tests.
    QPlainTextEdit* composer() const { return a_input; }
    QTextBrowser* transcript() const { return a_view; }
    QToolButton* sendButton() const { return a_send; }
    QToolButton* attachButton() const { return a_attach; }
    QFrame* permissionCard() const { return a_card; }
    QToolButton* allowButton() const { return a_allow; }
    QToolButton* allowEditsButton() const { return a_allowEdits; }
    QToolButton* denyButton() const { return a_deny; }
    QLabel* stateLabel() const { return a_stateText; }
    QLabel* modelLabel() const { return a_modelLabel; }
    QList<QAction*> modelActions() const;
    QList<QAction*> permissionActions() const;
    qucs_s::claude::ModelQuery* modelQuery() const { return a_modelQuery; }
    QString transcriptText() const;
    /// The conversation drawn now rather than a moment later.
    void renderNow();

public slots:
    void focusComposer();
    /// Sends what the composer holds.
    void sendComposer();
    /// Stops the turn under way.
    void stopTurn();
    void newConversation();

signals:
    /// Claude changed these files in a turn.
    void filesChanged(const QStringList& files);
    /// A file named in the conversation was clicked.
    void openFileRequested(const QString& path);

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct Entry {
        enum Kind { You, Claude, Tool, Note, Problem, Summary } kind;
        QString text;          // the prompt, the reply (Markdown), the note
        QString extra;         // the document sent along; a tool's subject; a failure
        QString id;            // a tool use's
        QString output = QString();   // why a tool failed
        enum ToolState { Running, Succeeded, Failed, Denied } tool = Running;
        bool streaming = false;
    };

    void buildHeader();
    void buildComposer();
    void buildPermissionCard();
    void buildMenu();
    void restyle();
    void scheduleRender();
    void render();
    void renderWelcome(QTextCursor& c);
    void renderEntry(QTextCursor& c, const Entry& e, bool& captioned);
    void append(const Entry& e);
    void updateState();
    void updateDirectory();
    void updateComposer();
    void showNextRequest();
    void answer(bool allow, bool allowEdits);
    void handleLink(const QUrl& url);
    QString programSetting() const;
    void findProgram();
    /// Asks the program which models it offers (once for each program).
    void listModels();
    void rebuildModelMenu();
    /// The choice that \a model is, or null.
    const qucs_s::claude::ModelChoice* choiceFor(const QString& model) const;
    void setPermissionMode(const QString& mode);
    void setModel(const QString& model);

    // Session events.
    void onReplyStreamed(const QString& text);
    void onReplyFinished(const QString& text);
    void onToolStarted(const QString& id, const QString& tool, const QString& subject);
    void onToolFinished(const QString& id, bool failed, const QString& output);
    void onPermissionRequested(const qucs_s::claude::PermissionRequest& request);
    void onPermissionWithdrawn(const QString& id);
    void onTurnFinished(const qucs_s::claude::TurnResult& result);

    qucs_s::claude::Session* a_session;
    QString a_defaultDir;
    QString a_chosenDir;       // empty: the default
    std::function<QString()> a_document;

    qucs_s::claude::ModelQuery* a_modelQuery;
    QString a_modelsFrom;      // the program asked which models it offers
    QJsonArray a_listedModels; // what it answered (kept in the settings)
    QList<qucs_s::claude::ModelChoice> a_choices;

    QList<Entry> a_entries;
    QList<qucs_s::claude::PermissionRequest> a_requests;
    QTimer* a_renderTimer;
    QTimer* a_clock;           // the seconds of a turn under way

    // The header.
    QWidget* a_header;
    QLabel* a_title;
    QLabel* a_stateDot;
    QLabel* a_stateText;
    QLabel* a_modelLabel;
    QToolButton* a_newButton;
    QToolButton* a_menuButton;
    QMenu* a_menu;
    QActionGroup* a_modes;
    QMenu* a_modelMenu;
    QActionGroup* a_models;
    // The folder.
    QWidget* a_dirRow;
    QLabel* a_dirIcon;
    QLabel* a_dirLabel;
    QToolButton* a_dirButton;
    QToolButton* a_dirReset;
    // The conversation.
    QTextBrowser* a_view;
    // The permission card.
    QFrame* a_card;
    QLabel* a_cardTitle;
    QLabel* a_cardSubject;
    QPlainTextEdit* a_cardDetail;
    QLabel* a_cardCount;
    QToolButton* a_allow;
    QToolButton* a_allowEdits;
    QToolButton* a_deny;
    // The composer.
    QFrame* a_composer;
    QPlainTextEdit* a_input;
    QToolButton* a_attach;
    QLabel* a_hint;
    QToolButton* a_send;
};

#endif // QUCS_CLAUDECODEPANEL_H
