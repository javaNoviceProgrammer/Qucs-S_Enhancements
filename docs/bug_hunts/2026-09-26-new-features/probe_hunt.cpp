// Bug-hunt probes (temporary; kept in bug_hunts/ as reproductions).
#include <QtTest>
#include <QPainter>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "schematic.h"
#include "mathtypeset.h"
#include "paintings/paintings.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "filebrowser.h"
#include "gitstatus.h"
#include "spreadsheet.h"
#include "zipfile.h"
#include "markdowndoc.h"
#include "textdoc.h"
#include "pdfdoc.h"
#include "claudehistory.h"
#include "numberformat.h"
#include <QPdfWriter>
#include <QPdfDocument>
#include <QInputDialog>
#include <QMenu>
#include <QDesktopServices>
#include <QTextBrowser>
#include <sys/resource.h>
#include <QMessageBox>
#include <QPushButton>

namespace {
QImage drawIt(Painting* p, int size = 200)
{
  QImage canvas(size, size, QImage::Format_ARGB32);
  canvas.fill(Qt::white);
  QPainter painter(&canvas);
  const QRect b = p->boundingRect();
  painter.translate(size / 2.0 - b.center().x(), size / 2.0 - b.center().y());
  p->paint(&painter);
  painter.end();
  return canvas;
}

bool writeFile(const QString& path, const QByteArray& bytes = "x")
{
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

// Runs fn; every message box it brings up is noted and answered: the
// button named \a button when it has one, else its default/escape.
QStringList answeringAll(const std::function<void()>& fn, const QString& button = QString())
{
  QStringList seen;
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&] {
    if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
      seen << box->text() + " | " + box->informativeText();
      QAbstractButton* b = button.isEmpty() ? nullptr : box->findChild<QPushButton*>(button);
      if (b == nullptr) b = box->defaultButton();
      if (b == nullptr) b = box->escapeButton();
      if (b == nullptr && !box->buttons().isEmpty()) b = box->buttons().first();
      if (b) b->click();
    }
  });
  timer.start(20);
  fn();
  return seen;
}

Painting* loaded(const QString& type, const QString& line)
{
  Painting* p = Painting::newNamed(type);
  if (p == nullptr) return nullptr;
  if (!p->load(line)) qInfo() << "load refused:" << line.left(120);
  return p;
}
}

class UrlCatcher : public QObject {
  Q_OBJECT
public:
  QList<QUrl> urls;
public slots:
  void open(const QUrl& url) { urls << url; }
};

class ProbeHunt : public QObject {
  Q_OBJECT
  QTemporaryDir dir;

private slots:
  void initTestCase()
  {
    useIsolatedSettings(dir.filePath("settings"));
    Module::registerModules();
  }

  void waveformNaNCycles()
  {
    std::unique_ptr<Painting> p(loaded("Waveform", "Waveform 0 0 100 60 #000080 2 1 #c0c0c0 1 0 0 0 0 nan 25 0"));
    QVERIFY(p);
    QElapsedTimer t;
    t.start();
    drawIt(p.get());
    p->boundingRect();
    qInfo() << "painted in" << t.elapsed() << "ms; saved:" << p->save();
  }

  void textBoxHugeTip()
  {
    std::unique_ptr<Painting> p(loaded("TextBox", "TextBox 0 0 100 60 #000000 1 1 #ffffc0 1 1 0 0 2 6 6 #000000 10 0 1 1 1 2147483647 2147483647 ~hi"));
    QVERIFY(p);
    const QRect b = p->boundingRect();
    qInfo() << "bounds" << b;
    drawIt(p.get());
    p->rotate();
    p->mirrorX();
    qInfo() << "saved:" << p->save();
  }

  void dimensionOddScale()
  {
    for (const char* scale : {"nan", "inf", "-inf", "1e308", "0"}) {
      std::unique_ptr<Painting> p(loaded("Dimension", QString("Dimension 0 0 100 0 20 #000000 1 1 0 10 %1 2 ~mm ~").arg(scale)));
      QVERIFY(p);
      drawIt(p.get());
      qInfo() << scale << "->" << static_cast<DimensionPainting*>(p.get())->label() << p->save();
    }
  }

  void dimensionSamePoints()
  {
    std::unique_ptr<Painting> p(loaded("Dimension", "Dimension 10 10 10 10 20 #000000 1 1 0 10 1 2 ~mm ~"));
    QVERIFY(p);
    drawIt(p.get());
    qInfo() << p->boundingRect() << p->save();
  }

  void texDeeplyNested()
  {
    for (int depth : {100, 1000, 20000}) {
      QString tex = QString("{").repeated(depth) + "x" + QString("}").repeated(depth);
      QElapsedTimer t;
      t.start();
      auto r = qucs_s::math::typeset(tex, QFont(), Qt::black, true);
      qInfo() << "depth" << depth << "braces:" << t.elapsed() << "ms";
      QString frac;
      for (int i = 0; i < depth; ++i) frac += "\\frac{1}{";
      frac += "x" + QString("}").repeated(depth);
      t.restart();
      r = qucs_s::math::typeset(frac, QFont(), Qt::black, true);
      qInfo() << "depth" << depth << "fracs:" << t.elapsed() << "ms";
      QString sup = "x";
      for (int i = 0; i < depth; ++i) sup += "^{x";
      sup += QString("}").repeated(depth);
      t.restart();
      r = qucs_s::math::typeset(sup, QFont(), Qt::black, true);
      qInfo() << "depth" << depth << "sups:" << t.elapsed() << "ms";
    }
  }

  // Replace, where the one there is the folder the one moved is in.
  void fileBrowserReplaceAncestor()
  {
    QTemporaryDir w;
    const QString root = QFileInfo(w.path()).canonicalFilePath();
    QVERIFY(writeFile(root + "/x/x/data.txt", "precious"));
    QVERIFY(writeFile(root + "/x/other.txt", "also precious"));
    FileBrowser fb;
    QStringList done;
    const QStringList boxes = answeringAll([&] { done = fb.transfer({root + "/x/x"}, root, Qt::MoveAction); }, "fbClashReplace");
    qInfo() << "boxes:" << boxes;
    qInfo() << "done:" << done;
    qInfo() << "x exists:" << QFileInfo::exists(root + "/x") << "x/data.txt:" << QFileInfo::exists(root + "/x/data.txt")
            << "x/other.txt:" << QFileInfo::exists(root + "/x/other.txt");
  }

  // A folder moved into its own subfolder reached through a link.
  void fileBrowserSymlinkIntoItself()
  {
    QTemporaryDir w;
    const QString root = QFileInfo(w.path()).canonicalFilePath();
    QVERIFY(writeFile(root + "/a/sub/f.txt", "f"));
    QVERIFY(QFile::link(root + "/a", root + "/link"));
    FileBrowser fb;
    QStringList done;
    QElapsedTimer t;
    t.start();
    const QStringList boxes = answeringAll([&] { done = fb.transfer({root + "/a"}, root + "/link/sub", Qt::MoveAction); });
    qInfo() << "took" << t.elapsed() << "ms boxes:" << boxes << "done:" << done;
    qInfo() << "a exists:" << QFileInfo::exists(root + "/a") << "a/sub/f.txt:" << QFileInfo::exists(root + "/a/sub/f.txt");
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    int n = 0;
    while (it.hasNext() && n < 30) { qInfo() << "  " << it.next().mid(root.size()); ++n; }
  }

  // A repository's own config names programs git runs: does the git bar
  // run them just by looking at the project?
  void gitStatusRunsRepoConfiguredPrograms()
  {
    QTemporaryDir w;
    const QString root = QFileInfo(w.path()).canonicalFilePath();
    const QString marker = root + "/../probe-fsmonitor-ran-" + QFileInfo(root).fileName();
    const auto sh = [&](const QString& cmd) {
      QProcess p;
      p.setWorkingDirectory(root);
      p.start("/bin/sh", {"-c", cmd});
      p.waitForFinished(10000);
      return p.exitCode();
    };
    QCOMPARE(sh("git init -q . && git config user.email a@b && git config user.name a && echo hi > f.txt && git add f.txt && git commit -qm one"), 0);
    QVERIFY(writeFile(root + "/hook.sh", QString("#!/bin/sh\necho ran >> '%1'\nexit 1\n").arg(marker).toUtf8()));
    QFile::setPermissions(root + "/hook.sh", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QCOMPARE(sh("git config core.fsmonitor \"$PWD/hook.sh\""), 0);
    QVERIFY(writeFile(root + "/f.txt", "changed"));
    QFile::remove(marker);
    const auto st = qucs_s::git::status(root);
    qInfo() << "repository" << st.repository << "changes" << st.changes.size();
    qInfo() << "fsmonitor hook ran:" << QFileInfo::exists(marker);
    if (!st.changes.isEmpty()) {
      QFile::remove(marker);
      qucs_s::git::diff(st, "f.txt");
      qInfo() << "after diff(): hook ran:" << QFileInfo::exists(marker);
    }
    QFile::remove(marker);
  }

  // A 200 KB workbook whose sheet inflates to 200 MB.
  void xlsxZipBomb()
  {
    const QString path = qEnvironmentVariable("PROBE_BOMB");
    if (path.isEmpty()) QSKIP("PROBE_BOMB not set");
    QElapsedTimer t;
    t.start();
    qucs_s::sheet::Workbook book;
    QString error;
    const bool ok = qucs_s::sheet::readFile(path, book, &error);
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    qInfo() << "file" << QFileInfo(path).size() << "bytes; read" << ok << error << "in" << t.elapsed() << "ms; peak RSS"
            << (u.ru_maxrss >> 20) << "MB";
    for (const auto& sheet : book.sheets)
      for (int r = 0; r < std::min<qsizetype>(sheet.rows.size(), 6); ++r)
        for (const auto& c : sheet.rows.at(r).cells) qInfo() << "  row" << r << int(c.kind) << c.text;
  }

  // A link in a Markdown preview to a program beside it.
  void markdownLinkToProgram()
  {
    QTemporaryDir w;
    const QString root = QFileInfo(w.path()).canonicalFilePath();
    QVERIFY(writeFile(root + "/evil.command", "#!/bin/sh\necho pwned\n"));
    QVERIFY(writeFile(root + "/README.md", "[Open the schematic](evil.command)\n"));
    UrlCatcher catcher;
    QDesktopServices::setUrlHandler("file", &catcher, "open");
    QDesktopServices::setUrlHandler("x-custom", &catcher, "open");
    MarkdownDoc doc(nullptr, root + "/README.md");
    QVERIFY(doc.load());
    auto* browser = doc.findChild<QTextBrowser*>();
    QVERIFY(browser);
    emit browser->anchorClicked(QUrl("evil.command"));
    emit browser->anchorClicked(QUrl("x-custom://whatever"));
    qInfo() << "handed to the system:" << catcher.urls;
    QDesktopServices::unsetUrlHandler("file");
    QDesktopServices::unsetUrlHandler("x-custom");
  }

  // Rename through the dialog (a file not in the view shown).
  void fileBrowserRenameDialog()
  {
    QTemporaryDir w;
    const QString root = QFileInfo(w.path()).canonicalFilePath();
    QVERIFY(writeFile(root + "/proj/amp.sch", "amp"));
    QVERIFY(writeFile(root + "/proj/filter.sch", "filter"));
    FileBrowser fb;
    QSignalSpy moved(&fb, &FileBrowser::moved);
    const auto renameTo = [&](const QString& path, const QString& name) {
      QStringList boxes;
      QTimer timer;
      QObject::connect(&timer, &QTimer::timeout, [&] {
        QWidget* m = QApplication::activeModalWidget();
        if (auto* d = qobject_cast<QInputDialog*>(m)) { d->setTextValue(name); d->accept(); }
        else if (auto* b = qobject_cast<QMessageBox*>(m)) { boxes << b->text(); b->accept(); }
      });
      timer.start(20);
      QMenu* menu = fb.contextMenuFor(path);
      for (QAction* a : menu->actions())
        if (a->text().startsWith("Rename")) a->trigger();
      delete menu;
      QTest::qWait(100);
      return boxes;
    };
    qInfo() << "case only:" << renameTo(root + "/proj/amp.sch", "Amp.sch")
            << QDir(root + "/proj").entryList(QDir::Files);
    qInfo() << "with ../:" << renameTo(root + "/proj/filter.sch", "../escaped.sch")
            << "proj:" << QDir(root + "/proj").entryList(QDir::Files) << "root:" << QDir(root).entryList(QDir::Files);
    qInfo() << "moved() emitted:" << moved.count();
  }

  // A CSV file that is not UTF-8, edited with a character Latin-1 has not.
  void csvLatin1Edit()
  {
    QTemporaryDir w;
    const QString path = w.filePath("parts.csv");
    QVERIFY(writeFile(path, "part,value\nR1,10 k\xB5\n"));   // Latin-1 micro sign
    qucs_s::sheet::Workbook book;
    QVERIFY(qucs_s::sheet::readFile(path, book));
    qInfo() << "read as latin1:" << book.latin1 << book.sheets[0].at(1, 1).text;
    book.sheets[0].cell(1, 1).text = QString::fromUtf8("4.7 k\xCE\xA9");   // "4.7 kΩ"
    QString error;
    QVERIFY(qucs_s::sheet::writeFile(path, book, 0, &error));
    qucs_s::sheet::Workbook again;
    QVERIFY(qucs_s::sheet::readFile(path, again));
    qInfo() << "typed" << QString::fromUtf8("4.7 k\xCE\xA9") << "saved and read back:" << again.sheets[0].at(1, 1).text;

    // UTF-16 (Excel's "Unicode Text"), tab separated.
    const QString u16 = w.filePath("unicode.tsv");
    QByteArray bytes("\xFF\xFE");
    const QString text = "part\tvalue\nR1\t10k\n";
    bytes += QByteArray(reinterpret_cast<const char*>(text.utf16()), text.size() * 2);
    QVERIFY(writeFile(u16, bytes));
    qucs_s::sheet::Workbook b16;
    QVERIFY(qucs_s::sheet::readFile(u16, b16));
    qInfo() << "UTF-16 read: latin1" << b16.latin1 << "cells" << b16.sheets[0].at(0, 0).text << b16.sheets[0].at(0, 1).text;
  }

  // Every new painting near the coordinate clamp, turned and mirrored
  // about far-away centres, its handles dragged far.
  void paintingsFarAway()
  {
    const QStringList lines = {
      "RoundRect 7999000 7999000 100 60 #000080 2 1 #c0c0c0 1 0 0 0 10",
      "RegPolygon -7999000 7999000 80 80 #000080 2 1 #c0c0c0 1 1 0 0 1 6 1 45 0",
      "Brace 7999000 -7999000 20 80 #000080 2 1 #c0c0c0 1 0 90 1 0",
      "Waveform 7999900 7999900 100 60 #000080 2 1 #c0c0c0 1 0 0 0 5 3 25 1",
      "TextBox 7999000 7999000 100 60 #000000 1 1 #ffffc0 1 1 0 0 2 6 6 #000000 10 0 1 1 1 7999500 7999999 ~hello",
      "Table 7999000 7999000 120 60 #000000 1 1 #ffffff 1 0 0 0 2 2 1 #e0e0e0 #000000 10 0 ~a ~b ~c ~d",
      "Dimension 7999000 7999000 -7999000 -7999000 7999999 #000000 1 1 0 10 1 2 ~mm ~",
      "Formula 7999000 7999000 12 #000000 0 1 ~x%5E2",
    };
    for (const QString& line : lines) {
      std::unique_ptr<Painting> p(loaded(line.section(' ', 0, 0), line));
      QVERIFY(p);
      for (int k = 0; k < 4; ++k) {
        p->rotate(-7999999, 7999999);
        p->mirrorX();
        p->mirrorY();
        p->rotate();
        p->boundingRect();
      }
      const QRect b = p->boundingRect();
      Q_UNUSED(b);
      p->getSelected(QPoint(0, 0), 5);
      qInfo() << line.section(' ', 0, 0) << p->boundingRect() << p->save().left(90);
    }
  }

  // A project whose simulation outputs are not ignored: what one refresh of
  // the git bar costs.
  void gitStatusUntrackedDatasets()
  {
    QTemporaryDir w;
    const QString root = QFileInfo(w.path()).canonicalFilePath();
    QProcess p;
    p.setWorkingDirectory(root);
    p.start("/bin/sh", {"-c", "git init -q . && git config user.email a@b && git config user.name a && echo x > a.sch && git add a.sch && git commit -qm one"});
    p.waitForFinished(10000);
    QByteArray line = "  1.000000000000000e-06  2.500000000000000e+00\n";
    QByteArray big;
    while (big.size() < 3 * 1024 * 1024) big += line;
    for (int i = 0; i < 150; ++i) QVERIFY(writeFile(root + QString("/sim%1.dat").arg(i), big));
    QElapsedTimer t;
    t.start();
    const auto st = qucs_s::git::status(root);
    qInfo() << "150 untracked 3 MB datasets:" << st.changes.size() << "changes, one refresh took" << t.elapsed() << "ms";
  }

  // Comment/Uncomment on a selection that starts and ends inside lines.
  void textCommentPartialSelection()
  {
    QTemporaryDir w;
    const QString path = w.filePath("n.cir");
    QVERIFY(writeFile(path, "R1 a b 1k\nC1 b 0 1n\nL1 b c 1u\n"));
    TextDoc doc(nullptr, path);
    QVERIFY(doc.load());
    QTextCursor c = doc.textCursor();
    c.setPosition(3);    // in "R1 a b 1k", after "R1 "
    c.setPosition(13, QTextCursor::KeepAnchor);   // in "C1 b 0 1n"
    doc.setTextCursor(c);
    doc.commentSelected();
    qInfo().noquote() << "partial selection ->\n" + doc.toPlainText();

    // Whole lines selected the usual way: from the start of one line to the
    // start of the line after the last.
    TextDoc doc2(nullptr, path);
    QVERIFY(doc2.load());
    QTextCursor c2 = doc2.textCursor();
    c2.setPosition(0);
    c2.setPosition(20, QTextCursor::KeepAnchor);   // start of "L1 ..."
    doc2.setTextCursor(c2);
    doc2.commentSelected();
    qInfo().noquote() << "lines 1-2 selected ->\n" + doc2.toPlainText();
  }

  // A report deleted and written again a few seconds later, as a script
  // that builds it does.
  void pdfRewrittenAfterDelete()
  {
    QTemporaryDir w;
    const QString path = w.filePath("report.pdf");
    const auto writePdf = [&](int pages) {
      QPdfWriter writer(path);
      QPainter painter(&writer);
      for (int i = 0; i < pages; ++i) {
        if (i > 0) writer.newPage();
        painter.drawText(100, 100, QString("page %1").arg(i + 1));
      }
    };
    writePdf(1);
    PdfDoc doc(nullptr, path);
    QVERIFY(doc.load());
    auto* pdf = doc.findChild<QPdfDocument*>();
    QVERIFY(pdf);
    qInfo() << "pages at first:" << pdf->pageCount();
    writePdf(2);   // written in place
    QTest::qWait(1500);
    qInfo() << "after writing in place:" << pdf->pageCount();
    QFile::remove(path);
    QTest::qWait(1500);   // the script takes its time
    writePdf(3);
    QTest::qWait(1500);
    qInfo() << "after delete, 1.5 s, write:" << pdf->pageCount();
    writePdf(4);
    QTest::qWait(1500);
    qInfo() << "and written once more:" << pdf->pageCount();
  }

  // The master cell of a shared formula edited, the workbook saved.
  void xlsxSharedFormulaMasterEdited()
  {
    const QString src = qEnvironmentVariable("PROBE_SHARED");
    if (src.isEmpty()) QSKIP("PROBE_SHARED not set");
    QTemporaryDir w;
    const QString path = w.filePath("shared.xlsx");
    QVERIFY(QFile::copy(src, path));
    qucs_s::sheet::Workbook book;
    QVERIFY(qucs_s::sheet::readFile(path, book));
    auto& sheet = book.sheets[0];
    qInfo() << "B1 formula" << sheet.at(0, 1).formula << "B2 formula" << sheet.at(1, 1).formula << sheet.at(1, 1).text;
    auto& b1 = sheet.cell(0, 1);
    b1.formula = "A1*3";
    b1.changed = true;
    sheet.changed = true;
    QString error;
    QVERIFY(qucs_s::sheet::writeFile(path, book, 0, &error));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const auto entries = qucs_s::zip::read(f.readAll());
    for (const auto& e : entries)
      if (e.name.contains("sheet1")) qInfo().noquote() << e.data.mid(e.data.indexOf("<sheetData"), 600);
  }

  // What the Content panel's poll costs for a large folder opened as a
  // project (the setting "Any folder is a project").
  void contentPollOfLargeFolder()
  {
    const QString dir = qEnvironmentVariable("PROBE_BIGDIR");
    if (dir.isEmpty()) QSKIP("PROBE_BIGDIR not set");
    QElapsedTimer t;
    t.start();
    const QStringList files = misc::projectFiles(QDir(dir));
    qInfo() << files.size() << "files; one walk took" << t.elapsed() << "ms (GUI thread, at every poll)";
  }

  // Claude Code's sessions of a folder reached through a symbolic link
  // (/var -> /private/var on macOS, a linked workspace).
  void claudeSessionsThroughSymlink()
  {
    QTemporaryDir w;   // /var/folders/... - /private/var/folders/... really
    const QString spelled = QDir::cleanPath(w.path()) + "/work";
    QDir().mkpath(spelled);
    const QString real = QFileInfo(spelled).canonicalFilePath();
    qInfo() << "spelled" << spelled << "real" << real;
    const QString config = w.filePath("claude-config");
    QString name = real;
    for (QChar& c : name)
      if (!(c.isLetterOrNumber() && c.unicode() < 128)) c = QLatin1Char('-');
    QVERIFY(writeFile(config + "/projects/" + name + "/0b3b2c1e-1111-4222-8333-944445555666.jsonl",
                      "{\"type\":\"user\",\"cwd\":\"" + real.toUtf8() + "\",\"message\":{\"role\":\"user\",\"content\":\"hello\"}}\n"));
    qputenv("CLAUDE_CONFIG_DIR", config.toUtf8());
    qInfo() << "sessions found by the path as spelled:" << qucs_s::claude::history::claudeSessions(spelled, 10).size()
            << "- by the real path:" << qucs_s::claude::history::claudeSessions(real, 10).size();
    qunsetenv("CLAUDE_CONFIG_DIR");
  }

  // $ in an indented code block, and in inline HTML code.
  void mathInIndentedCode()
  {
    const QString md = "Run it:\n\n    export PATH=$HOME/bin:$PATH\n    cp $SRC/a.sch $DST/\n\nand <code>$A-$B</code>\n";
    for (const auto& span : qucs_s::math::findMath(md)) qInfo() << "typeset as math:" << span.tex;
  }

  // A marker's precision as a file may give it: what formatting its value costs.
  void markerHugePrecision()
  {
    for (int precision : {1000, 100000, 10000000}) {
      QElapsedTimer t;
      t.start();
      const QString g = QString::number(1.41295e7, 'g', precision);
      const QString rect = misc::complexRect(0.5, -0.25, precision);
      qint64 gms = t.elapsed();
      t.restart();
      const QString fixed = qucs_s::numberformat::format(1.41295e7, qucs_s::numberformat::Notation::Decimal, precision);
      qInfo() << "precision" << precision << ": 'g'" << g.size() << "chars, complexRect" << rect.size() << "chars in" << gms
              << "ms; numberformat Fixed" << fixed.size() << "chars in" << t.elapsed() << "ms";
    }
  }

  // A CSV file whose name has a %1 in it, saved as a workbook.
  void csvWithPercentNameToXlsx()
  {
    QTemporaryDir w;
    const QString csv = w.filePath("duty50%1k.csv");
    QVERIFY(writeFile(csv, "t,v\n0,1\n"));
    qucs_s::sheet::Workbook book;
    QVERIFY(qucs_s::sheet::readFile(csv, book));
    const QString xlsx = w.filePath("out.xlsx");
    QVERIFY(qucs_s::sheet::writeFile(xlsx, book, 0));
    QFile f(xlsx);
    QVERIFY(f.open(QIODevice::ReadOnly));
    for (const auto& e : qucs_s::zip::read(f.readAll()))
      if (e.name == "xl/workbook.xml") qInfo().noquote() << e.data.mid(e.data.indexOf("<sheets"), 120);
    qucs_s::sheet::Workbook again;
    QString error;
    qInfo() << "read back:" << qucs_s::sheet::readFile(xlsx, again, &error) << error;
  }

  // The biggest formula the dialog allows, zoomed in.
  void formulaBigZoomed()
  {
    std::unique_ptr<Painting> p(loaded("Formula", "Formula 0 0 400 #000000 0 1 ~" +
        qucs_s::paintings::encodeText("H(s) = \\frac{\\omega_0^2}{s^2 + \\frac{\\omega_0}{Q}s + \\omega_0^2} = \\sum_{k=0}^{N} a_k s^k")));
    QVERIFY(p);
    auto* f = static_cast<FormulaPainting*>(p.get());
    QElapsedTimer t;
    t.start();
    const QImage img = f->image(Qt::black, 8.0);
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    qInfo() << "size in units" << f->formulaSize() << "image" << img.size() << (img.sizeInBytes() >> 20) << "MB in" << t.elapsed()
            << "ms; peak RSS" << (u.ru_maxrss >> 20) << "MB";
  }

  // A simulation's export: 500,000 rows of 4 numbers.
  void csvLarge()
  {
    QTemporaryDir w;
    const QString path = w.filePath("tran.csv");
    QByteArray bytes = "time,v(in),v(out),i(v1)\n";
    for (int i = 0; i < 500000; ++i) bytes += QByteArray::number(i * 1e-9, 'g', 12) + ",1.234567e-3,2.345678e-3,-4.5e-6\n";
    QVERIFY(writeFile(path, bytes));
    QElapsedTimer t;
    t.start();
    qucs_s::sheet::Workbook book;
    QVERIFY(qucs_s::sheet::readFile(path, book));
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    qInfo() << bytes.size() / 1024 / 1024 << "MB CSV read in" << t.elapsed() << "ms; rows" << book.sheets[0].rows.size()
            << "peak RSS" << (u.ru_maxrss >> 20) << "MB";
  }

  // Every new shape with a box of nothing.
  void shapesDegenerateBox()
  {
    const QStringList lines = {
      "RoundRect 10 10 0 0 #000080 2 1 #c0c0c0 1 0 0 0 10",
      "RegPolygon 10 10 0 0 #000080 2 1 #c0c0c0 1 1 0 0 1 6 1 45 0",
      "Brace 10 10 0 0 #000080 2 1 #c0c0c0 1 0 90 1 0",
      "Waveform 10 10 0 0 #000080 2 1 #c0c0c0 1 0 0 0 5 3 25 1",
      "TextBox 10 10 0 0 #000000 1 1 #ffffc0 1 1 0 0 2 6 6 #000000 10 0 1 1 1 10 10 ~hello%20world%20and%20more",
      "Table 10 10 0 0 #000000 1 1 #ffffff 1 0 0 0 2 2 1 #e0e0e0 #000000 10 0 ~a ~b ~c ~d",
      "Dimension 10 10 10 10 0 #000000 1 1 0 10 1 2 ~mm ~",
    };
    for (const QString& line : lines) {
      std::unique_ptr<Painting> p(loaded(line.section(' ', 0, 0), line));
      QVERIFY(p);
      QElapsedTimer t;
      t.start();
      drawIt(p.get());
      const bool hit = p->getSelected(QPoint(10, 10), 5);
      p->rotate();
      p->mirrorX();
      SymbolPrimitives parts;
      p->symbolPrimitives(parts);
      qInfo() << line.section(' ', 0, 0) << "painted in" << t.elapsed() << "ms, hit" << hit << "polylines" << parts.polylines.size()
              << p->save().left(60);
      qDeleteAll(parts.polylines); qDeleteAll(parts.lines); qDeleteAll(parts.texts); qDeleteAll(parts.images);
    }
  }

  // Save As onto the same file under another spelling (case, a link).
  void pdfSaveAsSameFileOtherSpelling()
  {
    QTemporaryDir w;
    const QString path = w.filePath("report.pdf");
    {
      QPdfWriter writer(path);
      QPainter painter(&writer);
      painter.drawText(100, 100, "the report");
    }
    const qint64 before = QFileInfo(path).size();
    PdfDoc doc(nullptr, path);
    QVERIFY(doc.load());
    doc.setName(w.filePath("Report.pdf"));   // the same file on macOS
    const int result = doc.save();
    qInfo() << "size before" << before << "save() returned" << result << "report.pdf exists:" << QFileInfo::exists(path)
            << "Report.pdf exists:" << QFileInfo::exists(w.filePath("Report.pdf")) << QDir(w.path()).entryList(QDir::Files);
  }

  // Undo right after a text document is opened; and a Latin-1 file saved.
  void textUndoAfterOpenAndLatin1()
  {
    QTemporaryDir w;
    const QString path = w.filePath("vendor.lib");
    QVERIFY(writeFile(path, "* (c) 2019 Vendor, T=25\xB0" "C, 10 \xB5" "A\n.model D1 D(Is=1e-14)\n"));
    TextDoc doc(nullptr, path);
    QVERIFY(doc.load());
    qInfo() << "after opening: undo steps" << doc.document()->availableUndoSteps() << "undo available" << doc.document()->isUndoAvailable();
    doc.undo();
    qInfo() << "after one Undo: text length" << doc.toPlainText().size() << "modified" << doc.document()->isModified();
    TextDoc doc2(nullptr, path);
    QVERIFY(doc2.load());
    doc2.save();
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    qInfo() << "Latin-1 file saved back:" << f.readAll().left(40).toHex(' ');
  }

  void texOddInputs()
  {
    for (const char* tex : {"\\frac", "\\frac{", "\\sqrt[", "\\begin{matrix}", "\\begin{matrix}a&b\\\\", "\\left(", "\\right)",
                            "}", "^^^^", "_", "\\\\\\\\", "\\begin{cases}", "\\end{matrix}", "\\sum_", "\\int^",
                            "\\begin{pmatrix}&&&&&&&&&&\\\\&&&&\\end{pmatrix}", "\\mathrm{", "\\text{", "\\operatorname"}) {
      auto r = qucs_s::math::typeset(QString::fromLatin1(tex), QFont(), Qt::black, true);
      Q_UNUSED(r);
    }
  }
};

QTEST_MAIN(ProbeHunt)
#include "probe_hunt.moc"
