/*
 * The PDF viewer (pdfdoc.h): a PDF document opened in a tab of its own -
 * one tab for one file - its pages laid out to the width, drawn, gone
 * through by page, zoomed as the View menu and the toolbar do, fitted;
 * its text searched, the matches gone through, selected (a word, a page)
 * and copied; its links followed; read again when the file changes; its
 * pages at the side; saved under another name; a file that is not a PDF
 * refused; and every command of the Edit, Positioning, Insert, Simulation
 * and View menus, and the document's commands of the File menu, used with
 * it in front - none of them may take it for a schematic.
 */
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfLink>
#include <QPdfLinkModel>
#include <QPdfSearchModel>
#include <QPdfWriter>
#include <QPrinter>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTreeView>

#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "pdfdoc.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"

using qucs_s::pdf::PageView;

namespace {

// A PDF of \a pages pages: "Page n" at the top of each, "needle" twice on
// the third, a link on the first. Written by Qt, as a report would be.
bool writePdf(const QString& path, int pages)
{
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setTitle(QStringLiteral("The probe"));
    QTextDocument doc;
    QFont font(QStringLiteral("Helvetica"));
    font.setPointSize(28);
    doc.setDefaultFont(font);
    QString html = QStringLiteral("<p>Page 1. Hello world, see <a href='https://example.org/datasheet'>the datasheet</a>.</p>");
    for (int i = 2; i <= pages; ++i) {
        html += QStringLiteral("<p style='page-break-before:always'>Page %1.</p>").arg(i);
        if (i == 3) html += QStringLiteral("<p>Here is the needle, and another needle.</p>");
    }
    doc.setHtml(html);
    doc.print(&writer);
    return QFileInfo(path).size() > 0;
}

// Whatever dialog comes up is closed: the commands are used, not answered.
class DialogCloser : public QObject
{
public:
    explicit DialogCloser(QObject* parent) : QObject(parent)
    {
        startTimer(40);
    }
    int closed = 0;

protected:
    void timerEvent(QTimerEvent*) override
    {
        if (QWidget* w = QApplication::activeModalWidget()) {
            if (auto* d = qobject_cast<QDialog*>(w)) d->reject();
            else w->close();
            ++closed;
        }
    }
};

// Links to the web end here, not in a browser.
class UrlCatcher : public QObject
{
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl& url) { urls << url; }
};

} // namespace

class TestPdfViewer : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QString file;
    UrlCatcher catcher;

    PdfDoc* pdf() const { return qobject_cast<PdfDoc*>(app->DocumentTab->currentWidget()); }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("ws"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("ws"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("ws"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &catcher, "open");
        file = dir.filePath("ws/probe.pdf");
        QVERIFY(writePdf(file, 3));
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 900);
        app->show();
        QVERIFY(QTest::qWaitForWindowExposed(app));
    }

    void cleanupTestCase()
    {
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
        for (QucsDoc* doc : app->allDocuments()) {
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
        delete app;
        QucsMain = nullptr;
    }

    // A tab of its own - once - known as a document by the window and by
    // Claude's tools.
    void aPdfOpensInATab()
    {
        QVERIFY(app->gotoPage(file));
        PdfDoc* doc = pdf();
        QVERIFY(doc != nullptr);
        QVERIFY(QucsApp::isPdfDocument(doc));
        QVERIFY(!QucsApp::isTextDocument(doc));
        QVERIFY(QucsApp::schematicIn(doc) == nullptr);
        QCOMPARE(app->getDoc(), static_cast<QucsDoc*>(doc));
        QCOMPARE(QucsApp::documentWidget(app->getDoc()), static_cast<QWidget*>(doc));
        QCOMPARE(doc->pageCount(), 3);
        QCOMPARE(doc->title(), QStringLiteral("The probe"));
        QVERIFY(doc->problem().isEmpty());
        QCOMPARE(app->DocumentTab->tabText(app->DocumentTab->currentIndex()).remove('&'), QStringLiteral("probe.pdf"));
        QVERIFY(doc->getDataDisplay().isEmpty());   // not simulated
        const int tabs = app->DocumentTab->count();
        QVERIFY(app->gotoPage(file));
        QCOMPARE(app->DocumentTab->count(), tabs);
        QCOMPARE(app->findDoc(file), static_cast<QucsDoc*>(doc));
        QVERIFY(app->allDocuments().contains(doc));

        auto* control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        QVERIFY(QucsControl::textOf(control->callNow("get_state", {})).contains("\"kind\": \"pdf\""));
        QVERIFY(control->callNow("get_schematic", {}).value("isError").toBool());   // not a schematic
    }

    // From the Content panel and the File Browser (and a drop): the same
    // tab, not the system's viewer.
    void itOpensFromThePanels()
    {
        const int tabs = app->DocumentTab->count();
        app->openFileFromProjectView(QFileInfo(file), QString());
        QCOMPARE(app->DocumentTab->count(), tabs);
        QVERIFY(QucsApp::isPdfDocument(app->DocumentTab->currentWidget()));
    }

    // Laid out to the width, drawn, gone through by page.
    void pagesAreShownAndGoneThrough()
    {
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        QCOMPARE(view->fit(), PageView::Fit::Width);
        QTRY_VERIFY(view->pageRect(0).width() > 100);
        QVERIFY(std::abs(view->pageRect(0).width() - (view->viewport()->width() - 32)) <= 2);
        QCOMPARE(view->currentPage(), 0);
        QCOMPARE(doc->pageField()->text(), QStringLiteral("1"));
        // Drawn: the paper white, the text dark.
        const QImage shot = view->viewport()->grab().toImage();
        const QRect page = view->pageRect(0).intersected(view->viewport()->rect());
        int dark = 0, white = 0;
        for (int y = page.top(); y < page.bottom(); y += 3)
            for (int x = page.left(); x < page.right(); x += 3) {
                const QColor c = shot.pixelColor(QPoint(x, y) * shot.devicePixelRatio());
                if (c.lightness() < 100) ++dark;
                else if (c.lightness() > 245) ++white;
            }
        QVERIFY2(dark > 20 && white > dark * 5, qPrintable(QStringLiteral("%1 %2").arg(dark).arg(white)));
        QVERIFY(view->tilesRendered() > 0);

        doc->nextPage();
        QCOMPARE(view->currentPage(), 1);
        QCOMPARE(doc->pageField()->text(), QStringLiteral("2"));
        doc->lastPage();
        QCOMPARE(view->currentPage(), 2);
        doc->previousPage();
        QCOMPARE(view->currentPage(), 1);
        doc->firstPage();
        QCOMPARE(view->currentPage(), 0);
        doc->pageField()->setText(QStringLiteral("3"));
        QMetaObject::invokeMethod(doc->pageField(), "returnPressed");
        QCOMPARE(view->currentPage(), 2);
        doc->pageField()->setText(QStringLiteral("99"));   // no such page: stays
        QMetaObject::invokeMethod(doc->pageField(), "returnPressed");
        QCOMPARE(view->currentPage(), 2);
        // The keys: Home, End.
        view->setFocus();
        QTest::keyClick(view, Qt::Key_Home);
        QCOMPARE(view->currentPage(), 0);
        QTest::keyClick(view, Qt::Key_End);
        QCOMPARE(view->currentPage(), 2);
        doc->firstPage();
    }

    // Zoomed by the View menu and the toolbar; fitted; the point in sight kept.
    void itIsZoomed()
    {
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        app->magOne->trigger();   // View > View 1:1
        QCOMPARE(view->zoom(), 1.0);
        QCOMPARE(view->fit(), PageView::Fit::None);
        const QSizeF points = doc->document()->pagePointSize(0);
        QVERIFY(std::abs(view->pageRect(0).width() - points.width() * view->pixelsPerPoint()) <= 2);
        app->magPlus->trigger();   // View > Zoom In
        QCOMPARE(view->zoom(), 1.25);
        QVERIFY(!app->magPlus->isChecked());   // not a mode, for a PDF
        app->magMinus->trigger();
        QCOMPARE(view->zoom(), 1.0);
        app->magAll->trigger();   // View All: the page whole
        QCOMPARE(view->fit(), PageView::Fit::Page);
        QVERIFY(view->pageRect(0).height() <= view->viewport()->height() - 30);
        app->magSel->trigger();   // Zoom to Selection: the width
        QCOMPARE(view->fit(), PageView::Fit::Width);
        view->setZoom(100);   // clamped
        QCOMPARE(view->zoom(), PageView::MaxZoom);
        // At 1600% only the tiles in sight are drawn.
        const int before = view->tilesRendered();
        view->viewport()->repaint();
        QVERIFY(view->tilesRendered() - before < 40);
        view->setZoom(0.001);
        QCOMPARE(view->zoom(), PageView::MinZoom);
        view->setFit(PageView::Fit::Width);
    }

    // Found, gone through, wrapped round; the menu's Find brings the bar.
    void itIsSearched()
    {
        PdfDoc* doc = pdf();
        doc->find(QStringLiteral("needle"));
        QTRY_COMPARE(doc->searchResultCount(), 2);
        doc->findNext();
        QCOMPARE(doc->currentSearchResult(), 0);
        QTRY_COMPARE(doc->view()->currentPage(), 2);
        doc->findNext();
        QCOMPARE(doc->currentSearchResult(), 1);
        doc->findNext();
        QCOMPARE(doc->currentSearchResult(), 0);
        doc->findNext(true);
        QCOMPARE(doc->currentSearchResult(), 1);
        doc->find(QStringLiteral("nowhere to be found"));
        QTest::qWait(300);
        QCOMPARE(doc->searchResultCount(), 0);
        doc->hideSearch();
        QVERIFY(!doc->searchField()->isVisible());
        app->editFind->trigger();   // Edit > Find
        QVERIFY(doc->searchField()->isVisible());
        doc->hideSearch();
        doc->firstPage();
    }

    // A page's text, a word, a stretch: selected, copied.
    void textIsSelectedAndCopied()
    {
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        QApplication::clipboard()->clear();
        app->selectAll->trigger();   // Edit > Select All: the page's text
        QVERIFY(view->hasSelection());
        QVERIFY2(view->selectedText().contains("Hello world"), qPrintable(view->selectedText()));
        app->editCopy->trigger();   // Edit > Copy
        QVERIFY(QApplication::clipboard()->text().contains("Hello world"));

        // A word: where the search finds it, double clicked.
        doc->find(QStringLiteral("Hello"));
        QTRY_COMPARE(doc->searchResultCount(), 1);
        doc->findNext();
        QTRY_VERIFY(doc->currentSearchResult() == 0);
        auto* model = doc->findChild<QPdfSearchModel*>();
        QVERIFY(model != nullptr);
        const QList<QRectF> rects = model->resultAtIndex(0).rectangles();
        QVERIFY(!rects.isEmpty());
        QRectF word = rects.first();
        for (const QRectF& r : rects) word = word.united(r);
        doc->hideSearch();
        const QPointF centre = word.center();
        const QPoint at = (view->pageRect(0).topLeft() + centre * view->pixelsPerPoint()).toPoint();
        QTest::mouseDClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(view->selectedText().trimmed(), QStringLiteral("Hello"));
        // A stretch, dragged across: from the word's start past its end.
        view->clearSelection();
        QVERIFY(view->selectText(0, QPointF(word.left() + 0.5, centre.y()), QPointF(word.right() + 40, centre.y())));
        QVERIFY2(view->selectedText().startsWith("Hello"), qPrintable(view->selectedText()));
        // Begun beside the text (the page's corner): from the nearest character.
        QVERIFY(view->selectText(0, QPointF(1, 1), QPointF(word.right() + 2, centre.y())));
        QVERIFY2(view->selectedText().contains("Hello"), qPrintable(view->selectedText()));
        view->clearSelection();
        QVERIFY(!view->hasSelection());
    }

    // A link to the web: opened (here, caught); the hand over it.
    void linksAreFollowed()
    {
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        doc->firstPage();
        QPdfLinkModel links;
        links.setDocument(doc->document());
        links.setPage(0);
        QVERIFY(links.rowCount(QModelIndex()) >= 1);   // a rectangle for each line it takes
        const QRectF rect = links.data(links.index(0), int(QPdfLinkModel::Role::Rectangle)).toRectF();
        const QPoint at = (view->pageRect(0).topLeft() + rect.center() * view->pixelsPerPoint()).toPoint();
        QMouseEvent hover(QEvent::MouseMove, QPointF(at), view->viewport()->mapToGlobal(QPointF(at)), Qt::NoButton,
                          Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &hover);
        QCOMPARE(view->viewport()->cursor().shape(), Qt::PointingHandCursor);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QTRY_COMPARE(catcher.urls.size(), 1);
        QCOMPARE(catcher.urls.first(), QUrl("https://example.org/datasheet"));
    }

    // Written again (a report made anew): read again, the page kept.
    void itIsReadAgainWhenTheFileChanges()
    {
        PdfDoc* doc = pdf();
        doc->nextPage();
        QCOMPARE(doc->view()->currentPage(), 1);
        QSignalSpy reloaded(doc, &PdfDoc::reloaded);
        QTest::qWait(1100);   // a newer time on the file
        QVERIFY(writePdf(file, 5));
        QTRY_VERIFY_WITH_TIMEOUT(reloaded.count() >= 1, 10000);
        QCOMPARE(doc->pageCount(), 5);
        QCOMPARE(doc->view()->currentPage(), 1);
    }

    // The pages at the side, drawn small; one clicked is gone to. No
    // outline in this document: its tab hidden.
    void itsPagesAreAtTheSide()
    {
        PdfDoc* doc = pdf();
        doc->setSidebarShown(true);
        QVERIFY(doc->sidebar()->isVisible());
        QCOMPARE(doc->thumbnails()->count(), 5);
        QTRY_VERIFY([&] {
            // The first page's text, some grey among the white.
            const QImage img = doc->thumbnails()->item(0)->icon().pixmap(QSize(104, 150)).toImage();
            int grey = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (img.pixelColor(x, y).lightness() < 200) ++grey;
            return grey > 10;
        }());
        doc->thumbnails()->setCurrentRow(3);
        QCOMPARE(doc->view()->currentPage(), 3);
        doc->setSidebarShown(false);
        QVERIFY(!doc->sidebar()->isVisible());
    }

    // Saved as: a copy, the tab named after it.
    void itIsSavedUnderAnotherName()
    {
        PdfDoc* doc = pdf();
        const QString copy = dir.filePath("ws/copy.pdf");
        QVERIFY(app->saveDocumentAs(doc, copy));
        QVERIFY(QFileInfo::exists(copy));
        QFile a(file), b(copy);
        QVERIFY(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly));
        QCOMPARE(a.readAll(), b.readAll());
        QCOMPARE(doc->getDocName(), QFileInfo(copy).absoluteFilePath());
        QVERIFY(app->saveFile(doc));   // nothing to write: fine
        file = copy;
    }

    // Not a PDF: refused, said why, no tab left.
    void aFileThatIsNotAPdfIsRefused()
    {
        const QString bad = dir.filePath("ws/bad.pdf");
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("this is not a PDF at all\n");
        f.close();
        const int tabs = app->DocumentTab->count();
        misc::ErrorCapture capture;
        QVERIFY(!app->gotoPage(bad));
        QCOMPARE(app->DocumentTab->count(), tabs);
        QVERIFY(!capture.errors().isEmpty());
    }

    // Every command that works on the document in front, with a PDF
    // there: none may take it for a schematic or a text document.
    void everyCommandLeavesItWhole()
    {
        QVERIFY(app->gotoPage(file));
        auto* closer = new DialogCloser(this);
        const QStringList menus = {"Edit", "Positioning", "Insert", "Simulation", "View"};
        const QStringList skip = {"Terminal", "Python", "Octave", "Claude", "Full Screen", "Exit", "Quit"};
        QList<QAction*> actions;
        std::function<void(QMenu*)> walk = [&](QMenu* menu) {
            for (QAction* a : menu->actions()) {
                if (a->menu() != nullptr) walk(a->menu());
                else if (!a->isSeparator()) actions << a;
            }
        };
        for (QAction* top : app->menuBar()->actions())
            if (top->menu() != nullptr && menus.contains(QString(top->text()).remove('&')))
                walk(top->menu());
        // (Not File > Print: the system's print panel is no dialog of
        // Qt's to close; printing is tested on its own.)
        actions << app->fileSave << app->fileSaveAs << app->fileSaveAll << app->fileSettings
                << app->exportAsImage << app->editCopyImage << app->symEdit;
        QVERIFY(actions.size() > 60);
        int used = 0;
        for (QAction* a : std::as_const(actions)) {
            const QString text = QString(a->text()).remove('&');
            if (std::any_of(skip.cbegin(), skip.cend(), [&](const QString& s) { return text.contains(s, Qt::CaseInsensitive); }))
                continue;
            QVERIFY(app->gotoPage(file));
            QVERIFY(QucsApp::isPdfDocument(app->DocumentTab->currentWidget()));
            if (!a->isEnabled()) continue;
            a->trigger();
            if (a->isCheckable() && a->isChecked() && a != app->select) a->trigger();   // back off a mode
            ++used;
            QTest::qWait(1);
        }
        QVERIFY(used > 40);
        // Still there and whole.
        QVERIFY(app->gotoPage(file));
        PdfDoc* doc = pdf();
        QVERIFY(doc != nullptr);
        QVERIFY(doc->pageCount() == 5);
        doc->view()->viewport()->repaint();
        delete closer;
    }

    // Printed: every page on a sheet (here to a PDF file).
    void itIsPrinted()
    {
        QVERIFY(app->gotoPage(file));
        PdfDoc* doc = pdf();
        const QString out = dir.filePath("printed.pdf");
        {
            QPrinter printer(QPrinter::HighResolution);
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(out);
            QPainter painter(&printer);
            doc->print(&printer, &painter, true, true);
        }
        QPdfDocument printed;
        QCOMPARE(printed.load(out), QPdfDocument::Error::None);
        QCOMPARE(printed.pageCount(), doc->pageCount());
        // Each sheet the page's picture: some text on white.
        const QImage sheet = printed.render(0, QSize(300, 424));
        int dark = 0;
        for (int y = 0; y < sheet.height(); ++y)
            for (int x = 0; x < sheet.width(); ++x)
                if (sheet.pixelColor(x, y).alpha() > 0 && sheet.pixelColor(x, y).lightness() < 128) ++dark;
        QVERIFY2(dark > 20, qPrintable(QString::number(dark)));
    }

    // Closed like any document.
    void itIsClosed()
    {
        QVERIFY(app->gotoPage(file));
        app->slotFileClose(app->DocumentTab->currentIndex());
        QVERIFY(app->findDoc(file) == nullptr);   // (the last tab closed: an untitled one in its place)
        for (int i = 0; i < app->DocumentTab->count(); ++i) QVERIFY(!QucsApp::isPdfDocument(app->DocumentTab->widget(i)));
    }
};

QTEST_MAIN(TestPdfViewer)
#include "test_pdf_viewer.moc"
