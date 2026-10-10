/*
 * gitui.cpp - git in the windows: the commit dialog, the history, the
 *             blame, a diff; and what the git commands do
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "gitui.h"
#include "githistory.h"
#include "schematicdiff.h"

#include "ink.h"
#include "misc.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <memory>

namespace qucs_s::git {

namespace {

bool darkOf(const QPalette& palette)
{
    return qucs_s::ink::isDark(palette.color(QPalette::Base));
}

QFont fixedFont()
{
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    return f;
}

QString dateText(const QDateTime& when)
{
    return when.isValid() ? QLocale().toString(when.toLocalTime(), QLocale::ShortFormat) : QString();
}

// The paths of \a entries in a menu's line: a few names, then how many.
QString namesOf(const QStringList& paths)
{
    QStringList names;
    for (const QString& p : paths) names << QFileInfo(p).fileName();
    if (names.size() <= 3) return names.join(QStringLiteral(", "));
    return QCoreApplication::translate("Git", "%1 and %2 more").arg(names.mid(0, 3).join(QStringLiteral(", "))).arg(names.size() - 3);
}

} // namespace

QColor colourOf(const Entry& e, const QPalette& palette)
{
    const bool dark = darkOf(palette);
    const QColor red = dark ? QColor(0xf1, 0x4c, 0x4c) : QColor(0xc6, 0x28, 0x28);
    const QColor green = dark ? QColor(0x73, 0xc9, 0x91) : QColor(0x2e, 0x7d, 0x32);
    const QColor amber = dark ? QColor(0xe2, 0xc0, 0x8d) : QColor(0xa1, 0x62, 0x07);
    if (e.conflicted) return red;
    if (e.ignored) return dark ? QColor(0x8c, 0x8c, 0x8c) : QColor(0x9e, 0x9e, 0x9e);
    if (e.untracked) return green;
    if (e.unstaged == QLatin1Char('D') || (e.staged == QLatin1Char('D') && e.unstaged == QLatin1Char('.'))) return red;
    if (e.unstaged != QLatin1Char('.')) return amber;
    return green;   // all of it staged
}

QColor changedFolderColour(const QPalette& palette)
{
    return darkOf(palette) ? QColor(0xd7, 0xb9, 0x8a) : QColor(0x9a, 0x6a, 0x1e);
}

QIcon badgeOf(const Entry& entry, const QPalette& palette)
{
    const qreal dpr = 2.0;
    QPixmap pm(QSize(16, 16) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(colourOf(entry, palette));
    p.drawRoundedRect(QRectF(1.5, 1.5, 13, 13), 3, 3);
    QFont f = QApplication::font();
    f.setBold(true);
    f.setPixelSize(10);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRectF(1.5, 1.5, 13, 13), Qt::AlignCenter, entry.letter());
    p.end();
    return QIcon(pm);
}

// ----------------------------------------------------------------------
// DiffHighlighter, TextDialog

DiffHighlighter::DiffHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {}

void DiffHighlighter::highlightBlock(const QString& text)
{
    const QPalette pal = QApplication::palette();
    const bool dark = darkOf(pal);
    QColor colour;
    if (text.startsWith(QLatin1String("+++")) || text.startsWith(QLatin1String("---")) || text.startsWith(QLatin1String("diff "))
        || text.startsWith(QLatin1String("index ")) || text.startsWith(QLatin1String("commit ")))
        colour = dark ? QColor(0x9c, 0xdc, 0xfe) : QColor(0x24, 0x29, 0x2f);
    else if (text.startsWith(QLatin1String("@@")))
        colour = dark ? QColor(0xd2, 0xa8, 0xff) : QColor(0x82, 0x50, 0xdf);
    else if (text.startsWith(QLatin1Char('+')))
        colour = dark ? QColor(0x3f, 0xb9, 0x50) : QColor(0x1a, 0x7f, 0x37);
    else if (text.startsWith(QLatin1Char('-')))
        colour = dark ? QColor(0xf8, 0x51, 0x49) : QColor(0xcf, 0x22, 0x2e);
    if (!colour.isValid()) return;
    QTextCharFormat format;
    format.setForeground(colour);
    if (text.startsWith(QLatin1String("diff ")) || text.startsWith(QLatin1String("commit "))) format.setFontWeight(QFont::Bold);
    setFormat(0, int(text.size()), format);
}

TextDialog::TextDialog(const QString& title, const QString& text, QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("gitTextDialog"));
    setWindowTitle(title);
    auto* layout = new QVBoxLayout(this);
    a_view = new QPlainTextEdit(this);
    a_view->setObjectName(QStringLiteral("gitText"));
    a_view->setReadOnly(true);
    a_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    a_view->setFont(fixedFont());
    new DiffHighlighter(a_view->document());
    a_view->setPlainText(text);
    layout->addWidget(a_view, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(900, 650);
}

// ----------------------------------------------------------------------
// CommitDialog

CommitDialog::CommitDialog(const QString& root, QWidget* parent) : QDialog(parent), a_root(root)
{
    setObjectName(QStringLiteral("gitCommitDialog"));
    setWindowTitle(tr("Commit — %1").arg(QFileInfo(root).fileName()));
    auto* layout = new QVBoxLayout(this);
    a_header = new QLabel(this);
    a_header->setObjectName(QStringLiteral("gitCommitHeader"));
    a_header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(a_header);

    const auto listOf = [this](const char* name) {
        auto* list = new QListWidget(this);
        list->setObjectName(QLatin1String(name));
        list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        list->setUniformItemSizes(true);
        list->setIconSize(QSize(16, 16));
        return list;
    };
    auto* lists = new QSplitter(Qt::Vertical, this);
    auto* unstagedBox = new QWidget(lists);
    auto* unstagedLayout = new QVBoxLayout(unstagedBox);
    unstagedLayout->setContentsMargins(0, 0, 0, 0);
    auto* unstagedRow = new QHBoxLayout;
    unstagedRow->addWidget(new QLabel(tr("Changes not staged"), unstagedBox), 1);
    a_stage = new QPushButton(tr("Stage"), unstagedBox);
    a_stage->setObjectName(QStringLiteral("gitStageButton"));
    a_stage->setToolTip(tr("Stage the files chosen: they go into the next commit"));
    auto* stageAllButton = new QPushButton(tr("Stage All"), unstagedBox);
    stageAllButton->setObjectName(QStringLiteral("gitStageAllButton"));
    a_discard = new QPushButton(tr("Discard…"), unstagedBox);
    a_discard->setObjectName(QStringLiteral("gitDiscardButton"));
    a_discard->setToolTip(tr("Throw the changes of the files chosen away: back to their last commit (new files to the trash)"));
    unstagedRow->addWidget(a_stage);
    unstagedRow->addWidget(stageAllButton);
    unstagedRow->addWidget(a_discard);
    unstagedLayout->addLayout(unstagedRow);
    a_unstaged = listOf("gitUnstagedList");
    unstagedLayout->addWidget(a_unstaged, 1);

    auto* stagedBox = new QWidget(lists);
    auto* stagedLayout = new QVBoxLayout(stagedBox);
    stagedLayout->setContentsMargins(0, 0, 0, 0);
    auto* stagedRow = new QHBoxLayout;
    stagedRow->addWidget(new QLabel(tr("Staged: in the next commit"), stagedBox), 1);
    a_unstage = new QPushButton(tr("Unstage"), stagedBox);
    a_unstage->setObjectName(QStringLiteral("gitUnstageButton"));
    auto* unstageAllButton = new QPushButton(tr("Unstage All"), stagedBox);
    unstageAllButton->setObjectName(QStringLiteral("gitUnstageAllButton"));
    stagedRow->addWidget(a_unstage);
    stagedRow->addWidget(unstageAllButton);
    stagedLayout->addLayout(stagedRow);
    a_staged = listOf("gitStagedList");
    stagedLayout->addWidget(a_staged, 1);
    lists->addWidget(unstagedBox);
    lists->addWidget(stagedBox);

    a_diff = new QPlainTextEdit(this);
    a_diff->setObjectName(QStringLiteral("gitCommitDiff"));
    a_diff->setReadOnly(true);
    a_diff->setLineWrapMode(QPlainTextEdit::NoWrap);
    a_diff->setFont(fixedFont());
    a_diff->setPlaceholderText(tr("Choose a file to see its changes"));
    new DiffHighlighter(a_diff->document());
    auto* top = new QSplitter(Qt::Horizontal, this);
    top->addWidget(lists);
    top->addWidget(a_diff);
    top->setStretchFactor(1, 1);
    top->setSizes({380, 620});

    auto* messageBox = new QWidget(this);
    auto* messageLayout = new QVBoxLayout(messageBox);
    messageLayout->setContentsMargins(0, 0, 0, 0);
    messageLayout->addWidget(new QLabel(tr("Commit message"), messageBox));
    a_message = new QPlainTextEdit(messageBox);
    a_message->setObjectName(QStringLiteral("gitCommitMessage"));
    a_message->setPlaceholderText(tr("What it does, in a line; then, after a blank line, why"));
    a_message->setTabChangesFocus(true);
    messageLayout->addWidget(a_message, 1);
    auto* main = new QSplitter(Qt::Vertical, this);
    main->addWidget(top);
    main->addWidget(messageBox);
    main->setStretchFactor(0, 3);
    main->setStretchFactor(1, 1);
    layout->addWidget(main, 1);

    auto* bottom = new QHBoxLayout;
    a_amend = new QCheckBox(tr("Amend the last commit"), this);
    a_amend->setObjectName(QStringLiteral("gitAmend"));
    a_amend->setToolTip(tr("This commit in place of the last one (its message to edit): for a commit not pushed yet"));
    bottom->addWidget(a_amend);
    bottom->addStretch(1);
    a_commitPush = new QPushButton(tr("Commit and Push"), this);
    a_commitPush->setObjectName(QStringLiteral("gitCommitPushButton"));
    a_commit = new QPushButton(tr("Commit"), this);
    a_commit->setObjectName(QStringLiteral("gitCommitButton"));
    a_commit->setDefault(true);
    auto* close = new QPushButton(tr("Close"), this);
    bottom->addWidget(a_commitPush);
    bottom->addWidget(a_commit);
    bottom->addWidget(close);
    layout->addLayout(bottom);

    connect(a_stage, &QPushButton::clicked, this, &CommitDialog::stageSelected);
    connect(stageAllButton, &QPushButton::clicked, this, &CommitDialog::stageAll);
    connect(a_discard, &QPushButton::clicked, this, &CommitDialog::discardSelected);
    connect(a_unstage, &QPushButton::clicked, this, &CommitDialog::unstageSelected);
    connect(unstageAllButton, &QPushButton::clicked, this, &CommitDialog::unstageAll);
    connect(a_commit, &QPushButton::clicked, this, [this] { commitNow(false); });
    connect(a_commitPush, &QPushButton::clicked, this, [this] { commitNow(true); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(a_message, &QPlainTextEdit::textChanged, this, &CommitDialog::updateButtons);
    connect(a_amend, &QCheckBox::toggled, this, [this](bool on) {
        if (on && a_message->toPlainText().trimmed().isEmpty()) a_message->setPlainText(lastMessage(a_root));
        updateButtons();
    });
    for (QListWidget* list : {a_unstaged, a_staged}) {
        connect(list, &QListWidget::itemSelectionChanged, this, &CommitDialog::updateButtons);
        connect(list, &QListWidget::currentItemChanged, this, [this, list](QListWidgetItem* item) {
            if (item != nullptr) showDiffOf(item, list == a_staged);
        });
        connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
            const QString path = QDir(a_root).filePath(item->data(Qt::UserRole).toString());
            if (QFileInfo(path).isFile()) emit openRequested(path);
        });
    }
    reload();
    resize(1040, 740);
}

void CommitDialog::reload()
{
    const Repository* found = Tracker::instance()->readNow(a_root);
    const Repository repo = found != nullptr ? *found : Repository();
    a_header->setText(repo.valid ? tr("%1  ·  on %2").arg(QDir::toNativeSeparators(a_root), repo.branchText())
                                 : tr("%1 is no git repository").arg(QDir::toNativeSeparators(a_root)));
    a_merging = repo.operation == QLatin1String("merge");
    if (a_merging) a_header->setText(a_header->text() + QStringLiteral("  ·  ") + tr("a merge under way: Commit records it"));
    const auto keepSelection = [](QListWidget* list) {
        QStringList chosen;
        for (QListWidgetItem* item : list->selectedItems()) chosen << item->data(Qt::UserRole).toString();
        return chosen;
    };
    const QStringList wasUnstaged = keepSelection(a_unstaged), wasStaged = keepSelection(a_staged);
    a_unstaged->clear();
    a_staged->clear();
    const QPalette pal = palette();
    for (const Entry& e : repo.entries) {
        if (e.ignored) continue;
        const QString shown = e.from.isEmpty() ? e.path : tr("%1 → %2").arg(e.from, e.path);
        const auto add = [&](QListWidget* list, const Entry& as, const QStringList& was) {
            auto* item = new QListWidgetItem(badgeOf(as, pal), shown, list);
            item->setData(Qt::UserRole, e.path);
            item->setToolTip(e.describe());
            item->setForeground(colourOf(as, pal));
            if (was.contains(e.path)) item->setSelected(true);
        };
        if (e.hasUnstaged()) {
            Entry only = e;
            only.staged = QLatin1Char('.');
            add(a_unstaged, only, wasUnstaged);
        }
        if (e.hasStaged()) {
            Entry only = e;
            only.unstaged = QLatin1Char('.');
            add(a_staged, only, wasStaged);
        }
    }
    a_diff->clear();
    updateButtons();
}

QStringList CommitDialog::selectedPaths(QListWidget* list) const
{
    QStringList paths;
    for (QListWidgetItem* item : list->selectedItems()) paths << item->data(Qt::UserRole).toString();
    if (paths.isEmpty() && list->currentItem() != nullptr) paths << list->currentItem()->data(Qt::UserRole).toString();
    return paths;
}

void CommitDialog::updateButtons()
{
    const bool busy = a_job != nullptr;
    a_stage->setEnabled(!busy && !selectedPaths(a_unstaged).isEmpty());
    a_discard->setEnabled(!busy && !selectedPaths(a_unstaged).isEmpty());
    a_unstage->setEnabled(!busy && !selectedPaths(a_staged).isEmpty());
    const bool something = a_staged->count() > 0 || a_unstaged->count() > 0 || a_amend->isChecked() || a_merging;
    const bool ready = !busy && something && !a_message->toPlainText().trimmed().isEmpty();
    a_commit->setEnabled(ready);
    a_commitPush->setEnabled(ready);
}

void CommitDialog::showDiffOf(QListWidgetItem* item, bool staged)
{
    const QString path = item->data(Qt::UserRole).toString();
    const DiffOf of = staged ? DiffOf::Staged : DiffOf::Unstaged;
    // A schematic: what changed in it part by part, before git's lines.
    const QString parts = isSchematicFile(path) ? schematicChangesOf(a_root, QDir(a_root).filePath(path), of) : QString();
    const QString text = diff(a_root, path, of);
    a_diff->setPlainText(parts.isEmpty() ? text : parts + QLatin1Char('\n') + text);
}

void CommitDialog::stageSelected()
{
    const QStringList paths = selectedPaths(a_unstaged);
    if (paths.isEmpty()) return;
    const Result r = stage(a_root, paths);
    if (!r.ok()) misc::reportError(tr("Stage") + QStringLiteral(":\n") + r.error());
    reload();
}

void CommitDialog::unstageSelected()
{
    const QStringList paths = selectedPaths(a_staged);
    if (paths.isEmpty()) return;
    const Result r = unstage(a_root, paths);
    if (!r.ok()) misc::reportError(tr("Unstage") + QStringLiteral(":\n") + r.error());
    reload();
}

void CommitDialog::stageAll()
{
    const Result r = stage(a_root, {});
    if (!r.ok()) misc::reportError(tr("Stage All") + QStringLiteral(":\n") + r.error());
    reload();
}

void CommitDialog::unstageAll()
{
    const Result r = unstage(a_root, {});
    if (!r.ok()) misc::reportError(tr("Unstage All") + QStringLiteral(":\n") + r.error());
    reload();
}

void CommitDialog::discardSelected()
{
    const QStringList paths = selectedPaths(a_unstaged);
    if (paths.isEmpty()) return;
    Commands::instance()->discard(a_root, paths);
    reload();
}

void CommitDialog::commitNow(bool push)
{
    const QString message = a_message->toPlainText().trimmed();
    if (message.isEmpty()) {
        misc::reportError(tr("A commit needs a message."));
        return;
    }
    const bool amend = a_amend->isChecked();
    // (A merge's commit records it with nothing staged: each file resolved
    // as the branch had it.)
    if (a_staged->count() == 0 && !amend && !a_merging) {
        if (a_unstaged->count() == 0) {
            misc::reportError(tr("There is nothing to commit."));
            return;
        }
        QMessageBox box(QMessageBox::Question, tr("Commit"), tr("Nothing is staged. Stage all the changes and commit them?"),
                        QMessageBox::NoButton, this);
        box.setObjectName(QStringLiteral("gitStageAllQuestion"));
        QPushButton* yes = box.addButton(tr("Stage All and Commit"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(yes);
        box.exec();
        if (box.clickedButton() != yes) return;
        const Result staged = stage(a_root, {});
        if (!staged.ok()) {
            misc::reportError(tr("Stage All") + QStringLiteral(":\n") + staged.error());
            return;
        }
    }
    QStringList args{QStringLiteral("commit"), QStringLiteral("-F"), QStringLiteral("-")};
    if (amend) args << QStringLiteral("--amend");
    QPointer<CommitDialog> self(this);
    Commands::instance()->runJob(
        a_root, args, tr("Committing"),
        [self, push](const Result& r) {
            if (self.isNull()) return;
            if (!r.ok()) {
                misc::reportError(tr("Commit") + QStringLiteral(":\n") + r.error());
                self->reload();
                return;
            }
            const QString hash = run(self->a_root, {QStringLiteral("rev-parse"), QStringLiteral("--short"), QStringLiteral("HEAD")}, true, 15000)
                                     .out.trimmed();
            self->a_message->clear();
            self->a_amend->setChecked(false);
            self->reload();
            emit Commands::instance()->message(tr("Committed %1").arg(hash));
            emit self->committed(hash, push);
            if (push) Commands::instance()->push(self->a_root);
        },
        message.toUtf8());
    a_job = Commands::instance()->runningJob();
    updateButtons();
    if (a_job != nullptr)
        connect(a_job, &Job::finished, this, [this] {
            a_job = nullptr;   // (done: deleted later)
            updateButtons();
        });
}

// ----------------------------------------------------------------------
// BlameDialog

BlameDialog::BlameDialog(const QString& root, const QString& path, QWidget* parent) : QDialog(parent), a_root(root)
{
    setObjectName(QStringLiteral("gitBlameDialog"));
    setWindowTitle(tr("Blame — %1").arg(QFileInfo(path).fileName()));
    auto* layout = new QVBoxLayout(this);
    a_lines = new QTreeWidget(this);
    a_lines->setObjectName(QStringLiteral("gitBlameLines"));
    a_lines->setRootIsDecorated(false);
    a_lines->setUniformRowHeights(true);
    a_lines->setHeaderLabels({tr("Line"), tr("Commit"), tr("Author"), tr("Date"), tr("Text")});
    a_lines->header()->setStretchLastSection(true);
    const QFont fixed = fixedFont();
    QString last;
    bool shade = false;
    const QColor tint = darkOf(palette()) ? QColor(255, 255, 255, 14) : QColor(0, 0, 0, 10);
    for (const BlameLine& l : blame(root, path)) {
        if (l.hash != last) shade = !shade;
        const bool first = l.hash != last;
        last = l.hash;
        auto* item = new QTreeWidgetItem(a_lines, {QString::number(l.line), first ? (l.uncommitted ? tr("not committed") : l.shortHash) : QString(),
                                                   first ? (l.uncommitted ? QString() : l.author) : QString(),
                                                   first && !l.uncommitted ? dateText(l.date) : QString(), l.text});
        item->setData(0, Qt::UserRole, l.uncommitted ? QString() : l.hash);
        item->setFont(4, fixed);
        item->setToolTip(1, l.uncommitted ? tr("Not committed yet") : QStringLiteral("%1 — %2").arg(l.shortHash, l.summary));
        item->setToolTip(4, item->toolTip(1));
        if (shade)
            for (int c = 0; c < 5; ++c) item->setBackground(c, tint);
    }
    for (int c = 0; c < 4; ++c) a_lines->resizeColumnToContents(c);
    connect(a_lines, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const QString hash = item->data(0, Qt::UserRole).toString();
        if (hash.isEmpty()) return;
        auto* d = new TextDialog(tr("Commit %1").arg(hash.left(7)), git::show(a_root, hash), this);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->show();
    });
    layout->addWidget(a_lines, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(1000, 700);
}

// ----------------------------------------------------------------------
// Commands

Commands* Commands::instance()
{
    static QPointer<Commands> one;
    if (one.isNull()) one = new Commands(QCoreApplication::instance());
    return one;
}

Commands::Commands(QObject* parent) : QObject(parent) {}

QWidget* Commands::window() const
{
    return QApplication::activeWindow();
}

bool Commands::report(const Result& result, const QString& what)
{
    if (result.ok()) return true;
    misc::reportError(what + QStringLiteral(":\n") + result.error());
    return false;
}

void Commands::done(const QString& root, const QString& said)
{
    Tracker::instance()->readNow(root);   // (what it did shown at once)
    if (!said.isEmpty()) emit message(said);
}

void Commands::keep(QDialog* dialog)
{
    a_windows.removeAll(nullptr);
    a_windows << dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void Commands::runJob(const QString& dir, const QStringList& args, const QString& title,
                      const std::function<void(const Result&)>& then, const QByteArray& input)
{
    if (a_job != nullptr && a_job->isRunning()) {
        misc::reportError(tr("git is busy (%1): try again when it is done.").arg(a_job->arguments().value(0)));
        return;
    }
    auto* job = new Job(dir, args, this, input);
    a_job = job;
    // Its progress, when it takes a moment.
    auto box = std::make_shared<QPointer<QDialog>>();
    auto line = std::make_shared<QPointer<QLabel>>();
    QTimer::singleShot(600, this, [this, job, box, line, title] {
        if (a_job != job || !job->isRunning()) return;
        auto* d = new QDialog(window());
        d->setObjectName(QStringLiteral("gitProgress"));
        d->setWindowTitle(title);
        d->setWindowModality(Qt::WindowModal);
        auto* l = new QVBoxLayout(d);
        auto* what = new QLabel(title + QStringLiteral("…"), d);
        l->addWidget(what);
        auto* at = new QLabel(d);
        at->setMinimumWidth(420);
        l->addWidget(at);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, d);
        connect(buttons, &QDialogButtonBox::rejected, job, &Job::cancel);
        l->addWidget(buttons);
        *box = d;
        *line = at;
        d->show();
    });
    connect(job, &Job::progress, this, [line](const QString& text) {
        if (!line->isNull()) (*line)->setText(text);
    });
    connect(job, &Job::finished, this, [this, job, box, dir, title, then](const Result& r) {
        if (!box->isNull()) (*box)->deleteLater();
        job->deleteLater();
        if (a_job == job) a_job = nullptr;
        if (then) then(r);
        else if (report(r, title)) done(dir, tr("%1: done").arg(title));
        Tracker::instance()->refresh(dir);
    });
    job->start();
}

// The menus

void Commands::fillEntryMenu(QMenu* menu, const QStringList& paths)
{
    if (paths.isEmpty()) return;
    const QString first = paths.first();
    const QString root = topLevel(first);
    if (root.isEmpty()) {
        const QString folder = QFileInfo(first).isDir() ? first : QFileInfo(first).absolutePath();
        menu->addAction(tr("Create Repository Here…"), this, [this, folder] { createRepository(folder); })
            ->setObjectName(QStringLiteral("gitInitHere"));
        menu->addAction(tr("Clone a Repository Here…"), this, [this, folder] { cloneRepository(folder); })
            ->setObjectName(QStringLiteral("gitCloneHere"));
        return;
    }
    const Repository* found = Tracker::instance()->fresh(first);
    const Repository repo = found != nullptr ? *found : Repository();
    bool unstaged = false, staged = false, changed = false, ignored = true, untracked = false;
    for (const QString& p : paths) {
        bool inside = false;
        const QString rel = relativePath(root, p, &inside);
        if (!inside) continue;
        const bool folder = QFileInfo(p).isDir();
        const Entry* e = rel.isEmpty() ? nullptr : repo.entryOf(rel);
        if (e == nullptr || !e->ignored) ignored = false;
        if (e != nullptr && e->untracked) untracked = true;
        if (e != nullptr && !e->ignored) {
            changed = true;
            unstaged = unstaged || e->hasUnstaged();
            staged = staged || e->hasStaged();
        }
        if (folder && repo.changedIn(rel)) {
            changed = true;
            for (const Entry& x : repo.entries) {
                if (x.ignored || !(rel.isEmpty() || x.path.startsWith(rel + QLatin1Char('/')))) continue;
                unstaged = unstaged || x.hasUnstaged();
                staged = staged || x.hasStaged();
            }
        }
    }
    const QString one = paths.size() == 1 ? first : QString();
    const bool oneFile = !one.isEmpty() && QFileInfo(one).isFile();
    if (oneFile && addResolveMenu(menu, root, one) != nullptr) menu->addSeparator();
    menu->addAction(tr("Commit…"), this, [this, root] { commit(root); })->setObjectName(QStringLiteral("gitCommit"));
    menu->addSeparator();
    QAction* a = menu->addAction(tr("Stage"), this, [this, root, paths] { stage(root, paths); });
    a->setObjectName(QStringLiteral("gitStage"));
    a->setEnabled(unstaged);
    a = menu->addAction(tr("Unstage"), this, [this, root, paths] { unstage(root, paths); });
    a->setObjectName(QStringLiteral("gitUnstage"));
    a->setEnabled(staged);
    a = menu->addAction(tr("Discard Changes…"), this, [this, root, paths] { discard(root, paths); });
    a->setObjectName(QStringLiteral("gitDiscard"));
    a->setEnabled(changed);
    menu->addSeparator();
    a = menu->addAction(tr("Show Changes"), this, [this, root, one] { showDiff(root, one, DiffOf::Head); });
    a->setObjectName(QStringLiteral("gitShowChanges"));
    a->setEnabled(!one.isEmpty() && changed);
    a = menu->addAction(tr("Show History"), this, [this, root, one] { showHistory(root, one); });
    a->setObjectName(QStringLiteral("gitShowHistory"));
    a->setEnabled(!one.isEmpty() && !untracked && !repo.head.isEmpty());
    a = menu->addAction(tr("Show Blame"), this, [this, root, one] { showBlame(root, one); });
    a->setObjectName(QStringLiteral("gitShowBlame"));
    a->setEnabled(oneFile && !untracked && !ignored && !repo.head.isEmpty());
    menu->addSeparator();
    a = menu->addAction(tr("Add to .gitignore"), this, [this, root, one] { ignore(root, one); });
    a->setObjectName(QStringLiteral("gitIgnore"));
    a->setEnabled(!one.isEmpty() && !ignored && !relativePath(root, one).isEmpty());
    a = menu->addAction(tr("Stop Tracking, Keep the File"), this, [this, root, paths] { untrack(root, paths); });
    a->setObjectName(QStringLiteral("gitUntrack"));
    a->setEnabled(!untracked && !ignored && !repo.head.isEmpty());
    menu->addSeparator();
    menu->addAction(tr("Fetch"), this, [this, root] { fetch(root); })->setObjectName(QStringLiteral("gitFetch"));
    menu->addAction(tr("Pull"), this, [this, root] { pull(root); })->setObjectName(QStringLiteral("gitPull"));
    menu->addAction(tr("Push"), this, [this, root] { push(root); })->setObjectName(QStringLiteral("gitPush"));
    QMenu* branches = menu->addMenu(tr("Switch to Branch"));
    branches->setObjectName(QStringLiteral("gitSwitchMenu"));
    connect(branches, &QMenu::aboutToShow, this, [this, branches, root] {
        branches->clear();
        fillBranchMenu(branches, root);
    });
    menu->addAction(tr("New Branch…"), this, [this, root] { newBranch(root); })->setObjectName(QStringLiteral("gitNewBranch"));
    a = menu->addAction(tr("Stash Changes…"), this, [this, root] { stashChanges(root); });
    a->setObjectName(QStringLiteral("gitStash"));
    a->setEnabled(repo.changedCount() > 0 && !repo.head.isEmpty());
}

void Commands::fillFileMenu(QMenu* menu, const QString& file)
{
    const QString root = topLevel(file);
    if (root.isEmpty()) {
        const QString folder = QFileInfo(file).absolutePath();
        menu->addAction(tr("Create Repository Here…"), this, [this, folder] { createRepository(folder); })
            ->setObjectName(QStringLiteral("gitInitHere"));
        return;
    }
    const Repository* found = Tracker::instance()->fresh(file);
    const Repository repo = found != nullptr ? *found : Repository();
    const QString rel = relativePath(root, file);
    const Entry* e = repo.entryOf(rel);
    const Entry entry = e != nullptr ? *e : Entry();
    const bool changed = e != nullptr && !e->ignored;
    // What it stands at.
    QAction* where = menu->addAction(tr("%1 — %2").arg(QFileInfo(root).fileName(), repo.branchText()));
    where->setEnabled(false);
    QAction* state = menu->addAction(e == nullptr ? tr("%1: as committed").arg(QFileInfo(file).fileName())
                                                  : QStringLiteral("%1: %2").arg(QFileInfo(file).fileName(), e->describe()));
    state->setEnabled(false);
    if (!repo.operation.isEmpty()) {
        QAction* op = menu->addAction(tr("A %1 is under way").arg(repo.operation));
        op->setEnabled(false);
    }
    menu->addSeparator();
    if (addResolveMenu(menu, root, file) != nullptr) menu->addSeparator();
    menu->addAction(tr("Commit…"), this, [this, root] { commit(root); })->setObjectName(QStringLiteral("gitCommit"));
    QAction* a = menu->addAction(tr("Show Changes in This File"), this, [this, root, file] { showDiff(root, file, DiffOf::Head); });
    a->setObjectName(QStringLiteral("gitShowChanges"));
    a->setEnabled(changed);
    a = menu->addAction(tr("Stage This File"), this, [this, root, file] { stage(root, {file}); });
    a->setObjectName(QStringLiteral("gitStage"));
    a->setEnabled(changed && entry.hasUnstaged());
    a = menu->addAction(tr("Unstage This File"), this, [this, root, file] { unstage(root, {file}); });
    a->setObjectName(QStringLiteral("gitUnstage"));
    a->setEnabled(changed && entry.hasStaged());
    a = menu->addAction(tr("Discard Changes in This File…"), this, [this, root, file] { discard(root, {file}); });
    a->setObjectName(QStringLiteral("gitDiscard"));
    a->setEnabled(changed);
    a = menu->addAction(tr("History of This File"), this, [this, root, file] { showHistory(root, file); });
    a->setObjectName(QStringLiteral("gitShowHistory"));
    a->setEnabled(!entry.untracked && !repo.head.isEmpty());
    a = menu->addAction(tr("Blame This File"), this, [this, root, file] { showBlame(root, file); });
    a->setObjectName(QStringLiteral("gitShowBlame"));
    a->setEnabled(!entry.untracked && !entry.ignored && !repo.head.isEmpty());
    menu->addSeparator();
    a = menu->addAction(tr("Show All Changes"), this, [this, root] { showDiff(root, {}, DiffOf::Head); });
    a->setObjectName(QStringLiteral("gitShowAllChanges"));
    a->setEnabled(repo.changedCount() > 0);
    menu->addAction(tr("History"), this, [this, root] { showHistory(root); })->setObjectName(QStringLiteral("gitHistory"));
    menu->addSeparator();
    menu->addAction(tr("Fetch"), this, [this, root] { fetch(root); })->setObjectName(QStringLiteral("gitFetch"));
    menu->addAction(tr("Pull"), this, [this, root] { pull(root); })->setObjectName(QStringLiteral("gitPull"));
    menu->addAction(tr("Push"), this, [this, root] { push(root); })->setObjectName(QStringLiteral("gitPush"));
    QMenu* branches = menu->addMenu(tr("Switch to Branch"));
    branches->setObjectName(QStringLiteral("gitSwitchMenu"));
    connect(branches, &QMenu::aboutToShow, this, [this, branches, root] {
        branches->clear();
        fillBranchMenu(branches, root);
    });
    menu->addAction(tr("New Branch…"), this, [this, root] { newBranch(root); })->setObjectName(QStringLiteral("gitNewBranch"));
    if (!repo.operation.isEmpty())
        menu->addAction(tr("Abort the %1").arg(repo.operation), this, [this, root] { abortOperation(root); })
            ->setObjectName(QStringLiteral("gitAbort"));
}

void Commands::fillBranchMenu(QMenu* menu, const QString& root)
{
    const QList<Branch> all = branches(root);
    QMenu* remote = nullptr;
    for (const Branch& b : all) {
        QMenu* in = menu;
        if (b.remote) {
            if (remote == nullptr) {
                menu->addSeparator();
                remote = menu->addMenu(tr("Remote Branches"));
                remote->setObjectName(QStringLiteral("gitRemoteBranches"));
            }
            in = remote;
        }
        QString text = b.name;
        if (b.ahead > 0) text += QStringLiteral(" ↑%1").arg(b.ahead);
        if (b.behind > 0) text += QStringLiteral(" ↓%1").arg(b.behind);
        QAction* a = in->addAction(text, this, [this, root, name = b.name] { switchBranch(root, name); });
        a->setCheckable(true);
        a->setChecked(b.current);
        a->setEnabled(!b.current);
        a->setData(b.name);
        a->setToolTip(b.subject);
    }
    if (all.isEmpty()) menu->addAction(tr("No branches yet: a first commit makes one"))->setEnabled(false);
}

void Commands::fillStashMenu(QMenu* menu, const QString& root)
{
    const QList<Stash> all = stashes(root);
    for (const Stash& s : all) {
        QMenu* one = menu->addMenu(QStringLiteral("%1: %2").arg(s.index).arg(s.message));
        one->addAction(tr("Apply"), this, [this, root, i = s.index] { applyStash(root, i, false); });
        one->addAction(tr("Apply and Drop"), this, [this, root, i = s.index] { applyStash(root, i, true); });
        one->addAction(tr("Show"), this, [this, root, i = s.index] { showStash(root, i); });
        one->addSeparator();
        one->addAction(tr("Drop…"), this, [this, root, i = s.index] { dropStash(root, i); });
    }
    if (all.isEmpty()) menu->addAction(tr("No stashes"))->setEnabled(false);
}

// What they do

void Commands::commit(const QString& root)
{
    for (const QPointer<QDialog>& d : std::as_const(a_windows)) {
        auto* open = qobject_cast<CommitDialog*>(d.data());
        if (open != nullptr && open->root() == root) {
            open->reload();
            open->raise();
            open->activateWindow();
            return;
        }
    }
    auto* d = new CommitDialog(root, window());
    connect(d, &CommitDialog::openRequested, this, &Commands::openRequested);
    keep(d);
}

void Commands::stage(const QString& root, const QStringList& paths)
{
    if (report(git::stage(root, paths), tr("Stage")))
        done(root, paths.isEmpty() ? tr("Staged all the changes") : tr("Staged %1").arg(namesOf(paths)));
}

void Commands::unstage(const QString& root, const QStringList& paths)
{
    if (report(git::unstage(root, paths), tr("Unstage")))
        done(root, paths.isEmpty() ? tr("Unstaged all the changes") : tr("Unstaged %1").arg(namesOf(paths)));
}

void Commands::discard(const QString& root, const QStringList& paths)
{
    QMessageBox box(QMessageBox::Warning, tr("Discard Changes"),
                    (paths.isEmpty() ? tr("Throw away every change not committed?")
                                     : tr("Throw away the changes of %1?").arg(namesOf(paths)))
                        + QStringLiteral("\n\n")
                        + tr("Files git knows go back to their last commit (what was changed in them is lost); new files go to "
                             "the trash."),
                    QMessageBox::NoButton, window());
    box.setObjectName(QStringLiteral("gitDiscardQuestion"));
    QPushButton* yes = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != yes) return;
    QStringList trashed;
    if (report(git::discard(root, paths, &trashed), tr("Discard Changes")))
        done(root, paths.isEmpty() ? tr("Discarded every change") : tr("Discarded the changes of %1").arg(namesOf(paths)));
}

void Commands::showDiff(const QString& root, const QString& path, DiffOf of)
{
    const QString lines = diff(root, path, of);
    const QString parts = lines.trimmed().isEmpty() ? QString() : schematicChangesOf(root, path, of);
    const QString text = parts.isEmpty() ? lines : parts + QLatin1Char('\n') + lines;
    if (text.trimmed().isEmpty()) {
        emit message(tr("No changes"));
        return;
    }
    keep(new TextDialog(path.isEmpty() ? tr("Changes — %1").arg(QFileInfo(root).fileName())
                                       : tr("Changes of %1").arg(QFileInfo(path).fileName()),
                        text, window()));
}

HistoryDialog* Commands::showHistory(const QString& root, const QString& path)
{
    // One window a repository (a file), as an IDE has one history view.
    const QString wanted = QDir::cleanPath(HistoryDialog::historyPath(root, path));
    for (const QPointer<QDialog>& w : std::as_const(a_windows))
        if (auto* h = qobject_cast<HistoryDialog*>(w.data());
            h != nullptr && h->isVisible() && QDir::cleanPath(h->root()) == QDir::cleanPath(root) && QDir::cleanPath(h->path()) == wanted) {
            h->refresh();
            h->raise();
            h->activateWindow();
            return h;
        }
    auto* h = new HistoryDialog(root, path, window());
    keep(h);
    return h;
}

void Commands::showBlame(const QString& root, const QString& path)
{
    keep(new BlameDialog(root, path, window()));
}

void Commands::ignore(const QString& root, const QString& path)
{
    QString why;
    if (!git::ignore(root, path, &why)) {
        misc::reportError(tr("Add to .gitignore") + QStringLiteral(":\n") + why);
        return;
    }
    done(root, tr("%1 is in .gitignore").arg(QFileInfo(path).fileName()));
}

void Commands::untrack(const QString& root, const QStringList& paths)
{
    if (report(git::untrack(root, paths), tr("Stop Tracking")))
        done(root, tr("git no longer follows %1 (the files stay)").arg(namesOf(paths)));
}

void Commands::fetch(const QString& root)
{
    runJob(root, fetchArgs(), tr("Fetching"), [this, root](const Result& r) {
        if (report(r, tr("Fetch"))) done(root, tr("Fetched"));
    });
}

void Commands::pull(const QString& root)
{
    runJob(root, pullArgs(), tr("Pulling"), [this, root](const Result& r) {
        if (report(r, tr("Pull"))) done(root, tr("Pulled"));
    });
}

void Commands::push(const QString& root)
{
    const Repository repo = read(root);
    QString why;
    const QStringList args = pushArgs(repo, &why);
    if (args.isEmpty()) {
        misc::reportError(tr("Push") + QStringLiteral(":\n") + why);
        return;
    }
    runJob(root, args, tr("Pushing"), [this, root](const Result& r) {
        if (report(r, tr("Push"))) done(root, tr("Pushed"));
    });
}

void Commands::newBranch(const QString& root, const QString& start)
{
    bool ok = false;
    const QString name = QInputDialog::getText(window(), tr("New Branch"),
                                               start.isEmpty() ? tr("A branch from the commit checked out, checked out then:")
                                                               : tr("A branch at %1, checked out then:").arg(start.left(7)),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) return;
    if (!run(root, {QStringLiteral("check-ref-format"), QStringLiteral("--branch"), name}, true, 15000).ok()) {
        misc::reportError(tr("%1 cannot be a branch's name (no spaces, no ~ ^ : ? * [ \\, not ending in .lock...).").arg(name));
        return;
    }
    if (report(createBranch(root, name, start, true), tr("New Branch"))) done(root, tr("On the new branch %1").arg(name));
}

void Commands::switchBranch(const QString& root, const QString& branch)
{
    Result r = switchTo(root, branch);
    if (!r.ok() && (r.err.contains(QLatin1String("would be overwritten")) || r.err.contains(QLatin1String("commit your changes")))) {
        QMessageBox box(QMessageBox::Question, tr("Switch to %1").arg(branch),
                        tr("Your changes would be overwritten by %1's files. Put them aside first (a stash), and switch?").arg(branch),
                        QMessageBox::NoButton, window());
        box.setObjectName(QStringLiteral("gitStashFirstQuestion"));
        QPushButton* yes = box.addButton(tr("Stash and Switch"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != yes) return;
        if (!report(git::stash(root, tr("Before switching to %1").arg(branch)), tr("Stash"))) return;
        r = switchTo(root, branch);
    }
    if (report(r, tr("Switch to %1").arg(branch))) done(root, tr("On %1").arg(branch));
}

void Commands::renameBranch(const QString& root)
{
    const Repository repo = read(root);
    if (repo.branch.isEmpty()) {
        misc::reportError(tr("No branch is checked out (HEAD is detached): there is none to rename."));
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(window(), tr("Rename Branch"), tr("The branch %1's new name:").arg(repo.branch),
                                               QLineEdit::Normal, repo.branch, &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == repo.branch) return;
    if (report(git::renameBranch(root, repo.branch, name), tr("Rename Branch"))) done(root, tr("The branch is %1 now").arg(name));
}

void Commands::deleteBranch(const QString& root)
{
    QStringList names;
    for (const Branch& b : branches(root))
        if (!b.remote && !b.current) names << b.name;
    if (names.isEmpty()) {
        misc::reportError(tr("There is no other local branch to delete."));
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getItem(window(), tr("Delete Branch"), tr("The branch to delete:"), names, 0, false, &ok);
    if (!ok || name.isEmpty()) return;
    Result r = git::deleteBranch(root, name, false);
    if (!r.ok() && r.err.contains(QLatin1String("not fully merged"))) {
        if (QMessageBox::warning(window(), tr("Delete Branch"),
                                 tr("%1 has commits no other branch has: they are lost with it. Delete it all the same?").arg(name),
                                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel)
            != QMessageBox::Discard)
            return;
        r = git::deleteBranch(root, name, true);
    }
    if (report(r, tr("Delete Branch"))) done(root, tr("Deleted the branch %1").arg(name));
}

void Commands::mergeInto(const QString& root)
{
    QStringList names;
    for (const Branch& b : branches(root))
        if (!b.current) names << b.name;
    if (names.isEmpty()) {
        misc::reportError(tr("There is no other branch to merge."));
        return;
    }
    const Repository repo = read(root);
    bool ok = false;
    const QString ref = QInputDialog::getItem(window(), tr("Merge"),
                                              tr("Merge into %1:").arg(repo.branch.isEmpty() ? tr("the commit checked out") : repo.branch),
                                              names, 0, false, &ok);
    if (!ok || ref.isEmpty()) return;
    runJob(root, {QStringLiteral("merge"), QStringLiteral("--no-edit"), ref}, tr("Merging"), [this, root, ref](const Result& r) {
        if (!r.ok() && (r.out.contains(QLatin1String("CONFLICT")) || r.err.contains(QLatin1String("CONFLICT")))) {
            misc::reportError(tr("Merging %1 left conflicts: resolve them (the files marked !), stage them, then commit - or Git > "
                                 "Abort the Merge.").arg(ref)
                              + QStringLiteral("\n\n") + r.error());
            done(root, {});
            return;
        }
        if (report(r, tr("Merge"))) done(root, tr("Merged %1").arg(ref));
    });
}

void Commands::stashChanges(const QString& root)
{
    bool ok = false;
    const QString message = QInputDialog::getText(window(), tr("Stash Changes"),
                                                  tr("Put every change aside (new files too), the files back to the last commit. "
                                                     "A note for it (or none):"),
                                                  QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    if (report(git::stash(root, message.trimmed(), true), tr("Stash Changes"))) done(root, tr("The changes are stashed"));
}

void Commands::applyStash(const QString& root, int index, bool pop)
{
    if (report(git::applyStash(root, index, pop), pop ? tr("Apply and Drop the Stash") : tr("Apply the Stash")))
        done(root, tr("The stash's changes are back"));
}

void Commands::dropStash(const QString& root, int index)
{
    if (QMessageBox::warning(window(), tr("Drop the Stash"), tr("Throw stash %1 away, its changes with it?").arg(index),
                             QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel)
        != QMessageBox::Discard)
        return;
    if (report(git::dropStash(root, index), tr("Drop the Stash"))) done(root, tr("Dropped the stash"));
}

void Commands::showStash(const QString& root, int index)
{
    keep(new TextDialog(tr("Stash %1").arg(index), git::showStash(root, index), window()));
}

void Commands::newTag(const QString& root, const QString& commit)
{
    bool ok = false;
    const QString name = QInputDialog::getText(window(), tr("New Tag"),
                                               commit.isEmpty() ? tr("A tag on the commit checked out:")
                                                                : tr("A tag on %1:").arg(commit.left(7)),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) return;
    const QString message = QInputDialog::getText(window(), tr("New Tag"), tr("A message for it (none: a plain tag):"), QLineEdit::Normal,
                                                  QString(), &ok);
    if (!ok) return;
    if (report(createTag(root, name, message.trimmed(), commit), tr("New Tag"))) done(root, tr("Tagged %1").arg(name));
}

void Commands::abortOperation(const QString& root)
{
    const Repository repo = read(root);
    if (repo.operation.isEmpty()) {
        emit message(tr("Nothing is under way"));
        return;
    }
    if (QMessageBox::question(window(), tr("Abort"),
                              tr("Give up the %1 under way? The files go back to how they were before it.").arg(repo.operation))
        != QMessageBox::Yes)
        return;
    if (report(git::abort(root, repo.operation), tr("Abort"))) done(root, tr("The %1 is given up").arg(repo.operation));
}

void Commands::resolveConflict(const QString& root, const QString& path, const QString& side)
{
    const QString name = QFileInfo(path).fileName();
    const bool mine = side == QLatin1String("ours");
    QMessageBox box(QMessageBox::Warning, tr("Resolve Conflict"),
                    mine ? tr("Keep your version of %1 - the branch's, as it was before the merge?").arg(name)
                         : tr("Take the version of %1 that was merged in?").arg(name),
                    QMessageBox::NoButton, window());
    box.setObjectName(QStringLiteral("gitResolveQuestion"));
    box.setInformativeText(tr("The file becomes that version whole, staged as resolved: the other side's changes to it, and "
                              "any edit made in the file since, are gone from it."));
    QPushButton* yes = box.addButton(mine ? tr("Keep Mine") : tr("Take Theirs"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != yes) return;
    if (report(git::resolve(root, path, side), tr("Resolve Conflict")))
        done(root, mine ? tr("%1: your version kept").arg(name) : tr("%1: theirs taken").arg(name));
}

QStringList Commands::openConflictVersions(const QString& root, const QString& path)
{
    const ConflictVersions v = conflictVersions(root, path);
    const QFileInfo info(path);
    if (!v.inConflict) {
        misc::reportError(tr("%1 is not in conflict.").arg(info.fileName()));
        return {};
    }
    // Beside it - its subcircuits, its dataset found as from it - named
    // for what they are (untracked: delete them once it is resolved).
    QStringList written;
    const auto put = [&](const std::optional<QString>& text, const QString& which) {
        if (!text.has_value()) return;
        const QString base = info.completeBaseName() + QStringLiteral(" (%1)").arg(which);
        QString target = info.dir().filePath(base + QLatin1Char('.') + info.suffix());
        for (int n = 2; QFileInfo::exists(target); ++n)
            target = info.dir().filePath(QStringLiteral("%1 (%2 %3).%4").arg(info.completeBaseName(), which).arg(n).arg(info.suffix()));
        QFile f(target);
        if (!f.open(QIODevice::WriteOnly) || f.write(text->toUtf8()) < 0) {
            misc::reportError(tr("%1 cannot be written: %2").arg(QDir::toNativeSeparators(target), f.errorString()));
            return;
        }
        written << QDir::cleanPath(target);
    };
    put(v.ours, tr("mine"));
    put(v.theirs, tr("theirs"));
    for (const QString& w : std::as_const(written)) emit openVersionRequested(w);
    if (!v.ours.has_value() || !v.theirs.has_value())
        emit message(!v.ours.has_value() ? tr("Your side deleted %1: theirs opened").arg(info.fileName())
                                         : tr("Their side deleted %1: yours opened").arg(info.fileName()));
    Tracker::instance()->refresh(root);
    return written;
}

QMenu* Commands::addResolveMenu(QMenu* menu, const QString& root, const QString& path)
{
    const Repository* repo = Tracker::instance()->fresh(path);
    const Entry* e = repo != nullptr ? repo->entryOf(relativePath(root, path)) : nullptr;
    if (e == nullptr || !e->conflicted) return nullptr;
    QMenu* resolve = menu->addMenu(tr("Resolve Conflict"));
    resolve->setObjectName(QStringLiteral("gitResolveMenu"));
    resolve->addAction(tr("Keep Mine"), this, [this, root, path] { resolveConflict(root, path, QStringLiteral("ours")); })
        ->setObjectName(QStringLiteral("gitKeepMine"));
    resolve->addAction(tr("Take Theirs"), this, [this, root, path] { resolveConflict(root, path, QStringLiteral("theirs")); })
        ->setObjectName(QStringLiteral("gitTakeTheirs"));
    resolve->addAction(tr("Open Both Versions"), this, [this, root, path] { openConflictVersions(root, path); })
        ->setObjectName(QStringLiteral("gitOpenBoth"));
    resolve->addSeparator();
    QAction* resolved = resolve->addAction(tr("Mark Resolved (Stage)"), this, [this, root, path] { stage(root, {path}); });
    resolved->setObjectName(QStringLiteral("gitMarkResolved"));
    resolved->setEnabled(!hasConflictMarkers(path));
    resolved->setToolTip(tr("Once the file holds what it should, with no conflict marks left"));
    return resolve;
}

void Commands::editRemotes(const QString& root)
{
    auto* d = new QDialog(window());
    d->setObjectName(QStringLiteral("gitRemotesDialog"));
    d->setWindowTitle(tr("Remotes — %1").arg(QFileInfo(root).fileName()));
    auto* l = new QVBoxLayout(d);
    auto* list = new QListWidget(d);
    list->setObjectName(QStringLiteral("gitRemotesList"));
    l->addWidget(list, 1);
    const auto fill = [list, root] {
        list->clear();
        for (const Remote& r : remotes(root)) {
            auto* item = new QListWidgetItem(QStringLiteral("%1   %2").arg(r.name, r.fetchUrl), list);
            item->setData(Qt::UserRole, r.name);
        }
    };
    fill();
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add…"), d);
    auto* remove = new QPushButton(tr("Remove"), d);
    auto* close = new QPushButton(tr("Close"), d);
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch(1);
    row->addWidget(close);
    l->addLayout(row);
    connect(close, &QPushButton::clicked, d, &QDialog::reject);
    connect(add, &QPushButton::clicked, d, [this, d, root, fill] {
        bool ok = false;
        const QString name = QInputDialog::getText(d, tr("Add a Remote"), tr("Its name:"), QLineEdit::Normal,
                                                   remotes(root).isEmpty() ? QStringLiteral("origin") : QString(), &ok)
                                 .trimmed();
        if (!ok || name.isEmpty()) return;
        const QString url = QInputDialog::getText(d, tr("Add a Remote"), tr("Its URL:"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || url.isEmpty()) return;
        if (report(addRemote(root, name, url), tr("Add a Remote"))) done(root, tr("Added the remote %1").arg(name));
        fill();
    });
    connect(remove, &QPushButton::clicked, d, [this, d, root, list, fill] {
        QListWidgetItem* item = list->currentItem();
        if (item == nullptr) return;
        const QString name = item->data(Qt::UserRole).toString();
        if (QMessageBox::question(d, tr("Remove the Remote"), tr("Remove the remote %1? Its branches go too (only here).").arg(name))
            != QMessageBox::Yes)
            return;
        if (report(removeRemote(root, name), tr("Remove the Remote"))) done(root, tr("Removed the remote %1").arg(name));
        fill();
    });
    d->resize(560, 300);
    keep(d);
}

void Commands::createRepository(const QString& folder)
{
    if (QMessageBox::question(window(), tr("Create Repository"),
                              tr("Make %1 a git repository? Its files are not committed yet: Commit does that.")
                                  .arg(QDir::toNativeSeparators(folder)))
        != QMessageBox::Yes)
        return;
    if (report(init(folder), tr("Create Repository"))) {
        Tracker::instance()->refresh(folder);
        emit message(tr("%1 is a git repository now").arg(QFileInfo(folder).fileName()));
    }
}

void Commands::cloneRepository(const QString& folder)
{
    bool ok = false;
    const QString url = QInputDialog::getText(window(), tr("Clone a Repository"),
                                              tr("The repository's URL (https://... or git@...): it is cloned into %1.")
                                                  .arg(QDir::toNativeSeparators(folder)),
                                              QLineEdit::Normal, QString(), &ok)
                            .trimmed();
    if (!ok || url.isEmpty()) return;
    QString name = url.section(QLatin1Char('/'), -1).section(QLatin1Char(':'), -1);
    if (name.endsWith(QLatin1String(".git"))) name.chop(4);
    if (name.isEmpty()) name = QStringLiteral("repository");
    QString target = QDir(folder).filePath(name);
    for (int n = 2; QFileInfo::exists(target); ++n) target = QDir(folder).filePath(QStringLiteral("%1-%2").arg(name).arg(n));
    runJob(folder, cloneArgs(url, target), tr("Cloning"), [this, target](const Result& r) {
        if (!report(r, tr("Clone"))) return;
        Tracker::instance()->refresh(target);
        emit showFolderRequested(target);
        emit message(tr("Cloned into %1").arg(QDir::toNativeSeparators(target)));
    });
}

} // namespace qucs_s::git
