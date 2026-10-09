/*
 * pythonviews.h - what the Python editor shows besides a script: the
 *                 figures a script draws (Python Plots), the Python
 *                 Shell's variables (Python Variables), a value's rows
 *                 (the Data Viewer), Go to Symbol, a breakpoint's dialog -
 *                 and the folders through which scripts reach Qucs-S
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PYTHONVIEWS_H
#define QUCS_PYTHONVIEWS_H

#include "pythondoc.h"

#include <QAbstractTableModel>
#include <QFrame>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QWidget>

#include <functional>

class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QTableView;
class QTimer;
class QToolButton;
class QTreeWidget;

namespace qucs_s::python {

/// A breakpoint edited in a dialog: its condition, its hits, a logpoint's
/// message, on or off (\a field focused: 0, 1, 2). False when cancelled.
/// \a existing: it is there already (the dialog says Edit, not Add).
bool editBreakpointDialog(QWidget* parent, Breakpoint* b, int field, bool existing);
/// A script's Run Settings edited in a dialog: its arguments, working
/// folder, environment variables and .env file. False when cancelled.
bool editRunSettingsDialog(QWidget* parent, const QString& script, RunSettings* settings);

/*!
 * \brief The folders of moduleFolder() through which scripts reach Qucs-S,
 *        watched: a figure written into plots/ (plotArrived()), the Python
 *        Shell's variables written into shell/variables.json
 *        (variablesChanged()) and its answers in shell/answers/
 *        (shellAnswered()), qucs.display()'s requests in requests/
 *        (displayRequested()).
 */
class Exchange : public QObject
{
    Q_OBJECT

public:
    explicit Exchange(QObject* parent = nullptr);
    /// Asks the Python Shell (its thread of answers, _qucs_shell.py): the
    /// request's id, its answer's.
    int askShell(QJsonObject request);
    /// Looks at the folders now (they are watched; this as well after a
    /// run).
    void scan();

signals:
    /// \a image (a PNG) and what it is: {"source", "figure", "title",
    /// "time", "scale"}.
    void plotArrived(const QString& image, const QJsonObject& about);
    void variablesChanged(const QJsonArray& variables);
    void shellAnswered(const QJsonObject& answer);
    void displayRequested(const QJsonObject& request);

private:
    QFileSystemWatcher* a_watcher = nullptr;
    QTimer* a_poll = nullptr;   // (a watcher missing a change: looked at again)
    QSet<QString> a_plotsSeen;
    QByteArray a_variables;     // the last read
    int a_questions = 0;
};

} // namespace qucs_s::python

/*!
 * \brief The figures scripts draw (matplotlib, Simulation > Python > Plots
 *        in Qucs-S): the one chosen, fitted to the pane or at its own size,
 *        a strip of them all below; saved as a picture, copied, removed.
 */
class PythonPlotsPane : public QWidget
{
    Q_OBJECT

public:
    explicit PythonPlotsPane(QWidget* parent = nullptr);

    /// The newest last; chosen.
    void addPlot(const QString& image, const QJsonObject& about);
    int count() const { return int(a_plots.size()); }
    int current() const { return a_current; }
    void choose(int index);
    /// What the one chosen is ("s.py, figure 1: square").
    QString title(int index) const;
    QString imagePath(int index) const;
    /// The picture as shown (fitted, or at its own size).
    QPixmap shown() const;
    bool fitted() const { return a_fit; }
    void setFitted(bool on);
    /// Its picture written to \a path (PNG, JPEG, BMP by the suffix).
    bool save(int index, const QString& path) const;

    /// The most kept: the oldest go.
    static constexpr int kMost = 200;

public slots:
    void remove(int index);
    void removeAll();
    void copy();
    void saveAs();

signals:
    void countChanged(int count);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void showCurrent();

    struct Plot {
        QString image;
        QJsonObject about;
    };
    QList<Plot> a_plots;
    int a_current = -1;
    bool a_fit = true;
    QLabel* a_title = nullptr;
    QLabel* a_view = nullptr;
    QScrollArea* a_scroll = nullptr;
    QListWidget* a_strip = nullptr;
    QToolButton* a_previous = nullptr;
    QToolButton* a_next = nullptr;
    QToolButton* a_fitButton = nullptr;
    QToolButton* a_save = nullptr;
    QToolButton* a_copy = nullptr;
    QToolButton* a_remove = nullptr;
    QToolButton* a_removeAll = nullptr;
    QLabel* a_empty = nullptr;
};

/*!
 * \brief The Python Shell's variables, after each command it runs: their
 *        names, types, sizes and values; one shown as a table (an array, a
 *        list, a dictionary of columns, a DataFrame, a qucs.Dataset) with a
 *        double-click - tableRequested().
 */
class PythonVariablesPane : public QWidget
{
    Q_OBJECT

public:
    explicit PythonVariablesPane(QWidget* parent = nullptr);
    void setVariables(const QJsonArray& variables);
    QTreeWidget* view() const { return a_view; }
    /// The rows shown: "name: type [size] = value".
    QStringList rows() const;

signals:
    void tableRequested(const QString& name);
    void refreshRequested();

private:
    void filter();

    QLineEdit* a_filter = nullptr;
    QTreeWidget* a_view = nullptr;
    QLabel* a_note = nullptr;
};

/// The rows of a value, fetched as they are scrolled to: \a ask fetches
/// those from start (count of them); answer() gives them.
class PythonTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    using Fetch = std::function<void(int start, int count)>;
    explicit PythonTableModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}
    void setFetch(Fetch fetch) { a_fetch = std::move(fetch); }
    /// An answer of _qucs_data.table(): the shape (the first), the rows.
    void answer(const QJsonObject& table);
    /// The first rows asked for - its shape with them.
    void fetchFirst() { ask(0); }
    /// The rows not yet fetched, asked for (Export, Copy All).
    void fetchAll();
    bool complete() const;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    /// A cell as text (a number with every digit).
    QString text(int row, int column) const;
    static constexpr int kBlock = 500;

signals:
    void shapeKnown();
    void allFetched();

private:
    void ask(int block) const;

    Fetch a_fetch;
    int a_rows = 0;
    int a_columns = 0;
    QStringList a_names;
    QHash<int, QJsonArray> a_blocks;      // a block's rows
    QHash<int, QJsonArray> a_index;       // a block's row labels
    mutable QSet<int> a_asked;
    bool a_wantAll = false;
};

/*!
 * \brief A value's rows in a table (Python Variables' View as Table, the
 *        debugger's): its name, type and shape above, every row fetched as
 *        it is scrolled to; Copy, Export as CSV.
 */
class PythonDataViewer : public QWidget
{
    Q_OBJECT

public:
    PythonDataViewer(const QString& name, PythonTableModel::Fetch fetch, QWidget* parent = nullptr);
    void answer(const QJsonObject& table);
    PythonTableModel* model() const { return a_model; }
    QTableView* view() const { return a_view; }
    QString name() const { return a_name; }
    /// The rows as CSV (every row: they are fetched first).
    bool exportTo(const QString& path);
    /// The cells selected - every one when none is - as tab-separated text.
    QString selectedText() const;

public slots:
    void copy();
    void exportAs();

signals:
    void exported();

private:
    QString a_name;
    PythonTableModel* a_model = nullptr;
    QTableView* a_view = nullptr;
    QLabel* a_about = nullptr;
    QString a_exportPath;
};

/*!
 * \brief Go to Symbol: the script's classes, functions, methods and
 *        variables - and those of the scripts beside it - picked by typing
 *        their letters; chosen() goes there.
 */
class PythonSymbolPicker : public QFrame
{
    Q_OBJECT

public:
    struct Entry {
        QString file;   // empty: the script's
        qucs_s::python::Symbol symbol;
    };
    PythonSymbolPicker(const QList<Entry>& entries, QWidget* parent);
    QLineEdit* filterLine() const { return a_filter; }
    QListWidget* list() const { return a_list; }
    /// The entries shown, "name" or "Class.name" each, the best first.
    QStringList shownNames() const;

signals:
    void chosen(const QString& file, int line, int column);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void filter();
    void choose();

    QList<Entry> a_entries;
    QLineEdit* a_filter = nullptr;
    QListWidget* a_list = nullptr;
};

#endif // QUCS_PYTHONVIEWS_H
