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

class QComboBox;
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
class QTreeWidgetItem;

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
 * \brief The figures scripts draw (matplotlib, Python > Plots
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
    /// The insides of the value \a expression is (the shell's answer):
    /// under its row - opened again after each command, as it was.
    void showChildren(const QString& expression, const QJsonArray& items);
    QTreeWidget* view() const { return a_view; }
    /// The rows shown: "name: type [size] = value" - those opened under
    /// their own, indented.
    QStringList rows() const;
    /// The row of \a expression (nullptr: none shown).
    QTreeWidgetItem* rowOf(const QString& expression) const;

signals:
    /// A value as a table, in a data display, its insides: by its
    /// expression (a name, or name['key'], name[3], name.attribute).
    void tableRequested(const QString& expression);
    void displayRequested(const QString& expression);
    void childrenRequested(const QString& expression);
    void refreshRequested();

private:
    void filter();
    void fill(QTreeWidgetItem* parent, const QJsonArray& items);

    QLineEdit* a_filter = nullptr;
    QTreeWidget* a_view = nullptr;
    QLabel* a_note = nullptr;
    QSet<QString> a_opened;   // the expressions opened: opened again
};

/// The rows of a value, fetched as they are scrolled to: the fetch asks for
/// those of a request ({"start", "count"} - and the view's "sort",
/// "descending", "filters", "token"); answer() gives them.
class PythonTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    using Fetch = std::function<void(const QJsonObject& request)>;
    explicit PythonTableModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}
    void setFetch(Fetch fetch) { a_fetch = std::move(fetch); }
    /// How its rows are asked for: sorted by \a sort (-1: as they are),
    /// those the filters keep ({column: "> 5" | text}) - fetched again from
    /// the first.
    void setView(int sort, bool descending, const QJsonObject& filters);
    int sortColumn() const { return a_sort; }
    bool descending() const { return a_descending; }
    QJsonObject filters() const { return a_filters; }
    /// The rows before the filters.
    int total() const { return a_total; }
    /// Complex numbers shown as they are (a+bj), as magnitude and phase
    /// (degrees), dB and phase, or one part: the real, the imaginary, the
    /// magnitude, dB (20 log10), the phase.
    enum ComplexMode { AsIs, MagnitudePhase, DbPhase, Real, Imaginary, Magnitude, Decibel, Phase };
    void setComplexMode(int mode);
    int complexMode() const { return a_complex; }
    /// A cell's number (a complex one's as the mode has it, its magnitude
    /// when it shows two) - NaN for none.
    double number(int row, int column) const;
    /// A row's label (its index), as text.
    QString rowLabel(int row) const;
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
    int a_sort = -1;
    bool a_descending = false;
    QJsonObject a_filters;
    int a_token = 0;                      // the view: an answer of another left aside
    int a_total = 0;
    int a_complex = AsIs;
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

    /// Its columns \a columns plotted (fetched first): the first the x of
    /// the rest when there are two or more, else over the rows' labels (or
    /// their numbers) - on a log x axis when x is positive over two
    /// decades; complex values as the complex mode has them.
    void plotColumns(const QList<int>& columns);
    /// Filters: a column's rule ("> 5", ">= 5", "< 5", "== 5", "!= 5", or
    /// text it has; empty: none); sorted by a column (a click on its
    /// header: up, down, as it was).
    void setFilter(int column, const QString& rule);
    void sortBy(int column, bool descending);

public slots:
    void copy();
    void exportAs();
    /// The columns selected plotted (plotColumns()).
    void plotSelected();

signals:
    void exported();
    /// A plot of its columns made: the picture (twice its size, for a
    /// dense screen) and what it is.
    void plotted(const QImage& picture, const QString& title);

private:
    void showFilters();

    QString a_name;
    PythonTableModel* a_model = nullptr;
    QTableView* a_view = nullptr;
    QLabel* a_about = nullptr;
    QLabel* a_filtersShown = nullptr;
    QComboBox* a_filterColumn = nullptr;
    QLineEdit* a_filterRule = nullptr;
    QComboBox* a_complexMode = nullptr;
    QString a_exportPath;
    QList<int> a_plotColumns;   // to plot when every row is in
};

namespace qucs_s::python {
/// A plot of series over \a x, drawn (axes, grid, a legend for more than
/// one): \a scale its pixels a point's.
QImage plotImage(const QString& title, const QString& xName, const QVector<double>& x,
                 const QList<std::pair<QString, QVector<double>>>& series, bool logX, qreal scale = 2);
} // namespace qucs_s::python

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
