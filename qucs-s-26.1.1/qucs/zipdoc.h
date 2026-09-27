/*
 * zipdoc.h - a ZIP archive in a tab: its files and folders listed, opened,
 *            added, renamed, deleted and extracted, and the archive saved
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_ZIPDOC_H
#define QUCS_ZIPDOC_H

#include "qucsdoc.h"
#include "zipfile.h"

#include <QFrame>
#include <QHash>
#include <QStringList>

#include <memory>

class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QStandardItemModel;
class QTemporaryDir;
class QToolButton;
class QTreeView;
class QUndoStack;

/// A ZIP archive (.zip) in a tab, as Eclipse's zip editor has one: its
/// files and folders in a tree (or a list), each with its size, packed
/// size, ratio, time, packing and checksum. A file opens in Qucs-S - a
/// copy of it, followed: saved there, it is in the archive again -; files
/// and folders are added (from the menus, or dropped from anywhere),
/// folders made, entries renamed and deleted, and extracted (or dragged
/// out). Each change is a step to undo; the archive is written when it is
/// saved, the files not changed copied as they were packed.
///
/// Nothing in it is ever run: a file Qucs-S does not open itself is only
/// extracted. An entry named to leave the folder it is extracted into
/// ("../x", "/x") is refused.
class ZipDoc : public QFrame, public QucsDoc
{
    Q_OBJECT

public:
    /// A file or folder of the archive.
    struct Entry {
        qucs_s::zip::Item item;   ///< name, time, comment, attributes; for one read, how it is packed
        QByteArray raw;           ///< packed as the archive had it: written back as it is while unchanged
        QByteArray data;          ///< its bytes, once new or changed: packed anew
        bool changed = false;
    };

    /// The largest archive opened (it is read whole).
    static constexpr qint64 MaxArchiveFile = 1024ll * 1024 * 1024;

    ZipDoc(QucsApp* app, const QString& name);
    ~ZipDoc() override;

    // QucsDoc
    void setName(const QString& name) override;
    bool load() override;
    int save() override;
    bool writeTo(const QString& path) override;
    void becomeCurrent(bool) override;
    double zoomBy(double factor) override;
    void showNoZoom() override;

    const QList<Entry>& entries() const { return a_entries; }
    /// The names of its files and folders ("docs/", "docs/amp.sch"), in
    /// the archive's order.
    QStringList names() const;
    /// A file's bytes (inflated); empty, with \a error said, when it
    /// cannot be read (encrypted, packed in a way not read, damaged).
    QByteArray contents(const QString& name, QString* error = nullptr, bool* ok = nullptr) const;
    /// The archive as saving writes it.
    QByteArray archive() const;
    /// The archive's own comment (kept).
    QString comment() const { return a_comment; }

    /// What a name taken already does when files are added.
    enum class Clash { Refuse, Replace, Skip };
    // What the buttons do, without their questions - each one step to undo:
    /// Files, and folders with all in them, added into \a folder ("" the
    /// top, "docs/"). Returns the names added; \a error says what was not.
    QStringList add(const QStringList& paths, const QString& folder, Clash clash, QString* error = nullptr);
    /// The names \a paths would take in \a folder that the archive has.
    QStringList clashes(const QStringList& paths, const QString& folder) const;
    /// A folder \a name made in \a folder.
    bool makeFolder(const QString& folder, const QString& name, QString* error = nullptr);
    /// Files and folders removed (a folder with all in it).
    void remove(const QStringList& names);
    /// A file or folder renamed where it is (\a newName a name, not a path).
    bool rename(const QString& name, const QString& newName, QString* error = nullptr);
    /// Files and folders (every one when \a names is empty) written into
    /// \a dir under their names in the archive; a file there already
    /// overwritten when \a overwrite, else left. Returns the files written.
    QStringList extract(const QStringList& names, const QString& dir, bool overwrite, QString* error = nullptr);
    /// A file opened in Qucs-S: a copy in a folder of its own, followed.
    /// Returns the copy; empty (\a error said) for a file that is not one
    /// Qucs-S opens, or cannot be read.
    QString openEntry(const QString& name, QString* error = nullptr);
    /// A file's bytes set - as its copy saved sets them: one step to undo.
    void replaceContents(const QString& name, const QByteArray& data);

    QTreeView* view() const { return a_view; }
    QStandardItemModel* model() const { return a_model; }
    QUndoStack* undoStack() const { return a_undo; }
    QLineEdit* filterEdit() const { return a_filter; }
    QLabel* statusLabel() const { return a_status; }
    /// The names of the rows selected (a folder's ending in '/').
    QStringList selectedNames() const;
    /// Selects the rows of \a names.
    void select(const QStringList& names);
    bool treeMode() const { return a_tree; }
    void setTreeMode(bool on);
    void setFilter(const QString& text);
    /// The folder new files go into: the folder selected, or the selected
    /// file's; "" (the top) when nothing is.
    QString targetFolder() const;

    enum { NameRole = Qt::UserRole + 1 };   ///< on each row: the entry's name ("docs/" for a folder)

public slots:
    void undo();
    void redo();
    void selectAll();
    /// Files on the clipboard (copied in a file manager) added.
    void paste();
    void addFilesAsked();
    void addFolderAsked();
    void newFolderAsked();
    void extractAsked();
    void extractAllAsked();
    void deleteSelected();
    void renameSelected();
    void openSelected();
    void focusFilter();

signals:
    void signalFileChanged(bool);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class ZipSnapshot;
    friend class ZipView;
    /// Parses \a bytes (an archive); false, with \a error, for one not read.
    bool setFromArchive(const QByteArray& bytes, QString* error);
    void setEntries(const QList<Entry>& entries);   // (undo's)
    /// \a after in place of the entries: one step to undo.
    void change(const QList<Entry>& after, const QString& what);
    void rebuild();
    void updateStatus();
    /// Files and folders added, asking about names taken.
    void addAsked(const QStringList& paths, const QString& folder);
    /// \a chosen (all when empty) extracted into a folder asked for,
    /// asking about files there already.
    void extractInto(const QStringList& chosen);
    /// The selected files extracted into a folder of their own and dragged.
    void dragOut();
    void copyChanged(const QString& path);

    QList<Entry> a_entries;
    QString a_comment;
    QTreeView* a_view = nullptr;
    QStandardItemModel* a_model = nullptr;
    QLineEdit* a_filter = nullptr;
    QLabel* a_status = nullptr;
    QToolButton* a_modeButton = nullptr;
    QUndoStack* a_undo = nullptr;
    bool a_tree = true;
    QString a_filterText;
    double a_zoom = 1.0;
    QFont a_font;
    // The copies opened in Qucs-S: their paths, the entries' names.
    std::unique_ptr<QTemporaryDir> a_temp;
    QHash<QString, QString> a_opened;
    QFileSystemWatcher* a_watcher = nullptr;
};

#endif // QUCS_ZIPDOC_H
