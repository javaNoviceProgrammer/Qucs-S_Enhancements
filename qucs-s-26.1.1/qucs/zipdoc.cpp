/*
 * zipdoc.cpp - a ZIP archive in a tab: zipdoc.h
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "zipdoc.h"

#include "links.h"
#include "main.h"
#include "misc.h"
#include "qucs.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDirIterator>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

using qucs_s::zip::Item;

namespace {

enum { SortRole = ZipDoc::NameRole + 1 };

constexpr int kColumns = 7;

// A name an entry may have: '/' between its parts, none of them empty,
// "." or ".." (one that would leave the folder it is extracted into),
// not absolute, no drive. Empty for one that is not.
QString safeName(QString name)
{
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const bool folder = name.endsWith(QLatin1Char('/'));
    if (folder) name.chop(1);
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || (name.size() > 1 && name.at(1) == QLatin1Char(':')))
        return QString();
    for (const QString& part : name.split(QLatin1Char('/')))
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) return QString();
    return folder ? name + QLatin1Char('/') : name;
}

// A name, not a path: for a file or folder renamed, a folder made.
bool isPlainName(const QString& name)
{
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..") && !name.contains(QLatin1Char('/'))
           && !name.contains(QLatin1Char('\\'));
}

// "docs/" of "docs/amp.sch" and of "docs/sub/"; "" at the top.
QString parentOf(const QString& name)
{
    QString n = name;
    if (n.endsWith(QLatin1Char('/'))) n.chop(1);
    const qsizetype slash = n.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : n.left(slash + 1);
}

// Its last part: "amp.sch", "sub".
QString baseOf(const QString& name)
{
    QString n = name;
    if (n.endsWith(QLatin1Char('/'))) n.chop(1);
    return n.mid(n.lastIndexOf(QLatin1Char('/')) + 1);
}

QString normalizedFolder(QString folder)
{
    folder.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (folder.startsWith(QLatin1Char('/'))) folder.remove(0, 1);
    if (!folder.isEmpty() && !folder.endsWith(QLatin1Char('/'))) folder += QLatin1Char('/');
    return folder;
}

} // namespace

// ---------------------------------------------------------------------
// A step to undo: the entries before and after it.
class ZipSnapshot : public QUndoCommand
{
public:
    ZipSnapshot(ZipDoc* doc, QList<ZipDoc::Entry> before, QList<ZipDoc::Entry> after, const QString& what)
        : QUndoCommand(what), a_doc(doc), a_before(std::move(before)), a_after(std::move(after))
    {
    }
    void undo() override { a_doc->setEntries(a_before); }
    void redo() override { a_doc->setEntries(a_after); }

private:
    ZipDoc* a_doc;
    QList<ZipDoc::Entry> a_before, a_after;
};

// The tree, whose drags carry the entries out as files.
class ZipView : public QTreeView
{
public:
    ZipView(ZipDoc* doc) : QTreeView(doc), a_doc(doc) {}

protected:
    void startDrag(Qt::DropActions) override { a_doc->dragOut(); }

private:
    ZipDoc* a_doc;
};

// ---------------------------------------------------------------------
ZipDoc::ZipDoc(QucsApp* app, const QString& name) : QFrame(), QucsDoc(app, name)
{
    setObjectName(QStringLiteral("ZipDoc"));
    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(0, 0, 0, 0);
    all->setSpacing(0);

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(4, 3, 4, 3);
    bar->setSpacing(2);
    const struct {
        const char* name;
        const char* text;
        const char* tip;
        void (ZipDoc::*slot)();
    } buttons[] = {
        {"zipOpen", QT_TR_NOOP("Open"), QT_TR_NOOP("Open the files selected in Qucs-S (a copy: saved, it is in the archive again)"),
         &ZipDoc::openSelected},
        {"zipExtract", QT_TR_NOOP("Extract…"), QT_TR_NOOP("Write the files and folders selected into a folder"), &ZipDoc::extractAsked},
        {"zipExtractAll", QT_TR_NOOP("Extract All…"), QT_TR_NOOP("Write every file and folder into a folder"), &ZipDoc::extractAllAsked},
        {"zipAddFiles", QT_TR_NOOP("Add Files…"), QT_TR_NOOP("Add files into the folder selected (or drop them here)"), &ZipDoc::addFilesAsked},
        {"zipAddFolder", QT_TR_NOOP("Add Folder…"), QT_TR_NOOP("Add a folder with all in it into the folder selected"), &ZipDoc::addFolderAsked},
        {"zipNewFolder", QT_TR_NOOP("New Folder"), QT_TR_NOOP("Make a folder in the folder selected"), &ZipDoc::newFolderAsked},
        {"zipRename", QT_TR_NOOP("Rename…"), QT_TR_NOOP("Rename the file or folder selected (F2)"), &ZipDoc::renameSelected},
        {"zipDelete", QT_TR_NOOP("Delete"), QT_TR_NOOP("Take the files and folders selected out of the archive (Delete)"),
         &ZipDoc::deleteSelected},
    };
    for (const auto& b : buttons) {
        auto* button = new QToolButton(this);
        button->setObjectName(QLatin1String(b.name));
        button->setText(tr(b.text));
        button->setToolTip(tr(b.tip));
        button->setAutoRaise(true);
        connect(button, &QToolButton::clicked, this, b.slot);
        bar->addWidget(button);
    }
    bar->addSpacing(8);
    a_modeButton = new QToolButton(this);
    a_modeButton->setObjectName(QStringLiteral("zipMode"));
    a_modeButton->setAutoRaise(true);
    a_modeButton->setCheckable(true);
    a_modeButton->setChecked(true);
    a_modeButton->setText(tr("Tree"));
    a_modeButton->setToolTip(tr("Folders as a tree, or every entry in a list by its path"));
    connect(a_modeButton, &QToolButton::toggled, this, &ZipDoc::setTreeMode);
    bar->addWidget(a_modeButton);
    bar->addStretch(1);
    a_filter = new QLineEdit(this);
    a_filter->setObjectName(QStringLiteral("zipFilter"));
    a_filter->setPlaceholderText(tr("Filter by name"));
    a_filter->setClearButtonEnabled(true);
    a_filter->setMaximumWidth(220);
    connect(a_filter, &QLineEdit::textChanged, this, &ZipDoc::setFilter);
    bar->addWidget(a_filter);
    all->addLayout(bar);

    a_model = new QStandardItemModel(this);
    a_model->setSortRole(SortRole);
    a_view = new ZipView(this);
    a_view->setObjectName(QStringLiteral("zipView"));
    a_view->setModel(a_model);
    a_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    a_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    a_view->setUniformRowHeights(true);
    a_view->setFrameShape(QFrame::NoFrame);
    a_view->setDragEnabled(true);
    a_view->setAcceptDrops(true);
    a_view->viewport()->setAcceptDrops(true);
    a_view->setDragDropMode(QAbstractItemView::DragDrop);
    a_view->setDropIndicatorShown(false);
    a_view->installEventFilter(this);
    a_view->viewport()->installEventFilter(this);
    connect(a_view, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!index.sibling(index.row(), 0).data(NameRole).toString().endsWith(QLatin1Char('/'))) openSelected();
    });
    all->addWidget(a_view, 1);

    a_status = new QLabel(this);
    a_status->setObjectName(QStringLiteral("zipStatus"));
    a_status->setContentsMargins(6, 2, 6, 3);
    all->addWidget(a_status);

    a_undo = new QUndoStack(this);
    connect(a_undo, &QUndoStack::cleanChanged, this, [this](bool clean) {
        setDocChanged(!clean);
        emit signalFileChanged(!clean);
        updateStatus();
    });
    const auto front = [this] {
        return a_App != nullptr && a_App->DocumentTab != nullptr && a_App->DocumentTab->currentWidget() == this;
    };
    connect(a_undo, &QUndoStack::canUndoChanged, this, [this, front](bool can) {
        if (front()) a_App->undo->setEnabled(can);
    });
    connect(a_undo, &QUndoStack::canRedoChanged, this, [this, front](bool can) {
        if (front()) a_App->redo->setEnabled(can);
    });
    if (app != nullptr) connect(this, SIGNAL(signalFileChanged(bool)), app, SLOT(slotFileChanged(bool)));

    a_watcher = new QFileSystemWatcher(this);
    connect(a_watcher, &QFileSystemWatcher::fileChanged, this, &ZipDoc::copyChanged);
    a_font = a_view->font();
    rebuild();
}

ZipDoc::~ZipDoc()
{
    // The stack clears as it goes and says it is clean: not to the label
    // and the rows, which are gone first.
    a_undo->disconnect(this);
}

void ZipDoc::setName(const QString& name)
{
    a_DocName = name;
}

// ---------------------------------------------------------------------
bool ZipDoc::setFromArchive(const QByteArray& bytes, QString* error)
{
    QList<Entry> entries;
    QString comment;
    if (!bytes.isEmpty()) {   // (an empty file: an archive with nothing in it yet)
        QString why;
        const QList<Item> items = qucs_s::zip::list(bytes, &why);
        if (!why.isEmpty()) {
            if (error != nullptr) *error = why;
            return false;
        }
        for (const Item& item : items) {
            Entry e;
            e.item = item;
            e.raw = qucs_s::zip::packedData(bytes, item);
            entries.append(e);
        }
        comment = qucs_s::zip::comment(bytes);
    }
    a_entries = entries;
    a_comment = comment;
    rebuild();
    return true;
}

bool ZipDoc::load()
{
    QFile file(a_DocName);
    if (!file.open(QIODevice::ReadOnly)) {
        misc::reportError(tr("Cannot read %1:\n%2").arg(QDir::toNativeSeparators(a_DocName), file.errorString()));
        return false;
    }
    if (file.size() > MaxArchiveFile) {
        misc::reportError(tr("%1 is larger than an archive opened here (%2 MB at most).")
                              .arg(QDir::toNativeSeparators(a_DocName))
                              .arg(MaxArchiveFile / (1024 * 1024)));
        return false;
    }
    QString why;
    if (!setFromArchive(file.readAll(), &why)) {
        misc::reportError(tr("Cannot read %1:\n%2").arg(QDir::toNativeSeparators(a_DocName), why));
        return false;
    }
    a_undo->clear();
    a_undo->setClean();
    setDocChanged(false);
    a_lastSaved = QFileInfo(a_DocName).lastModified();
    // The folders at the top open.
    for (int row = 0; row < a_model->rowCount(); ++row) a_view->expand(a_model->index(row, 0));
    return true;
}

QByteArray ZipDoc::archive() const
{
    QList<qucs_s::zip::Part> parts;
    for (const Entry& e : a_entries) {
        qucs_s::zip::Part part;
        part.item = e.item;
        if (e.item.folder()) {
            // (nothing in it)
        } else if (!e.changed && e.item.dataOffset >= 0) {
            part.copied = true;   // packed as it was
            part.raw = e.raw;
        } else {
            part.data = e.data;
        }
        parts.append(part);
    }
    return qucs_s::zip::write(parts, a_comment);
}

bool ZipDoc::writeTo(const QString& path)
{
    const QByteArray bytes = archive();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

int ZipDoc::save()
{
    const QByteArray bytes = archive();
    QSaveFile file(a_DocName);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        misc::reportError(tr("Cannot write %1:\n%2").arg(QDir::toNativeSeparators(a_DocName), file.errorString()));
        return -1;
    }
    // Read again as written: every file packed, copied as it is from now.
    const QStringList selected = selectedNames();
    QString why;
    setFromArchive(bytes, &why);
    select(selected);
    a_undo->setClean();
    setDocChanged(false);
    emit signalFileChanged(false);
    a_lastSaved = QFileInfo(a_DocName).lastModified();
    return 0;
}

void ZipDoc::becomeCurrent(bool)
{
    if (a_App != nullptr) {
        a_App->undo->setEnabled(a_undo->canUndo());
        a_App->redo->setEnabled(a_undo->canRedo());
    }
    a_view->setFocus();
}

double ZipDoc::zoomBy(double factor)
{
    a_zoom = std::clamp(a_zoom * (factor > 1 ? 1.25 : factor < 1 ? 1 / 1.25 : 1.0), 0.5, 4.0);
    QFont f = a_font;
    if (f.pointSizeF() > 0) f.setPointSizeF(f.pointSizeF() * a_zoom);
    a_view->setFont(f);
    return a_zoom;
}

void ZipDoc::showNoZoom()
{
    a_zoom = 1.0;
    a_view->setFont(a_font);
}

// ---------------------------------------------------------------------
QStringList ZipDoc::names() const
{
    QStringList list;
    for (const Entry& e : a_entries) list << e.item.name;
    return list;
}

QByteArray ZipDoc::contents(const QString& name, QString* error, bool* ok) const
{
    for (const Entry& e : a_entries) {
        if (e.item.name != name || e.item.folder()) continue;
        if (e.changed || e.item.dataOffset < 0) {
            if (ok != nullptr) *ok = true;
            return e.data;
        }
        Item alone = e.item;   // its packed bytes alone: they start at 0
        alone.dataOffset = 0;
        return qucs_s::zip::extract(e.raw, alone, error, ok);
    }
    if (error != nullptr) *error = tr("The archive has no file %1.").arg(name);
    if (ok != nullptr) *ok = false;
    return QByteArray();
}

void ZipDoc::setEntries(const QList<Entry>& entries)
{
    const QStringList selected = selectedNames();
    a_entries = entries;
    rebuild();
    select(selected);
}

void ZipDoc::change(const QList<Entry>& after, const QString& what)
{
    a_undo->push(new ZipSnapshot(this, a_entries, after, what));
}

QStringList ZipDoc::clashes(const QStringList& paths, const QString& folderGiven) const
{
    const QString folder = normalizedFolder(folderGiven);
    const QStringList have = names();
    QStringList found;
    for (const QString& path : paths) {
        const QFileInfo fi(path);
        if (fi.isDir() && !fi.isSymLink()) {
            QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                const QString name = folder + fi.fileName() + QLatin1Char('/') + QDir(path).relativeFilePath(it.filePath());
                if (have.contains(name)) found << name;
            }
        } else if (have.contains(folder + fi.fileName())) {
            found << folder + fi.fileName();
        }
    }
    return found;
}

QStringList ZipDoc::add(const QStringList& paths, const QString& folderGiven, Clash clash, QString* error)
{
    const QString folder = normalizedFolder(folderGiven);
    QList<Entry> after = a_entries;
    QHash<QString, int> index;
    for (int i = 0; i < after.size(); ++i) index.insert(after.at(i).item.name, i);
    QStringList added, problems;
    const auto put = [&](const QString& nameGiven, const QFileInfo& fi) {
        const bool isFolder = nameGiven.endsWith(QLatin1Char('/'));
        const QString name = safeName(nameGiven);
        if (name.isEmpty()) {
            problems << tr("%1: not a name an archive's file may have").arg(nameGiven);
            return;
        }
        const auto have = index.constFind(name);
        if (have != index.cend()) {
            if (isFolder) return;   // there already
            if (clash == Clash::Skip) return;
            if (clash == Clash::Refuse) {
                problems << tr("%1: the archive has one of that name").arg(name);
                return;
            }
        }
        Entry e = have != index.cend() ? after.at(*have) : Entry();
        e.item.name = name;
        e.item.modified = fi.lastModified();
        e.item.comment = have != index.cend() ? e.item.comment : QString();
#ifndef Q_OS_WIN
        // Made on Unix: its mode (a script's execute bit) kept for who unpacks it.
        const QFileDevice::Permissions p = fi.permissions();
        quint32 mode = (isFolder ? 0040000u : 0100000u) | ((p & QFileDevice::ReadOwner) ? 0400u : 0u)
                       | ((p & QFileDevice::WriteOwner) ? 0200u : 0u) | ((p & QFileDevice::ExeOwner) ? 0100u : 0u)
                       | ((p & QFileDevice::ReadGroup) ? 040u : 0u) | ((p & QFileDevice::ExeGroup) ? 010u : 0u)
                       | ((p & QFileDevice::ReadOther) ? 04u : 0u) | ((p & QFileDevice::ExeOther) ? 01u : 0u);
        e.item.madeBy = quint16((3 << 8) | 20);
        e.item.externalAttributes = (mode << 16) | (isFolder ? 0x10u : 0u);
#else
        e.item.madeBy = 20;
        e.item.externalAttributes = isFolder ? 0x10u : 0u;
#endif
        if (!isFolder) {
            if (fi.size() > qint64(qucs_s::zip::MaxEntrySize)) {
                problems << tr("%1: larger than a file of an archive written here (%2 MB)")
                                .arg(name)
                                .arg(qucs_s::zip::MaxEntrySize / (1024 * 1024));
                return;
            }
            QFile f(fi.filePath());
            if (!f.open(QIODevice::ReadOnly)) {
                problems << tr("%1: %2").arg(QDir::toNativeSeparators(fi.filePath()), f.errorString());
                return;
            }
            e.data = f.readAll();
            e.raw.clear();
            e.changed = true;
            e.item.method = 8;
            e.item.flags = 0;
            e.item.dataOffset = -1;
        }
        if (have != index.cend()) {
            after[*have] = e;
        } else {
            index.insert(name, int(after.size()));
            after.append(e);
        }
        added << name;
    };
    for (const QString& path : paths) {
        const QFileInfo fi(path);
        if (!fi.exists() || fi.isSymLink()) {
            problems << tr("%1: not a file or folder").arg(QDir::toNativeSeparators(path));
            continue;
        }
        if (!fi.isDir()) {
            put(folder + fi.fileName(), fi);
            continue;
        }
        // A folder, with all in it (links not followed).
        const QString top = folder + fi.fileName() + QLatin1Char('/');
        put(top, fi);
        QDirIterator it(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        QList<QFileInfo> inside;
        while (it.hasNext()) {
            it.next();
            if (!it.fileInfo().isSymLink()) inside << it.fileInfo();
        }
        std::sort(inside.begin(), inside.end(), [](const QFileInfo& a, const QFileInfo& b) { return a.filePath() < b.filePath(); });
        for (const QFileInfo& f : inside) {
            const QString relative = QDir(path).relativeFilePath(f.filePath());
            put(top + relative + (f.isDir() ? QStringLiteral("/") : QString()), f);
        }
    }
    if (error != nullptr) *error = problems.join(QLatin1Char('\n'));
    if (!added.isEmpty())
        change(after, added.size() == 1 ? tr("Add %1").arg(baseOf(added.first())) : tr("Add %1 entries").arg(added.size()));
    return added;
}

bool ZipDoc::makeFolder(const QString& folderGiven, const QString& name, QString* error)
{
    const QString folder = normalizedFolder(folderGiven);
    if (!isPlainName(name)) {
        if (error != nullptr) *error = tr("“%1” is not a name for a folder.").arg(name);
        return false;
    }
    const QString made = safeName(folder + name + QLatin1Char('/'));
    if (made.isEmpty()) {
        if (error != nullptr) *error = tr("“%1” is not a name for a folder.").arg(name);
        return false;
    }
    const QStringList have = names();
    if (std::any_of(have.cbegin(), have.cend(),
                    [&](const QString& n) { return n == made.chopped(1) || n.startsWith(made); })) {
        if (error != nullptr) *error = tr("The archive has %1 already.").arg(made);
        return false;
    }
    QList<Entry> after = a_entries;
    Entry e;
    e.item.name = made;
    e.item.modified = QDateTime::currentDateTime();
    e.item.externalAttributes = 0x10;
    after.append(e);
    change(after, tr("New Folder %1").arg(name));
    return true;
}

void ZipDoc::remove(const QStringList& chosen)
{
    if (chosen.isEmpty()) return;
    QList<Entry> after;
    for (const Entry& e : a_entries) {
        const QString& n = e.item.name;
        const bool gone = std::any_of(chosen.cbegin(), chosen.cend(), [&n](const QString& c) {
            return n == c || (c.endsWith(QLatin1Char('/')) && n.startsWith(c));
        });
        if (!gone) after.append(e);
    }
    if (after.size() == a_entries.size()) return;
    change(after, chosen.size() == 1 ? tr("Delete %1").arg(baseOf(chosen.first())) : tr("Delete %1 entries").arg(chosen.size()));
}

bool ZipDoc::rename(const QString& name, const QString& newName, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (!isPlainName(newName)) return fail(tr("“%1” is not a name: it holds no “/” and is not “.” or “..”.").arg(newName));
    const bool folder = name.endsWith(QLatin1Char('/'));
    const QString target = parentOf(name) + newName + (folder ? QStringLiteral("/") : QString());
    if (target == name) return true;
    const QStringList have = names();
    const QString plain = folder ? target.chopped(1) : target;
    const bool taken = std::any_of(have.cbegin(), have.cend(), [&](const QString& n) {
        return (n == plain || n == plain + QLatin1Char('/') || n.startsWith(plain + QLatin1Char('/')))
               && !(folder ? n.startsWith(name) : n == name);
    });
    if (taken) return fail(tr("The archive has %1 already.").arg(target));
    QList<Entry> after = a_entries;
    bool any = false;
    for (Entry& e : after) {
        if (folder ? e.item.name.startsWith(name) : e.item.name == name) {
            e.item.name = target + e.item.name.mid(name.size());
            any = true;
        }
    }
    if (!any) return fail(tr("The archive has no %1.").arg(name));
    // The copies opened follow.
    for (auto it = a_opened.begin(); it != a_opened.end(); ++it)
        if (folder ? it.value().startsWith(name) : it.value() == name) it.value() = target + it.value().mid(name.size());
    change(after, tr("Rename %1 to %2").arg(baseOf(name), newName));
    return true;
}

QStringList ZipDoc::extract(const QStringList& chosen, const QString& dir, bool overwrite, QString* error)
{
    QStringList written, problems;
    const QString root = QDir::cleanPath(QDir(dir).absolutePath());
    if (!QDir().mkpath(root)) {
        if (error != nullptr) *error = tr("%1 cannot be made.").arg(QDir::toNativeSeparators(root));
        return written;
    }
    for (const Entry& e : a_entries) {
        const QString& n = e.item.name;
        if (!chosen.isEmpty() && !std::any_of(chosen.cbegin(), chosen.cend(), [&n](const QString& c) {
                return n == c || (c.endsWith(QLatin1Char('/')) && n.startsWith(c));
            }))
            continue;
        // Never out of the folder: "../x", "/x", "C:x" are refused.
        const QString name = safeName(n);
        const QString target = QDir::cleanPath(root + QLatin1Char('/') + name);
        if (name.isEmpty() || !target.startsWith(root + QLatin1Char('/'))) {
            problems << tr("%1: a name that would leave the folder - not extracted").arg(n);
            continue;
        }
        if (e.item.folder()) {
            QDir().mkpath(target);
            continue;
        }
        if (QFileInfo::exists(target) && !overwrite) {
            problems << tr("%1: there already - left as it is").arg(QDir::toNativeSeparators(target));
            continue;
        }
        QString why;
        bool ok = false;
        const QByteArray data = contents(n, &why, &ok);
        if (!ok) {
            problems << why;
            continue;
        }
        QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            problems << tr("%1: %2").arg(QDir::toNativeSeparators(target), file.errorString());
            continue;
        }
        if (e.item.modified.isValid()) {
            QFile touched(target);
            if (touched.open(QIODevice::Append)) touched.setFileTime(e.item.modified, QFileDevice::FileModificationTime);
        }
        written << target;
    }
    if (error != nullptr) *error = problems.join(QLatin1Char('\n'));
    return written;
}

QString ZipDoc::openEntry(const QString& name, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return QString();
    };
    const QString safe = safeName(name);
    if (safe.isEmpty() || safe.endsWith(QLatin1Char('/'))) return fail(tr("%1 is not a file that can be opened.").arg(name));
    // Nothing of an archive is run: what Qucs-S does not open itself is
    // extracted, to be opened elsewhere.
    if (!qucs_s::links::opensItself(safe))
        return fail(tr("“%1” is not a document Qucs-S opens: extract it to open it elsewhere (nothing in an archive is "
                       "run from here).")
                        .arg(baseOf(safe)));
    QString why;
    bool ok = false;
    const QByteArray data = contents(name, &why, &ok);
    if (!ok) return fail(why);
    if (!a_temp) a_temp = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/qucs-s-zip-XXXXXX"));
    if (!a_temp->isValid()) return fail(tr("No folder for a copy: %1").arg(a_temp->errorString()));
    const QString path = QDir::cleanPath(a_temp->path() + QLatin1Char('/') + safe);
    // A copy open already (perhaps with unsaved changes): left as it is.
    bool open = false;
    if (a_opened.contains(path) && a_App != nullptr) {
        const QList<QucsDoc*> docs = a_App->allDocuments();
        open = std::any_of(docs.cbegin(), docs.cend(), [&path](QucsDoc* d) { return d->getDocName() == path; });
    }
    if (!open) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
            return fail(tr("%1: %2").arg(QDir::toNativeSeparators(path), file.errorString()));
        a_opened.insert(path, name);
        if (!a_watcher->files().contains(path)) a_watcher->addPath(path);
    }
    if (a_App != nullptr) a_App->gotoPage(path);
    return path;
}

void ZipDoc::replaceContents(const QString& name, const QByteArray& data)
{
    QList<Entry> after = a_entries;
    for (Entry& e : after) {
        if (e.item.name != name || e.item.folder()) continue;
        e.data = data;
        e.raw.clear();
        e.changed = true;
        e.item.method = 8;
        e.item.flags = 0;
        e.item.dataOffset = -1;
        e.item.modified = QDateTime::currentDateTime();
        change(after, tr("Change %1").arg(baseOf(name)));
        return;
    }
}

void ZipDoc::copyChanged(const QString& path)
{
    // A moment for the writer to finish (and a file written anew to be
    // there again: its watch is set once more).
    QTimer::singleShot(150, this, [this, path] {
        if (!a_opened.contains(path)) return;
        if (QFileInfo::exists(path) && !a_watcher->files().contains(path)) a_watcher->addPath(path);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        const QByteArray bytes = file.readAll();
        const QString name = a_opened.value(path);
        QStatusBar* bar = a_App != nullptr ? a_App->statusBar() : nullptr;
        if (!names().contains(name)) {
            if (bar != nullptr) bar->showMessage(tr("%1 is no longer in %2.").arg(baseOf(name), QFileInfo(a_DocName).fileName()), 6000);
            return;
        }
        bool ok = false;
        if (contents(name, nullptr, &ok) == bytes && ok) return;
        replaceContents(name, bytes);
        if (bar != nullptr)
            bar->showMessage(tr("%1 changed: it is in %2 again - save the archive to keep it.")
                                 .arg(baseOf(name), QFileInfo(a_DocName).fileName()),
                             8000);
    });
}

// ---------------------------------------------------------------------
// The rows: a tree of folders (or a list of paths), filtered.

void ZipDoc::rebuild()
{
    // What was open and selected stays so.
    QStringList expanded;
    std::function<void(const QModelIndex&)> collect = [&](const QModelIndex& parent) {
        for (int row = 0; row < a_model->rowCount(parent); ++row) {
            const QModelIndex i = a_model->index(row, 0, parent);
            if (a_view->isExpanded(i)) expanded << i.data(NameRole).toString();
            collect(i);
        }
    };
    collect(QModelIndex());
    const QStringList selected = selectedNames();

    a_model->clear();
    a_model->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Packed"), tr("Ratio"), tr("Modified"), tr("Method"), tr("CRC-32")});
    const QLocale locale;
    static const QIcon folderIcon = QFileIconProvider().icon(QFileIconProvider::Folder);
    static const QIcon fileIcon = QFileIconProvider().icon(QFileIconProvider::File);
    const QString filter = a_filterText.trimmed();
    const auto shown = [&filter](const QString& name) { return filter.isEmpty() || name.contains(filter, Qt::CaseInsensitive); };

    struct Totals {
        qint64 size = 0, packed = 0;
        bool packedKnown = true;
    };
    QHash<QStandardItem*, Totals> totals;
    const auto cells = [&](const QString& name, bool folder) {
        QList<QStandardItem*> row;
        auto* first = new QStandardItem(folder ? folderIcon : fileIcon, a_tree ? baseOf(name) : name);
        first->setData(name, NameRole);
        first->setData(QString(folder ? QStringLiteral("0") : QStringLiteral("1")) + (a_tree ? baseOf(name) : name).toLower(), SortRole);
        row << first;
        for (int c = 1; c < kColumns; ++c) row << new QStandardItem();
        for (QStandardItem* i : row) i->setEditable(false);
        return row;
    };
    QHash<QString, QStandardItem*> folders;   // (tree) by name
    std::function<QStandardItem*(const QString&)> folderItem = [&](const QString& name) -> QStandardItem* {
        if (name.isEmpty()) return a_model->invisibleRootItem();
        if (QStandardItem* f = folders.value(name)) return f;
        QStandardItem* parent = folderItem(parentOf(name));
        const QList<QStandardItem*> row = cells(name, true);
        parent->appendRow(row);
        folders.insert(name, row.first());
        return row.first();
    };
    for (const Entry& e : a_entries) {
        const QString& name = e.item.name;
        const bool folder = e.item.folder();
        if (!shown(name)) continue;
        if (folder) {
            if (a_tree) folderItem(name);
            else a_model->appendRow(cells(name, true));
            continue;
        }
        const QList<QStandardItem*> row = cells(name, false);
        (a_tree ? folderItem(parentOf(name)) : a_model->invisibleRootItem())->appendRow(row);
        const qint64 size = e.changed || e.item.dataOffset < 0 ? e.data.size() : qint64(e.item.size);
        row.at(1)->setText(locale.toString(size));
        row.at(1)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        const bool packed = !e.changed && e.item.dataOffset >= 0;
        if (packed) {
            row.at(2)->setText(locale.toString(qint64(e.item.packed)));
            row.at(2)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            // The space saved, rounded down (99%, not 100%, for a file that still takes some).
            if (size > 0) row.at(3)->setText(QStringLiteral("%1%").arg(std::max<qint64>(0, (size - qint64(e.item.packed)) * 100 / size)));
            row.at(3)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row.at(6)->setText(QStringLiteral("%1").arg(e.item.crc, 8, 16, QLatin1Char('0')).toUpper());
        } else {
            row.at(2)->setText(QStringLiteral("—"));
            row.at(2)->setToolTip(tr("Packed when the archive is saved"));
            row.at(2)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }
        if (e.item.modified.isValid()) row.at(4)->setText(e.item.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        row.at(5)->setText(e.item.encrypted() ? tr("encrypted")
                           : !packed           ? tr("new or changed")
                           : e.item.method == 8 ? tr("Deflated")
                           : e.item.method == 0 ? tr("Stored")
                                                : tr("method %1").arg(e.item.method));
        if (!e.item.comment.isEmpty()) row.first()->setToolTip(e.item.comment);
        // Its folders count it.
        if (a_tree)
            for (QStandardItem* f = row.first()->parent(); f != nullptr; f = f->parent()) {
                Totals& t = totals[f];
                t.size += size;
                if (packed) t.packed += e.item.packed;
                else t.packedKnown = false;
            }
    }
    for (auto it = totals.cbegin(); it != totals.cend(); ++it) {
        QStandardItem* f = it.key();
        QStandardItem* parent = f->parent() != nullptr ? f->parent() : a_model->invisibleRootItem();
        parent->child(f->row(), 1)->setText(locale.toString(it->size));
        parent->child(f->row(), 1)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (it->packedKnown) {
            parent->child(f->row(), 2)->setText(locale.toString(it->packed));
            parent->child(f->row(), 2)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    a_model->sort(0);
    a_view->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    for (int c = 1; c < kColumns; ++c) a_view->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    if (a_view->header()->sectionSize(0) < 240) a_view->header()->resizeSection(0, 280);
    a_view->setRootIsDecorated(a_tree);

    std::function<void(const QModelIndex&)> restore = [&](const QModelIndex& parent) {
        for (int row = 0; row < a_model->rowCount(parent); ++row) {
            const QModelIndex i = a_model->index(row, 0, parent);
            if (expanded.contains(i.data(NameRole).toString()) || !filter.isEmpty()) a_view->expand(i);
            restore(i);
        }
    };
    restore(QModelIndex());
    select(selected);
    updateStatus();
}

void ZipDoc::updateStatus()
{
    int files = 0, folders = 0;
    qint64 size = 0, packed = 0;
    bool changed = false;
    QSet<QString> folderNames;
    for (const Entry& e : a_entries) {
        if (e.item.folder()) {
            folderNames.insert(e.item.name);
            continue;
        }
        ++files;
        for (QString p = parentOf(e.item.name); !p.isEmpty(); p = parentOf(p)) folderNames.insert(p);
        const bool isPacked = !e.changed && e.item.dataOffset >= 0;
        size += isPacked ? e.item.size : e.data.size();
        if (isPacked) packed += e.item.packed;
        changed = changed || !isPacked;
    }
    folders = int(folderNames.size());
    const QLocale locale;
    QString text = (files == 1 ? tr("1 file") : tr("%1 files").arg(locale.toString(files))) + QStringLiteral(", ")
                   + (folders == 1 ? tr("1 folder") : tr("%1 folders").arg(locale.toString(folders))) + QStringLiteral(" · ")
                   + locale.formattedDataSize(size);
    if (!changed && size > 0)
        text += QStringLiteral(" · ") + tr("packed %1 (%2% saved)").arg(locale.formattedDataSize(packed)).arg(std::max<qint64>(0, (size - packed) * 100 / size));
    if (getDocChanged()) text += QStringLiteral(" · ") + tr("changed: save to write the archive");
    a_status->setText(text);
    if (!a_comment.isEmpty()) a_status->setToolTip(tr("The archive's comment: %1").arg(a_comment));
}

// ---------------------------------------------------------------------
QStringList ZipDoc::selectedNames() const
{
    QStringList list;
    if (a_view == nullptr || a_view->selectionModel() == nullptr) return list;
    for (const QModelIndex& i : a_view->selectionModel()->selectedRows(0)) {
        const QString name = i.data(NameRole).toString();
        if (!name.isEmpty() && !list.contains(name)) list << name;
    }
    return list;
}

void ZipDoc::select(const QStringList& chosen)
{
    if (a_view->selectionModel() == nullptr) return;
    a_view->selectionModel()->clearSelection();
    bool first = true;
    std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int row = 0; row < a_model->rowCount(parent); ++row) {
            const QModelIndex i = a_model->index(row, 0, parent);
            if (chosen.contains(i.data(NameRole).toString())) {
                a_view->selectionModel()->select(i, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                if (first) a_view->selectionModel()->setCurrentIndex(i, QItemSelectionModel::NoUpdate);
                first = false;
            }
            walk(i);
        }
    };
    walk(QModelIndex());
}

QString ZipDoc::targetFolder() const
{
    const QModelIndex at = a_view->currentIndex();
    if (!at.isValid() || !a_view->selectionModel()->isSelected(at.sibling(at.row(), 0))) return QString();
    const QString name = at.sibling(at.row(), 0).data(NameRole).toString();
    return name.endsWith(QLatin1Char('/')) ? name : parentOf(name);
}

void ZipDoc::setTreeMode(bool on)
{
    if (a_tree == on) return;
    a_tree = on;
    const QSignalBlocker block(a_modeButton);
    a_modeButton->setChecked(on);
    a_modeButton->setText(on ? tr("Tree") : tr("List"));
    rebuild();
}

void ZipDoc::setFilter(const QString& text)
{
    if (a_filterText == text) return;
    a_filterText = text;
    if (a_filter->text() != text) a_filter->setText(text);
    rebuild();
}

// ---------------------------------------------------------------------
// The buttons and keys: the operations above, with their questions.

void ZipDoc::undo()
{
    a_undo->undo();
}

void ZipDoc::redo()
{
    a_undo->redo();
}

void ZipDoc::selectAll()
{
    a_view->selectAll();
}

void ZipDoc::focusFilter()
{
    a_filter->setFocus();
    a_filter->selectAll();
}

void ZipDoc::paste()
{
    QStringList files;
    if (const QMimeData* mime = QApplication::clipboard()->mimeData())
        for (const QUrl& url : mime->urls())
            if (url.isLocalFile()) files << url.toLocalFile();
    if (!files.isEmpty()) addAsked(files, targetFolder());
}

void ZipDoc::addAsked(const QStringList& paths, const QString& folder)
{
    Clash clash = Clash::Refuse;
    const QStringList taken = clashes(paths, folder);
    if (!taken.isEmpty()) {
        QMessageBox box(QMessageBox::Question, tr("Add to Archive"),
                        taken.size() == 1 ? tr("The archive has one of these already.")
                                          : tr("The archive has %1 of these already.").arg(taken.size()),
                        QMessageBox::NoButton, this);
        box.setObjectName(QStringLiteral("zipClash"));
        box.setInformativeText(taken.mid(0, 8).join(QLatin1Char('\n')) + (taken.size() > 8 ? QStringLiteral("\n…") : QString()));
        QPushButton* replace = box.addButton(tr("Replace"), QMessageBox::DestructiveRole);
        replace->setObjectName(QStringLiteral("zipClashReplace"));
        QPushButton* skip = box.addButton(tr("Skip Them"), QMessageBox::AcceptRole);
        skip->setObjectName(QStringLiteral("zipClashSkip"));
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(skip);
        box.setEscapeButton(cancel);
        box.exec();
        if (box.clickedButton() == replace) clash = Clash::Replace;
        else if (box.clickedButton() == skip) clash = Clash::Skip;
        else return;
    }
    QString why;
    const QStringList added = add(paths, folder, clash, &why);
    if (!why.isEmpty()) QMessageBox::warning(this, tr("Add to Archive"), why);
    if (!added.isEmpty()) select(added);
}

void ZipDoc::addFilesAsked()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add Files to %1").arg(QFileInfo(a_DocName).fileName()),
                                                            QFileInfo(a_DocName).absolutePath());
    if (!files.isEmpty()) addAsked(files, targetFolder());
}

void ZipDoc::addFolderAsked()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Add a Folder to %1").arg(QFileInfo(a_DocName).fileName()),
                                                             QFileInfo(a_DocName).absolutePath());
    if (!folder.isEmpty()) addAsked({folder}, targetFolder());
}

void ZipDoc::newFolderAsked()
{
    const QString in = targetFolder();
    QString suggested = tr("New Folder");
    for (int n = 2; names().contains(in + suggested + QLatin1Char('/')); ++n) suggested = tr("New Folder %1").arg(n);
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Folder"), in.isEmpty() ? tr("The new folder's name:")
                                                                                   : tr("The name of the new folder in %1:").arg(in),
                                               QLineEdit::Normal, suggested, &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) return;
    QString why;
    if (!makeFolder(in, name, &why)) QMessageBox::warning(this, tr("New Folder"), why);
    else select({in + name + QLatin1Char('/')});
}

void ZipDoc::extractAllAsked()
{
    extractInto({});
}

void ZipDoc::extractAsked()
{
    extractInto(selectedNames());   // (nothing selected: all)
}

void ZipDoc::extractInto(const QStringList& chosen)
{
    const QString archiveName = QFileInfo(a_DocName).fileName();
    const QString dir = QFileDialog::getExistingDirectory(
        this, chosen.isEmpty() ? tr("Extract All of %1 to").arg(archiveName) : tr("Extract to"), QFileInfo(a_DocName).absolutePath());
    if (dir.isEmpty()) return;
    QStringList there;
    for (const Entry& e : a_entries) {
        const QString& n = e.item.name;
        const bool in = chosen.isEmpty() || std::any_of(chosen.cbegin(), chosen.cend(), [&n](const QString& c) {
                            return n == c || (c.endsWith(QLatin1Char('/')) && n.startsWith(c));
                        });
        if (in && !e.item.folder() && QFileInfo::exists(QDir(dir).filePath(n))) there << n;
    }
    bool replace = false;
    if (!there.isEmpty()) {
        const auto answer = QMessageBox::question(this, tr("Extract"),
                                                  there.size() == 1 ? tr("%1 is there already. Overwrite it?").arg(there.first())
                                                                    : tr("%1 files are there already. Overwrite them?").arg(there.size()),
                                                  QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::No);
        if (answer == QMessageBox::Cancel) return;
        replace = answer == QMessageBox::Yes;
    }
    QString why;
    const QStringList written = extract(chosen, dir, replace, &why);
    if (!why.isEmpty()) QMessageBox::warning(this, tr("Extract"), why);
    if (a_App != nullptr)
        a_App->statusBar()->showMessage(tr("%1 extracted to %2")
                                            .arg(written.size() == 1 ? tr("1 file") : tr("%1 files").arg(written.size()),
                                                 QDir::toNativeSeparators(dir)),
                                        6000);
}

void ZipDoc::deleteSelected()
{
    remove(selectedNames());
}

void ZipDoc::renameSelected()
{
    const QStringList chosen = selectedNames();
    if (chosen.size() != 1) return;
    const QString name = chosen.first();
    bool ok = false;
    const QString now = QInputDialog::getText(this, tr("Rename"), tr("The new name of “%1”:").arg(baseOf(name)), QLineEdit::Normal,
                                              baseOf(name), &ok)
                            .trimmed();
    if (!ok || now.isEmpty() || now == baseOf(name)) return;
    QString why;
    if (!rename(name, now, &why)) QMessageBox::warning(this, tr("Rename"), why);
    else select({parentOf(name) + now + (name.endsWith(QLatin1Char('/')) ? QStringLiteral("/") : QString())});
}

void ZipDoc::openSelected()
{
    QStringList problems;
    int opened = 0;
    for (const QString& name : selectedNames()) {
        if (name.endsWith(QLatin1Char('/'))) continue;
        if (++opened > 12) break;   // (not a hundred tabs from one click)
        QString why;
        if (openEntry(name, &why).isEmpty()) problems << why;
    }
    if (!problems.isEmpty()) QMessageBox::information(this, tr("Open"), problems.join(QLatin1Char('\n')));
}

void ZipDoc::dragOut()
{
    const QStringList chosen = selectedNames();
    if (chosen.isEmpty()) return;
    if (!a_temp) a_temp = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/qucs-s-zip-XXXXXX"));
    if (!a_temp->isValid()) return;
    static int drags = 0;
    const QString dir = a_temp->path() + QStringLiteral("/drag-%1").arg(++drags);
    QString why;
    extract(chosen, dir, true, &why);
    QList<QUrl> urls;
    for (const QString& name : chosen) {
        // Those whose folder is not dragged too.
        if (std::any_of(chosen.cbegin(), chosen.cend(), [&name](const QString& c) {
                return c != name && c.endsWith(QLatin1Char('/')) && name.startsWith(c);
            }))
            continue;
        QString n = safeName(name);
        if (n.endsWith(QLatin1Char('/'))) n.chop(1);
        const QString path = QDir(dir).filePath(n);
        if (!n.isEmpty() && QFileInfo::exists(path)) urls << QUrl::fromLocalFile(path);
    }
    if (urls.isEmpty()) return;
    auto* mime = new QMimeData;
    mime->setUrls(urls);
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction);
}

bool ZipDoc::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == a_view && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete || (key->key() == Qt::Key_Backspace && key->modifiers() & Qt::ControlModifier)) {
            deleteSelected();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            openSelected();
            return true;
        }
        if (key->key() == Qt::Key_F2) {
            renameSelected();
            return true;
        }
    }
    if (watched == a_view->viewport()) {
        // Files dropped from anywhere (not the rows' own drag): into the
        // folder under them.
        const auto files = [](const QMimeData* mime) {
            QStringList list;
            if (mime != nullptr)
                for (const QUrl& url : mime->urls())
                    if (url.isLocalFile()) list << url.toLocalFile();
            return list;
        };
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* drag = static_cast<QDropEvent*>(event);
            if (drag->source() != a_view && !files(drag->mimeData()).isEmpty()) {
                drag->setDropAction(Qt::CopyAction);
                drag->accept();
            } else {
                drag->ignore();
            }
            return true;
        }
        if (event->type() == QEvent::Drop) {
            auto* drop = static_cast<QDropEvent*>(event);
            const QStringList paths = files(drop->mimeData());
            if (drop->source() == a_view || paths.isEmpty()) {
                drop->ignore();
                return true;
            }
            const QModelIndex at = a_view->indexAt(drop->position().toPoint());
            QString folder;
            if (at.isValid()) {
                const QString name = at.sibling(at.row(), 0).data(NameRole).toString();
                folder = name.endsWith(QLatin1Char('/')) ? name : parentOf(name);
            }
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            // After the drop has ended (the question below waits).
            QTimer::singleShot(0, this, [this, paths, folder] { addAsked(paths, folder); });
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}
