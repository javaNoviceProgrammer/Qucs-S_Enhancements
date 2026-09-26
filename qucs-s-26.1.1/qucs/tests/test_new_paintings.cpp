/*
 * The paintings added to the palette: a rounded rectangle, a triangle, a
 * regular polygon and a star, a brace, a waveform, a text box, a note and
 * a callout, a table, a dimension, a formula. Each is registered with its
 * icon, saves and loads its line (text with spaces, quotes and line
 * breaks too), draws, turns and mirrors, is edited in its dialog - and in
 * a subcircuit's symbol reaches every instance, as the primitives it is
 * made of.
 */
#include <QtTest>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>

#include <memory>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "schematic.h"
#include "components/subcircuit.h"
#include "extsimkernels/spicecompat.h"
#include "paintings/paintings.h"
#include "isolated_settings.h"

namespace {

QString subcircuitWith(const QString& symbolLines)
{
  return "<Qucs Schematic " PACKAGE_VERSION ">\n"
         "<Properties>\n</Properties>\n"
         "<Symbol>\n"
         "  <.PortSym -40 0 1 0 in>\n"
         "  <.PortSym 40 0 2 180 out>\n"
         + symbolLines +
         "</Symbol>\n"
         "<Components>\n"
         "  <Port P1 1 100 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
         "  <Port P2 1 400 100 4 -42 0 2 \"2\" 1 \"analog\" 0>\n"
         "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
         "</Components>\n"
         "<Wires>\n"
         "  <100 100 220 100 \"in\" 140 70 0 \"\">\n"
         "  <280 100 400 100 \"out\" 320 70 0 \"\">\n"
         "</Wires>\n"
         "<Diagrams>\n</Diagrams>\n"
         "<Paintings>\n</Paintings>\n";
}

int pixelsOf(const QImage& image, const QColor& colour, int tolerance = 40)
{
  int found = 0;
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) {
      const QColor c = image.pixelColor(x, y);
      if (qAbs(c.red() - colour.red()) < tolerance && qAbs(c.green() - colour.green()) < tolerance
          && qAbs(c.blue() - colour.blue()) < tolerance)
        ++found;
    }
  return found;
}

QImage drawn(Painting* p, int size = 300)
{
  QImage canvas(size, size, QImage::Format_ARGB32);
  canvas.fill(Qt::white);
  QPainter painter(&canvas);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRect b = p->boundingRect();
  painter.translate(size / 2.0 - b.center().x(), size / 2.0 - b.center().y());
  p->paint(&painter);
  painter.end();
  return canvas;
}

// A painting brand new from its palette entry.
std::unique_ptr<Painting> fromPalette(pInfoFunc info)
{
  QString name;
  char* bitmap = nullptr;
  return std::unique_ptr<Painting>(static_cast<Painting*>(info(name, bitmap, true)));
}

// The same painting, loaded from its line.
std::unique_ptr<Painting> reloaded(Painting* p)
{
  const QString line = p->save();
  std::unique_ptr<Painting> again(Painting::newNamed(line.section(' ', 0, 0)));
  if (again == nullptr || !again->load(line)) return nullptr;
  return again;
}

QPolygonF firstPolygon(const ShapePainting& s)
{
  const QList<QPolygonF> polygons = s.worldPath().toSubpathPolygons();
  return polygons.isEmpty() ? QPolygonF() : polygons.first();
}

// Runs fn and answers the dialog it brings up with answer(dialog).
void answering(const std::function<void()>& fn, const std::function<bool(QWidget*)>& answer)
{
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&] {
    if (QWidget* modal = QApplication::activeModalWidget())
      if (answer(modal)) timer.stop();
  });
  timer.start(20);
  fn();
}

} // namespace

class TestNewPaintings : public QObject
{
  Q_OBJECT

  QTemporaryDir dir;

  QString write(const QString& name, const QString& text)
  {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return QString();
    QTextStream(&file) << text;
    return path;
  }

private slots:
  void initTestCase()
  {
    QVERIFY(dir.isValid());
    useIsolatedSettings(dir.filePath("settings"));
    QucsSettings.font = QApplication::font();
    QucsSettings.DefaultSimulator = spicecompat::simNgspice;
    QucsSettings.firstRun = false;
    QucsSettings.maxUndo = 20;
    QucsVersion = VersionTriplet(PACKAGE_VERSION);
    QucsSettings.QucsWorkDir.setPath(dir.path());
    Module::registerModules();
  }

  // In the palette's paintings, each with its icon.
  void inThePalette()
  {
    const QStringList expected{"Rounded Rectangle", "filled Rounded Rectangle", "Triangle", "Regular Polygon", "Star",
                               "Brace", "Waveform", "Text Box", "Note", "Callout", "Table", "Dimension", "Formula"};
    QStringList found;
    for (Module* m : Category::getModules(QObject::tr("paintings"))) {
      QString name;
      char* bitmap = nullptr;
      std::unique_ptr<Element> e(m->info(name, bitmap, true));
      if (!expected.contains(name)) continue;
      found << name;
      QVERIFY2(QFileInfo::exists(misc::getIconPath(QString(bitmap))), bitmap);
      QVERIFY2(dynamic_cast<Painting*>(e.get()) != nullptr, qPrintable(name));
    }
    QCOMPARE(found, expected);
  }

  // Every one saves and loads its line as it was.
  void theirLinesGoAndComeBack()
  {
    const pInfoFunc all[] = {&RoundedRectangle::info, &RoundedRectangle::info_filled, &RegularPolygon::info,
                             &RegularPolygon::info_polygon, &RegularPolygon::info_star, &BracePainting::info,
                             &WaveformPainting::info, &TextBoxPainting::info, &TextBoxPainting::info_note,
                             &TextBoxPainting::info_callout, &TablePainting::info, &DimensionPainting::info,
                             &FormulaPainting::info};
    for (pInfoFunc info : all) {
      auto p = fromPalette(info);
      if (auto* shape = dynamic_cast<ShapePainting*>(p.get())) shape->setBox(QRect(QPoint(-40, -30), QPoint(60, 50)));
      if (auto* d = dynamic_cast<DimensionPainting*>(p.get())) d->setPoints(QPoint(0, 0), QPoint(100, 0), -20);
      const QString line = p->save();
      auto again = reloaded(p.get());
      QVERIFY2(again != nullptr, qPrintable(line));
      QCOMPARE(again->save(), line);
      QVERIFY2(pixelsOf(drawn(again.get()), Qt::white, 10) < 300 * 300, qPrintable(line));   // something is drawn
    }
    // A bad line is refused.
    RoundedRectangle r;
    QVERIFY(!r.load("RoundRect 1 2 x 4"));
    TablePainting t;
    QVERIFY(!t.load("Table 0 0 100 50 #000000 1 1 #ffffff 1 1 0 0 2 2 1 #e4e8ee #000000 10 0 ~a ~b"));   // two cells of four
  }

  // Text with spaces, quotes, angle brackets, percent signs and lines.
  void textSurvivesItsLine()
  {
    const QString text = "Gain \"A\" > 20 dB <typ.>\n100% at 1 kHz \\ ok";
    TextBoxPainting box(TextBoxPainting::Kind::Note);
    box.setBox(QRect(0, 0, 150, 60));
    box.setField("text", text);
    const QString line = box.save();
    QVERIFY(!line.contains('\n') && !line.contains('"') && !line.contains('>'));
    TextBoxPainting again;
    QVERIFY(again.load(line));
    QCOMPARE(again.text(), text);

    TablePainting table;
    table.setField("rows", 2);
    table.setField("columns", 2);
    table.setField("cells", QVariantList{QStringList{"a b", ""}, QStringList{"", "100 %"}});
    TablePainting back;
    QVERIFY(back.load(table.save()));
    QCOMPARE(back.rows(), 2);
    QCOMPARE(back.columns(), 2);
    QCOMPARE(back.cell(0, 0), QStringLiteral("a b"));
    QCOMPARE(back.cell(0, 1), QString());
    QCOMPARE(back.cell(1, 1), QStringLiteral("100 %"));

    FormulaPainting formula;
    formula.setField("tex", "\\frac{V_{out}}{V_{in}} = -\\frac{R_2}{R_1}");
    FormulaPainting f2;
    QVERIFY(f2.load(formula.save()));
    QCOMPARE(f2.tex(), formula.tex());
  }

  // A triangle points right, as an amplifier does; turned, up; mirrored, left.
  void aTriangleTurnsAndMirrors()
  {
    RegularPolygon t(RegularPolygon::Kind::Triangle);
    t.setBox(QRect(QPoint(0, 0), QPoint(60, 40)));
    const auto apex = [&t] {
      QPointF best(-1e9, 0);
      const QPolygonF polygon = firstPolygon(t);
      // The corner alone on its side: the one furthest from the others' mean.
      QPointF mean;
      for (int i = 0; i < 3; ++i) mean += polygon.at(i);
      mean /= 3;
      qreal far = -1;
      for (int i = 0; i < 3; ++i) {
        const QPointF d = polygon.at(i) - mean;
        if (std::hypot(d.x(), d.y()) > far) {
          far = std::hypot(d.x(), d.y());
          best = polygon.at(i);
        }
      }
      return best.toPoint();
    };
    QCOMPARE(apex(), QPoint(60, 20));
    QCOMPARE(firstPolygon(t).boundingRect().toRect(), QRect(0, 0, 60, 40));

    QVERIFY(t.rotate());   // counter-clockwise: up
    QCOMPARE(t.angle(), 90);
    QCOMPARE(t.box().size(), QSize(41, 61));
    QCOMPARE(apex().y(), t.box().top());

    RegularPolygon m(RegularPolygon::Kind::Triangle);
    m.setBox(QRect(QPoint(0, 0), QPoint(60, 40)));
    QVERIFY(m.mirrorY());
    QVERIFY(m.isMirrored());
    t = m;
    QCOMPARE(apex(), QPoint(0, 20));
    QVERIFY(m.mirrorX());   // twice mirrored: a half turn
    QCOMPARE(m.angle(), 180);
    QVERIFY(!m.isMirrored());

    // Saved turned and mirrored, loaded the same.
    auto again = reloaded(&m);
    QVERIFY(again != nullptr);
    QCOMPARE(static_cast<ShapePainting*>(again.get())->worldPath().boundingRect(), m.worldPath().boundingRect());
  }

  // Turned or mirrored - after a turn too - every point of the outline
  // goes where turning or mirroring it about the centre takes it.
  void everyPointTurnsAndMirrors()
  {
    const auto points = [](const ShapePainting& s) {
      QList<QPointF> all;
      for (const QPolygonF& polygon : s.worldPath().toSubpathPolygons())
        for (const QPointF& p : polygon) all << QPointF(std::round(p.x() * 100) / 100, std::round(p.y() * 100) / 100);
      std::sort(all.begin(), all.end(), [](const QPointF& a, const QPointF& b) { return a.x() != b.x() ? a.x() < b.x() : a.y() < b.y(); });
      return all;
    };
    const auto mapped = [](QList<QPointF> all, const std::function<QPointF(const QPointF&)>& f) {
      for (QPointF& p : all) {
        p = f(p);
        p = QPointF(std::round(p.x() * 100) / 100, std::round(p.y() * 100) / 100);
      }
      std::sort(all.begin(), all.end(), [](const QPointF& a, const QPointF& b) { return a.x() != b.x() ? a.x() < b.x() : a.y() < b.y(); });
      return all;
    };
    const auto near = [](const QList<QPointF>& a, const QList<QPointF>& b) {
      if (a.size() != b.size()) return false;
      for (int i = 0; i < a.size(); ++i)
        if (std::abs(a.at(i).x() - b.at(i).x()) > 0.05 || std::abs(a.at(i).y() - b.at(i).y()) > 0.05) return false;
      return true;
    };
    std::vector<std::unique_ptr<ShapePainting>> shapes;
    {
      auto star = std::make_unique<RegularPolygon>(RegularPolygon::Kind::Star);
      star->setField("turn", 17);   // no symmetry left
      shapes.push_back(std::move(star));
      auto saw = std::make_unique<WaveformPainting>();
      saw->setField("shape", int(WaveformPainting::Shape::Sawtooth));
      saw->setField("cycles", 1.5);
      shapes.push_back(std::move(saw));
    }
    for (auto& s : shapes) {
      s->setBox(QRect(QPoint(0, 0), QPoint(60, 40)));   // (its centre on a whole point: 30, 20)
      for (int turns = 0; turns < 4; ++turns) {
        const QList<QPointF> before = points(*s);
        const qreal cx = s->box().center().x() + 0.5 * ((s->box().width() + 1) % 2), cy = s->box().center().y() + 0.5 * ((s->box().height() + 1) % 2);
        QVERIFY(s->mirrorY());
        QVERIFY2(near(points(*s), mapped(before, [&](const QPointF& p) { return QPointF(2 * cx - p.x(), p.y()); })),
                 qPrintable(QStringLiteral("mirrorY at %1").arg(s->angle())));
        QVERIFY(s->mirrorY());
        QVERIFY(near(points(*s), before));
        QVERIFY(s->mirrorX());
        QVERIFY2(near(points(*s), mapped(before, [&](const QPointF& p) { return QPointF(p.x(), 2 * cy - p.y()); })),
                 qPrintable(QStringLiteral("mirrorX at %1").arg(s->angle())));
        QVERIFY(s->mirrorX());
        QVERIFY(s->rotate());   // counter-clockwise on the screen: right goes up
        QVERIFY2(near(points(*s), mapped(before, [&](const QPointF& p) { return QPointF(cx + (p.y() - cy), cy - (p.x() - cx)); })),
                 qPrintable(QStringLiteral("rotate to %1").arg(s->angle())));
      }
    }
  }

  void aStarAndAPolygon()
  {
    RegularPolygon star(RegularPolygon::Kind::Star);
    star.setBox(QRect(0, 0, 101, 101));
    QVERIFY(star.isStar());
    QCOMPARE(firstPolygon(star).size(), 11);   // 10 corners, closed
    RegularPolygon hexagon(RegularPolygon::Kind::Polygon);
    hexagon.setBox(QRect(0, 0, 101, 101));
    QCOMPARE(firstPolygon(hexagon).size(), 7);
    hexagon.setField("sides", 8);
    QCOMPARE(firstPolygon(hexagon).size(), 9);
  }

  // A square wave's edges are upright; a sine stays in its box.
  void waveforms()
  {
    WaveformPainting w;
    w.setBox(QRect(QPoint(0, 0), QPoint(100, 40)));
    for (const QPointF& p : firstPolygon(w)) QVERIFY(QRectF(-0.01, -0.01, 100.02, 40.02).contains(p));
    w.setField("shape", int(WaveformPainting::Shape::Square));
    w.setField("cycles", 2.0);
    const QPolygonF square = firstPolygon(w);
    int upright = 0;
    for (int i = 1; i < square.size(); ++i)
      if (qFuzzyCompare(square.at(i).x() + 1, square.at(i - 1).x() + 1) && square.at(i).y() != square.at(i - 1).y()) ++upright;
    QCOMPARE(upright, 3);   // two cycles of a square wave: three edges between them
    w.setField("baseline", true);
    QCOMPARE(w.worldPath().toSubpathPolygons().size(), 2);
  }

  // A callout's pointer: placed below left of it, moved, turned, mirrored
  // with it, part of its outline and of its bounds.
  void aCalloutPointsAtSomething()
  {
    TextBoxPainting c(TextBoxPainting::Kind::Callout);
    QVERIFY(c.hasPointer());
    c.setBox(QRect(QPoint(100, 100), QPoint(240, 160)));
    c.setField("pointer", true);   // (placed as a new one is)
    const QPoint tip = c.pointerTip();
    QVERIFY(tip.x() < 100 && tip.y() > 160);
    QVERIFY(c.boundingRect().contains(tip));
    QCOMPARE(c.worldPath().boundingRect().left(), qreal(tip.x()));   // the wedge is part of the outline
    QCOMPARE(c.worldPath().boundingRect().bottom(), qreal(tip.y()));
    c.moveCenter(10, 20);
    QCOMPARE(c.pointerTip(), tip + QPoint(10, 20));
    QVERIFY(c.getSelected(QPoint(170, 150), 4));   // inside picks it

    TextBoxPainting again;
    QVERIFY(again.load(c.save()));
    QCOMPARE(again.pointerTip(), c.pointerTip());
    QVERIFY(again.hasPointer());
  }

  void aDimensionSaysItsLength()
  {
    DimensionPainting d;
    d.setPoints(QPoint(0, 0), QPoint(100, 0), -20);
    QCOMPARE(d.label(), QStringLiteral("100"));
    d.setField("scale", 0.1);
    d.setField("unit", "mm");
    d.setField("decimals", 1);
    QCOMPARE(d.label(), QStringLiteral("10.0 mm"));
    d.setField("text", "L1");
    QCOMPARE(d.label(), QStringLiteral("L1"));
    QVERIFY(d.boundingRect().top() < -20);   // the line above the points, the label above it
    QVERIFY(d.getSelected(QPoint(50, -20), 3));
    QVERIFY(!d.getSelected(QPoint(50, 30), 3));
    QVERIFY(d.mirrorX());
    QCOMPARE(d.offset(), 20);
    auto again = reloaded(&d);
    QVERIFY(again != nullptr);
    QCOMPARE(static_cast<DimensionPainting*>(again.get())->label(), QStringLiteral("L1"));
  }

  void aFormulaIsTypeset()
  {
    FormulaPainting f;
    const QSizeF size = f.formulaSize();
    QVERIFY(size.width() > 20 && size.height() > 10);
    QVERIFY(!f.image(Qt::black, 2.0).isNull());
    f.setField("size", f.size() * 2);
    QVERIFY(f.formulaSize().width() > size.width() * 1.6);
    const QRect before = f.boundingRect();
    QVERIFY(f.rotate());
    QCOMPARE(f.angle(), 90);
    QCOMPARE(f.boundingRect().width(), before.height());
    SymbolPrimitives parts;
    QVERIFY(f.symbolPrimitives(parts));
    QCOMPARE(parts.images.size(), 1);
    qDeleteAll(parts.images);
  }

  // Drawn in a subcircuit's symbol, they reach its instances: polylines,
  // texts, an image - and turn with them.
  void inASymbolTheyReachTheInstances()
  {
    RegularPolygon triangle(RegularPolygon::Kind::Triangle);
    triangle.setBox(QRect(QPoint(-30, -30), QPoint(30, 30)));
    triangle.setField("lineColour", QColor(255, 0, 0));
    TextBoxPainting label(TextBoxPainting::Kind::Block);
    label.setBox(QRect(QPoint(-20, 40), QPoint(40, 60)));
    label.setField("text", "OPA");
    FormulaPainting formula;
    formula.moveCenter(0, 70);
    WaveformPainting wave;
    wave.setBox(QRect(QPoint(-60, -60), QPoint(-20, -40)));
    const QString lines = "  <" + triangle.save() + ">\n  <" + label.save() + ">\n  <" + formula.save() + ">\n  <"
                          + wave.save() + ">\n";
    const QString path = write("amp.sch", subcircuitWith(lines));
    auto sub = std::make_unique<Subcircuit>();
    sub->Props.front()->Value = path;
    sub->recreate();
    QCOMPARE(sub->Ports.count(), 2);
    QCOMPARE(sub->Polylines.count(), 3);   // the triangle, the box, the wave
    QVERIFY(sub->Polylines.first()->closed);
    QCOMPARE(sub->Polylines.first()->pen.color(), QColor(255, 0, 0));
    QCOMPARE(sub->Texts.count(), 1);
    QCOMPARE(sub->Texts.first()->s, QStringLiteral("OPA"));
    QCOMPARE(sub->Images.count(), 1);

    QImage canvas(400, 400, QImage::Format_ARGB32);
    canvas.fill(Qt::white);
    QPainter painter(&canvas);
    painter.translate(200 - sub->cx, 200 - sub->cy);
    sub->paint(&painter);
    painter.end();
    QVERIFY(pixelsOf(canvas, QColor(255, 0, 0)) > 30);

    const QPointF before = sub->Polylines.first()->points.front();
    QVERIFY(sub->rotate());
    QCOMPARE(sub->Polylines.first()->points.front(), QPointF(before.y(), -before.x()));
  }

  // In a schematic: loaded, and saved back as they were.
  void inASchematic()
  {
    RoundedRectangle r(true);
    r.setBox(QRect(QPoint(0, 0), QPoint(80, 40)));
    TablePainting t;
    t.setBox(QRect(QPoint(0, 100), QPoint(210, 166)));
    DimensionPainting d;
    d.setPoints(QPoint(0, 200), QPoint(120, 200), -15);
    BracePainting b;
    b.setBox(QRect(QPoint(300, 0), QPoint(320, 100)));
    const QStringList lines{r.save(), t.save(), d.save(), b.save()};
    QString text = subcircuitWith(QString());
    QString painted;
    for (const QString& l : lines) painted += "  <" + l + ">\n";
    text.replace("<Paintings>\n</Paintings>\n", "<Paintings>\n" + painted + "</Paintings>\n");
    const QString path = write("drawn.sch", text);
    auto doc = std::make_unique<Schematic>(nullptr, path);
    QVERIFY(doc->load());
    QCOMPARE(int(doc->a_Paintings->size()), 4);
    QStringList saved;
    for (Painting* p : *doc->a_Paintings) saved << p->save();
    QCOMPARE(saved, lines);
  }

  // The dialog: the fields, a preview, what was changed kept.
  void theDialog()
  {
    RoundedRectangle r;
    r.setBox(QRect(QPoint(0, 0), QPoint(80, 40)));
    bool changed = false;
    answering([&] { changed = r.Dialog(nullptr); },
              [](QWidget* w) {
                auto* d = qobject_cast<QDialog*>(w);
                if (d == nullptr || d->objectName() != "paintingDialog") return false;
                if (d->findChild<QWidget*>("paintingPreview") == nullptr) return false;
                d->findChild<QSpinBox*>("field_radius")->setValue(3);
                d->findChild<QCheckBox*>("field_filled")->setChecked(true);
                d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                return true;
              });
    QVERIFY(changed);
    QCOMPARE(r.radius(), 3);
    QVERIFY(r.isFilled());
    // Cancelled: nothing changes.
    answering([&] { changed = r.Dialog(nullptr); },
              [](QWidget* w) {
                auto* d = qobject_cast<QDialog*>(w);
                if (d == nullptr) return false;
                d->findChild<QSpinBox*>("field_radius")->setValue(30);
                d->reject();
                return true;
              });
    QVERIFY(!changed);
    QCOMPARE(r.radius(), 3);

    // A table's rows and columns size its cells.
    TablePainting t;
    answering([&] { t.Dialog(nullptr); },
              [](QWidget* w) {
                auto* d = qobject_cast<QDialog*>(w);
                if (d == nullptr) return false;
                d->findChild<QSpinBox*>("field_rows")->setValue(5);
                d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                return true;
              });
    QCOMPARE(t.rows(), 5);
    QCOMPARE(t.cell(1, 0), QStringLiteral("R1"));
  }
};

QTEST_MAIN(TestNewPaintings)
#include "test_new_paintings.moc"
