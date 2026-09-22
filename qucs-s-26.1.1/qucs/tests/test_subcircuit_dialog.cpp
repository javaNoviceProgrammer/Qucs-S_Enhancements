/*
 * The subcircuit properties dialog (paintings/id_dialog.*, upstream
 * #1285): the prefix and the parameters of a subcircuit's symbol in one
 * table edited in its cells, rows added, removed and moved, the table
 * checked before it is written, Apply without closing, and the symbol
 * line the file keeps.
 */
#include <QtTest>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>

#include "config.h"
#include "main.h"
#include "paintings/id_text.h"
#include "paintings/id_dialog.h"

namespace {

// A symbol's ID text as a subcircuit file keeps it.
std::unique_ptr<ID_Text> symbol(const QString& line)
{
    auto t = std::make_unique<ID_Text>();
    if (!t->load(line)) return nullptr;
    return t;
}

const QString kLine = QStringLiteral(
    ".ID -20 14 AMP \"1=gain=10=voltage gain=real\" \"0=rin=1k=input resistance=\" \"1=n=2==integer\"");

QStringList rows(ID_Dialog& d)
{
    QStringList out;
    QTableWidget* t = d.table();
    for (int r = 0; r < t->rowCount(); ++r)
        out << QStringLiteral("%1|%2|%3|%4|%5")
                   .arg(t->item(r, ID_Dialog::ShowColumn)->checkState() == Qt::Checked ? "shown" : "hidden",
                        t->item(r, ID_Dialog::NameColumn)->text(), t->item(r, ID_Dialog::DefaultColumn)->text(),
                        t->item(r, ID_Dialog::TypeColumn)->text(), t->item(r, ID_Dialog::DescriptionColumn)->text());
    return out;
}

QPushButton* button(ID_Dialog& d, const QString& text)
{
    for (QPushButton* b : d.findChildren<QPushButton*>())
        if (b->text().remove('&') == text) return b;
    return nullptr;
}

QPushButton* standard(ID_Dialog& d, QDialogButtonBox::StandardButton which)
{
    return d.findChild<QDialogButtonBox*>()->button(which);
}

} // namespace

class TestSubcircuitDialog : public QObject
{
    Q_OBJECT

private slots:
    void theTableShowsTheParametersAsTheFileHasThem()
    {
        auto t = symbol(kLine);
        QVERIFY(t);
        ID_Dialog d(t.get());
        QCOMPARE(d.prefixEdit()->text(), QStringLiteral("AMP"));
        QCOMPARE(d.table()->columnCount(), int(ID_Dialog::ColumnCount));
        QCOMPARE(rows(d), QStringList({"shown|gain|10|real|voltage gain", "hidden|rin|1k||input resistance",
                                       "shown|n|2|integer|"}));
        // Edited in the cells: the show flag a check box, the rest text.
        QVERIFY(d.table()->item(0, ID_Dialog::ShowColumn)->flags() & Qt::ItemIsUserCheckable);
        QVERIFY(!(d.table()->item(0, ID_Dialog::ShowColumn)->flags() & Qt::ItemIsEditable));
        for (int c = ID_Dialog::NameColumn; c < ID_Dialog::ColumnCount; ++c)
            QVERIFY(d.table()->item(0, c)->flags() & Qt::ItemIsEditable);
    }

    void editedCellsReachTheSymbol()
    {
        auto t = symbol(kLine);
        ID_Dialog d(t.get());
        d.table()->item(0, ID_Dialog::DefaultColumn)->setText("20");
        d.table()->item(1, ID_Dialog::ShowColumn)->setCheckState(Qt::Checked);
        d.table()->item(1, ID_Dialog::TypeColumn)->setText("real");
        d.prefixEdit()->setText("OPA");
        standard(d, QDialogButtonBox::Ok)->click();
        QCOMPARE(d.result(), int(QDialog::Accepted));
        QCOMPARE(t->prefix, QStringLiteral("OPA"));
        QCOMPARE(t->save(), QStringLiteral(".ID -20 14 OPA \"1=gain=20=voltage gain=real\" "
                                           "\"1=rin=1k=input resistance=real\" \"1=n=2==integer\""));
        // What was written reads back the same.
        auto again = symbol(t->save());
        QVERIFY(again);
        QCOMPARE(again->save(), t->save());
    }

    void okWithoutAChangeIsNoChange()
    {
        auto t = symbol(kLine);
        const QString before = t->save();
        ID_Dialog d(t.get());
        standard(d, QDialogButtonBox::Ok)->click();
        QCOMPARE(d.result(), int(QDialog::Rejected));   // what ID_Text::Dialog() reports as unchanged
        QVERIFY(!d.applied());
        QCOMPARE(t->save(), before);
    }

    void rowsAreAddedRemovedAndMoved()
    {
        auto t = symbol(kLine);
        ID_Dialog d(t.get());
        d.show();
        // A new row has a name of its own and is being typed into.
        button(d, "Add")->click();
        QCOMPARE(d.table()->rowCount(), 4);
        QCOMPARE(d.table()->item(3, ID_Dialog::NameColumn)->text(), QStringLiteral("P1"));
        QCOMPARE(d.table()->currentRow(), 3);
        QVERIFY(d.table()->findChild<QLineEdit*>() != nullptr);   // the editor is open
        d.table()->setCurrentCell(3, ID_Dialog::NameColumn);
        button(d, "Add")->click();
        QCOMPARE(d.table()->item(4, ID_Dialog::NameColumn)->text(), QStringLiteral("P2"));

        // Move "n" to the top.
        d.table()->setCurrentCell(2, ID_Dialog::NameColumn);
        button(d, "Move Up")->click();
        button(d, "Move Up")->click();
        QVERIFY(!button(d, "Move Up")->isEnabled());   // it is first
        QCOMPARE(d.table()->item(0, ID_Dialog::NameColumn)->text(), QStringLiteral("n"));
        QCOMPARE(d.table()->item(0, ID_Dialog::TypeColumn)->text(), QStringLiteral("integer"));   // the row went whole
        d.table()->setCurrentCell(4, ID_Dialog::NameColumn);
        QVERIFY(!button(d, "Move Down")->isEnabled());   // it is last

        // Remove the two new ones.
        d.table()->clearSelection();
        d.table()->selectRow(3);
        d.table()->setRangeSelected(QTableWidgetSelectionRange(3, 0, 4, ID_Dialog::ColumnCount - 1), true);
        button(d, "Remove")->click();
        QCOMPARE(d.table()->rowCount(), 3);

        standard(d, QDialogButtonBox::Ok)->click();
        QCOMPARE(t->subParameters.size(), std::size_t(3));
        QCOMPARE(t->subParameters.front()->name, QStringLiteral("n=2"));
        QCOMPARE(t->subParameters.back()->name, QStringLiteral("rin=1k"));
    }

    void aTableThatIsNotRightIsNotWritten()
    {
        auto t = symbol(kLine);
        const QString before = t->save();
        ID_Dialog d(t.get());
        auto refused = [&](const char* why) {
            standard(d, QDialogButtonBox::Ok)->click();
            QVERIFY2(!d.messageLabel()->text().isEmpty(), why);
            QCOMPARE(t->save(), before);
            QVERIFY(d.result() != int(QDialog::Accepted));
        };
        QTableWidgetItem* name = d.table()->item(1, ID_Dialog::NameColumn);

        name->setText("gain");
        refused("a name twice");
        QVERIFY(d.messageLabel()->text().contains("twice"));
        QCOMPARE(d.table()->currentRow(), 1);   // the cell is shown
        QCOMPARE(d.table()->currentColumn(), int(ID_Dialog::NameColumn));

        name->setText("File");
        refused("the reserved name");
        name->setText("");
        refused("no name");
        name->setText("r in");
        refused("a space");
        name->setText("rin");

        d.table()->item(1, ID_Dialog::DefaultColumn)->setText("1\"k");
        refused("a quote in a default");
        d.table()->item(1, ID_Dialog::DefaultColumn)->setText("1k");
        d.table()->item(1, ID_Dialog::DescriptionColumn)->setText("choices [a,b]");
        refused("brackets in a description");
        d.table()->item(1, ID_Dialog::DescriptionColumn)->setText("input resistance");

        d.prefixEdit()->setText("");
        refused("no prefix");
        d.prefixEdit()->setText("AMP");

        d.table()->item(1, ID_Dialog::DefaultColumn)->setText("2k");   // now right
        standard(d, QDialogButtonBox::Ok)->click();
        QVERIFY(d.messageLabel()->text().isEmpty());
        QCOMPARE(d.result(), int(QDialog::Accepted));
        QVERIFY(t->save().contains("\"0=rin=2k=input resistance=\""));
    }

    void applyWritesAndStaysOpen()
    {
        auto t = symbol(kLine);
        auto d = std::make_unique<ID_Dialog>(t.get());
        d->show();
        d->table()->item(0, ID_Dialog::DefaultColumn)->setText("33");
        standard(*d, QDialogButtonBox::Apply)->click();
        QVERIFY(d->isVisible());
        QVERIFY(d->applied());
        QVERIFY(t->save().contains("\"1=gain=33="));
        // Cancel after Apply: the symbol keeps what was applied, and the
        // caller hears that it changed.
        standard(*d, QDialogButtonBox::Cancel)->click();
        QCOMPARE(t->subParameters.front()->name, QStringLiteral("gain=33"));

        // Through ID_Text::Dialog(), as the schematic calls it.
        d.reset();
        QTimer::singleShot(0, [] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* dlg = qobject_cast<ID_Dialog*>(w); dlg && dlg->isVisible()) {
                    dlg->table()->item(0, ID_Dialog::DefaultColumn)->setText("44");
                    dlg->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
                    dlg->reject();
                }
        });
        QVERIFY(t->Dialog(nullptr));
        QCOMPARE(t->subParameters.front()->name, QStringLiteral("gain=44"));
    }

    void theEditorsLetThroughWhatTheFieldMayHold()
    {
        auto t = symbol(kLine);
        ID_Dialog d(t.get());
        QTableWidget* table = d.table();
        // The editors the table opens on its cells.
        auto editor = [&](int column) {
            return std::unique_ptr<QWidget>(table->itemDelegate()->createEditor(
                table->viewport(), QStyleOptionViewItem(), table->model()->index(0, column)));
        };
        auto typed = [&](int column, const char* keys) {
            auto e = editor(column);
            auto* line = qobject_cast<QLineEdit*>(e.get());
            if (line == nullptr) return QStringLiteral("<no line edit>");
            QTest::keyClicks(line, keys);
            return line->text();
        };
        QCOMPARE(typed(ID_Dialog::DefaultColumn, "1\"0=k"), QStringLiteral("10k"));
        QCOMPARE(typed(ID_Dialog::NameColumn, "r in"), QStringLiteral("rin"));
        QCOMPARE(typed(ID_Dialog::DescriptionColumn, "a [b] = \"c\""), QStringLiteral("a b  c"));

        // The type: a list to choose from, or typed.
        auto e = editor(ID_Dialog::TypeColumn);
        auto* combo = qobject_cast<QComboBox*>(e.get());
        QVERIFY(combo != nullptr);
        QVERIFY(combo->isEditable());
        QStringList offered;
        for (int i = 0; i < combo->count(); ++i) offered << combo->itemText(i);
        QCOMPARE(offered, ID_Dialog::types());
        // It shows the cell's type, and writes back what is chosen.
        table->itemDelegate()->setEditorData(combo, table->model()->index(0, ID_Dialog::TypeColumn));
        QCOMPARE(combo->currentText(), QStringLiteral("real"));
        combo->setCurrentText("integer");
        table->itemDelegate()->setModelData(combo, table->model(), table->model()->index(0, ID_Dialog::TypeColumn));
        QCOMPARE(table->item(0, ID_Dialog::TypeColumn)->text(), QStringLiteral("integer"));
    }
};

QTEST_MAIN(TestSubcircuitDialog)
#include "test_subcircuit_dialog.moc"
