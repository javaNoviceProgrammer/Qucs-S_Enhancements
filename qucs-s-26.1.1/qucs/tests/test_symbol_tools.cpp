/*
 * What a subcircuit's symbol can be made of and what it shows: the pin
 * names and directions an instance draws, drawing the symbol anew,
 * taking one from a file, the order of the pins, and the polyline
 * painting - which, like every other drawing, reaches the instances.
 */
#include <QtTest>
#include <QPainter>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>

#include "config.h"
#include "element.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "schematic.h"
#include "components/component.h"
#include "components/subcircuit.h"
#include "dialogs/pinorderdialog.h"
#include "extsimkernels/spicecompat.h"
#include "paintings/paintings.h"
#include "isolated_settings.h"

namespace {

// A subcircuit with three ports: two on labelled nets, one on a bare
// one, and each with the port type the caller asks for.
QString subcircuit(const QStringList& types = {"analog", "analog", "analog"})
{
    auto type = [&](int i) { return i < types.size() ? types.at(i) : QStringLiteral("analog"); };

    return "<Qucs Schematic " PACKAGE_VERSION ">\n"
           "<Properties>\n</Properties>\n"
           "<Symbol>\n</Symbol>\n"
           "<Components>\n"
         + QStringLiteral("  <Port P1 1 100 100 -23 12 0 0 \"1\" 1 \"%1\" 0>\n").arg(type(0))
         + QStringLiteral("  <Port P2 1 400 100 4 -42 0 2 \"2\" 1 \"%1\" 0>\n").arg(type(1))
         + QStringLiteral("  <Port P3 1 100 200 -23 12 0 0 \"3\" 1 \"%1\" 0>\n").arg(type(2))
         + "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
           "  <R R2 1 250 200 -26 15 0 0 \"2 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
           "</Components>\n"
           "<Wires>\n"
           "  <100 100 220 100 \"in\" 140 70 0 \"\">\n"
           "  <280 100 400 100 \"out\" 320 70 0 \"\">\n"
           "  <100 200 220 200 \"\" 0 0 0 \"\">\n"
           "  <280 200 400 200 \"\" 0 0 0 \"\">\n"
           "  <400 100 400 200 \"\" 0 0 0 \"\">\n"
           "</Wires>\n"
           "<Diagrams>\n</Diagrams>\n"
           "<Paintings>\n</Paintings>\n";
}

// How many pixels of a colour a picture holds.
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

QImage draw(Component* component, int size = 400)
{
    QImage canvas(size, size, QImage::Format_ARGB32);
    canvas.fill(Qt::white);
    QPainter painter(&canvas);
    painter.translate(size / 2 - component->cx, size / 2 - component->cy);
    component->paint(&painter);
    painter.end();
    return canvas;
}

//! Whether the picture holds this text, by what the same text looks like
//! drawn on its own: the count of dark pixels has to have gone up by
//! about that much.
int darkPixels(const QImage& image) { return pixelsOf(image, Qt::black, 90); }

QStringList symbolLines(const QString& path, const QString& startsWith = QString())
{
    QStringList lines;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return lines;

    QTextStream stream(&file);
    bool inSymbol = false;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line == "<Symbol>") { inSymbol = true; continue; }
        if (line == "</Symbol>") break;
        if (!inSymbol) continue;
        if (startsWith.isEmpty() || line.startsWith("<" + startsWith)) lines << line;
    }
    return lines;
}

} // namespace

class TestSymbolTools : public QObject
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

    // A saved subcircuit, its symbol filled in as the application does.
    QString subcircuitFile(const QString& name, const QStringList& types = {})
    {
        const QString path = write(name, types.isEmpty() ? subcircuit() : subcircuit(types));
        if (path.isEmpty()) return QString();

        auto doc = std::make_unique<Schematic>(nullptr, path);
        if (!doc->load()) return QString();
        doc->createSubcircuitSymbol();   // as entering symbol mode does
        doc->save();
        return path;
    }

    std::unique_ptr<Subcircuit> instanceOf(const QString& file)
    {
        auto sub = std::make_unique<Subcircuit>();
        sub->Props.front()->Value = file;
        sub->recreate();
        return sub;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.ShowPinNames = true;
        QucsSettings.ShowPinDirections = false;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        QucsSettings.QucsWorkDir.setPath(dir.path());
        Module::registerModules();
        QVERIFY(QucsMain == nullptr);
    }

    void cleanup()
    {
        QucsSettings.ShowPinNames = true;
        QucsSettings.ShowPinDirections = false;
    }

    // ---- 1: the instance writes the names of its pins ---------------

    void anInstanceKnowsWhatItsPinsAreCalled()
    {
        const QString file = subcircuitFile("named.sch");
        QVERIFY(!file.isEmpty());

        auto sub = instanceOf(file);
        QCOMPARE(sub->Ports.count(), 3);

        QStringList names;
        for (const Port* port : sub->Ports) names << port->Name;
        QCOMPARE(names, (QStringList{"in", "out", "P3"}));
    }

    void thePinNamesAreDrawnOnlyWhenAskedFor()
    {
        const QString file = subcircuitFile("drawn.sch");
        QVERIFY(!file.isEmpty());
        auto sub = instanceOf(file);

        QucsSettings.ShowPinNames = false;
        const int without = darkPixels(draw(sub.get()));
        QucsSettings.ShowPinNames = true;
        const int with = darkPixels(draw(sub.get()));

        QVERIFY2(with > without + 40, qPrintable(QStringLiteral("%1 -> %2").arg(without).arg(with)));
    }

    // The names go with the drawing when the component is turned.
    void thePinNamesTurnWithTheComponent()
    {
        const QString file = subcircuitFile("turned.sch");
        QVERIFY(!file.isEmpty());
        auto sub = instanceOf(file);

        const int upright = darkPixels(draw(sub.get()));
        QVERIFY(sub->rotate());
        const int turned = darkPixels(draw(sub.get()));

        // The same marks, laid out the other way: about as much ink.
        QVERIFY2(qAbs(turned - upright) < upright / 2,
                 qPrintable(QStringLiteral("%1 -> %2").arg(upright).arg(turned)));
    }

    // ---- 2: which way a pin points ----------------------------------

    void anInstanceKnowsWhichWayItsPinsPoint()
    {
        const QString file = subcircuitFile("directed.sch", {"in", "out", "inout"});
        QVERIFY(!file.isEmpty());

        auto sub = instanceOf(file);
        QStringList directions;
        for (const Port* port : sub->Ports) directions << port->Dir;
        QCOMPARE(directions, (QStringList{"in", "out", "inout"}));
    }

    void theDirectionMarksAreOffByDefault()
    {
        const QString file = subcircuitFile("marks.sch", {"in", "out", "inout"});
        QVERIFY(!file.isEmpty());
        auto sub = instanceOf(file);

        const QColor mark(Qt::darkGreen);
        QCOMPARE(pixelsOf(draw(sub.get()), mark, 30), 0);

        QucsSettings.ShowPinDirections = true;
        QVERIFY(pixelsOf(draw(sub.get()), mark, 30) > 20);
    }

    // With the directions shown, a symbol drawn anew puts the inputs on
    // one side and the outputs on the other.
    void aSymbolDrawnAnewSortsTheSidesByDirection()
    {
        QucsSettings.ShowPinDirections = true;

        const QString path = write("sides.sch", subcircuit({"in", "out", "inout"}));
        QVERIFY(!path.isEmpty());

        auto doc = std::make_unique<Schematic>(nullptr, path);
        QVERIFY(doc->load());
        QVERIFY(doc->recreateSubcircuitSymbol());

        QMap<QString, int> xOf;
        for (Painting* painting : doc->a_SymbolPaints)
            if (painting->Name == ".PortSym ")
                xOf.insert(static_cast<PortSymbol*>(painting)->nameStr, painting->cx);

        QCOMPARE(xOf.size(), 3);
        QVERIFY2(xOf.value("in") < 0 && xOf.value("out") > 0 && xOf.value("P3") < 0,
                 qPrintable(QStringLiteral("in=%1 out=%2 P3=%3")
                                .arg(xOf.value("in")).arg(xOf.value("out")).arg(xOf.value("P3"))));
    }

    // ---- 3: drawing the symbol anew ---------------------------------

    void theSymbolCanBeDrawnAnew()
    {
        const QString path = write("anew.sch", subcircuit());
        QVERIFY(!path.isEmpty());

        auto doc = std::make_unique<Schematic>(nullptr, path);
        QVERIFY(doc->load());
        QVERIFY(doc->createSubcircuitSymbol());

        // Something the user drew, and a port moved out of the way.
        doc->a_SymbolPaints.push_back(new GraphicLine(-100, -100, 100, 100, QPen(Qt::red, 1)));
        for (Painting* painting : doc->a_SymbolPaints)
            if (painting->Name == ".PortSym ") { painting->moveCenterTo(500, 500); break; }

        QVERIFY(doc->recreateSubcircuitSymbol());

        for (Painting* painting : doc->a_SymbolPaints)
            QVERIFY2(painting->cx < 400, qPrintable(painting->save().left(40)));

        int ports = 0, lines = 0;
        for (Painting* painting : doc->a_SymbolPaints) {
            if (painting->Name == ".PortSym ") ++ports;
            if (painting->Name == "Line ") ++lines;
        }
        QCOMPARE(ports, 3);
        QCOMPARE(lines, 4 + 3);   // the box and one stub per port
    }

    // A box wide enough for the names it now carries.
    void theNewSymbolMakesRoomForThePinNames()
    {
        const QString path = write("wide.sch", subcircuit());
        QVERIFY(!path.isEmpty());

        auto doc = std::make_unique<Schematic>(nullptr, path);
        QVERIFY(doc->load());

        const auto symbolWidth = [&doc] {
            int left = 0, right = 0;
            for (Painting* painting : doc->a_SymbolPaints)
                if (painting->Name == ".PortSym ") {
                    left = std::min(left, painting->cx);
                    right = std::max(right, painting->cx);
                }
            return right - left;
        };

        QucsSettings.ShowPinNames = false;
        QVERIFY(doc->recreateSubcircuitSymbol());
        const int narrow = symbolWidth();

        QucsSettings.ShowPinNames = true;
        QVERIFY(doc->recreateSubcircuitSymbol());
        const int wide = symbolWidth();

        QVERIFY2(wide > narrow, qPrintable(QStringLiteral("%1 -> %2").arg(narrow).arg(wide)));
    }

    // ---- 4: the order of the pins -----------------------------------

    void thePinOrderCanBeChanged()
    {
        const QString path = write("order.sch", subcircuit());
        QVERIFY(!path.isEmpty());

        auto doc = std::make_unique<Schematic>(nullptr, path);
        QVERIFY(doc->load());
        QVERIFY(doc->createSubcircuitSymbol());

        PinOrderDialog dialog(doc.get());
        QCOMPARE(dialog.pinCount(), 3);
        QCOMPARE(dialog.pinNames(), (QStringList{"in", "out", "P3"}));

        dialog.movePin(2, -1);   // P3 up, in front of "out"
        QCOMPARE(dialog.pinNames(), (QStringList{"in", "P3", "out"}));
        dialog.applyOrder();
        QVERIFY(dialog.changedAnything());

        QMap<QString, QString> numberOf;
        for (Component* c : doc->a_DocComps)
            if (c->Model == "Port") numberOf.insert(doc->portPinName(c), c->Props.first()->Value);
        QCOMPARE(numberOf.value("in"), QStringLiteral("1"));
        QCOMPARE(numberOf.value("P3"), QStringLiteral("2"));
        QCOMPARE(numberOf.value("out"), QStringLiteral("3"));

        // And the symbol's pins carry the new numbers.
        QMap<QString, QString> symbolNumberOf;
        for (Painting* painting : doc->a_SymbolPaints)
            if (painting->Name == ".PortSym ") {
                auto* port = static_cast<PortSymbol*>(painting);
                symbolNumberOf.insert(port->nameStr, port->numberStr);
            }
        QCOMPARE(symbolNumberOf.value("P3"), QStringLiteral("2"));
        QCOMPARE(symbolNumberOf.value("out"), QStringLiteral("3"));
    }

    // ---- 5: the polyline painting -----------------------------------

    void aPolylineSavesAndLoads()
    {
        PolylinePainting drawn;
        QVERIFY(drawn.load("Polyline 4 0 0 10 20 30 20 40 0 #000080 2 1 #c0c0c0 1 0 0"));
        QCOMPARE(int(drawn.corners().size()), 4);
        QCOMPARE(drawn.corners().front(), QPoint(0, 0));
        QCOMPARE(drawn.corners().back(), QPoint(40, 0));
        QVERIFY(!drawn.isClosed());
        QCOMPARE(drawn.boundingRect(), QRect(0, 0, 40, 20));

        PolylinePainting again;
        QVERIFY(again.load(drawn.save().trimmed()));
        QCOMPARE(again.corners(), drawn.corners());
        QCOMPARE(again.save(), drawn.save());

        PolylinePainting closed;
        QVERIFY(closed.load("Polyline 3 0 0 20 0 10 20 #000080 2 1 #c0c0c0 1 1 1"));
        QVERIFY(closed.isClosed());
    }

    void aPolylineTurnsAndMirrors()
    {
        PolylinePainting drawn;
        QVERIFY(drawn.load("Polyline 3 0 0 20 0 20 10 #000080 2 1 #c0c0c0 1 0 0"));

        const QPoint centre(drawn.cx, drawn.cy);
        QVERIFY(drawn.rotate());
        QCOMPARE(int(drawn.corners().size()), 3);
        QCOMPARE(drawn.boundingRect().size(), QSize(10, 20));

        QVERIFY(drawn.mirrorY());
        QCOMPARE(QPoint(drawn.cx, drawn.cy), centre);   // mirrored in place
    }

    // A polyline in a symbol is drawn by every instance of it, and turns
    // with them like the lines around it.
    void aPolylineInASymbolReachesTheInstances()
    {
        QString text = subcircuit();
        text.replace("<Symbol>\n</Symbol>",
                     "<Symbol>\n"
                     "  <.PortSym -30 0 1 0 in>\n"
                     "  <.PortSym 30 0 2 180 out>\n"
                     "  <.PortSym -30 30 3 0 P3>\n"
                     "  <Polyline 4 -20 -20 0 -30 20 -20 0 10 #ff0000 2 1 #c0c0c0 1 1 1>\n"
                     "</Symbol>");
        const QString path = write("polysym.sch", text);
        QVERIFY(!path.isEmpty());

        auto sub = instanceOf(path);
        QCOMPARE(sub->Polylines.count(), 1);
        QCOMPARE(int(sub->Polylines.first()->points.size()), 4);
        QVERIFY(sub->Polylines.first()->closed);

        const QColor red(255, 0, 0);
        QVERIFY(pixelsOf(draw(sub.get()), red, 40) > 20);

        const QPointF before = sub->Polylines.first()->points.front();
        QVERIFY(sub->rotate());
        const QPointF after = sub->Polylines.first()->points.front();
        QCOMPARE(after, QPointF(before.y(), -before.x()));
    }

    // The component panel takes a painting's icon from the resources; a
    // painting registered without one has none painted for it, so a
    // missing file would leave the palette without a picture.
    void theNewPaintingsHaveTheirIcons()
    {
        QString name;
        char* file = nullptr;

        delete PolylinePainting::info(name, file, true);
        QVERIFY2(QFileInfo::exists(misc::getIconPath(QString(file))), file);
        QCOMPARE(name, QStringLiteral("Polyline"));

        delete PolylinePainting::info_filled(name, file, true);
        QVERIFY2(QFileInfo::exists(misc::getIconPath(QString(file))), file);
    }

    // ---- 6: taking a symbol from a file -----------------------------

    void aSymbolCanBeSavedAndTakenBack()
    {
        const QString path = write("source.sch", subcircuit());
        QVERIFY(!path.isEmpty());

        auto source = std::make_unique<Schematic>(nullptr, path);
        QVERIFY(source->load());
        QVERIFY(source->createSubcircuitSymbol());
        // Something recognisable, and a port put somewhere of its own.
        source->a_SymbolPaints.push_back(new GraphicLine(-40, -40, 40, -40, QPen(Qt::red, 3)));
        for (Painting* painting : source->a_SymbolPaints)
            if (painting->Name == ".PortSym "
                && static_cast<PortSymbol*>(painting)->numberStr == "1") {
                painting->moveCenterTo(-70, -70);
                break;
            }

        const QString symbol = dir.filePath("house.sym");
        QVERIFY(source->saveSymbolToFile(symbol));
        QVERIFY(QFile::exists(symbol));

        const QString other = write("target.sch", subcircuit());
        QVERIFY(!other.isEmpty());
        auto target = std::make_unique<Schematic>(nullptr, other);
        QVERIFY(target->load());
        QVERIFY(target->createSubcircuitSymbol());

        QCOMPARE(target->loadSymbolFromFile(symbol), QString());

        int red = 0, ports = 0;
        for (Painting* painting : target->a_SymbolPaints) {
            if (painting->Name == "Line " && painting->save().contains("#ff0000")) ++red;
            if (painting->Name == ".PortSym ") {
                ++ports;
                if (static_cast<PortSymbol*>(painting)->numberStr == "1")
                    QCOMPARE(QPoint(painting->cx, painting->cy), QPoint(-70, -70));
            }
        }
        QCOMPARE(red, 1);
        QCOMPARE(ports, 3);

        // The names stay this schematic's own.
        QStringList names;
        for (Painting* painting : target->a_SymbolPaints)
            if (painting->Name == ".PortSym ") names << static_cast<PortSymbol*>(painting)->nameStr;
        names.sort();
        QCOMPARE(names, (QStringList{"P3", "in", "out"}));
    }

    void aSymbolFileThatIsNotOneIsRefused()
    {
        const QString nonsense = write("nonsense.txt", "hello\n");
        QVERIFY(!nonsense.isEmpty());

        const QString path = write("refuse.sch", subcircuit());
        auto doc = std::make_unique<Schematic>(nullptr, path);
        QVERIFY(doc->load());
        QVERIFY(doc->createSubcircuitSymbol());
        const auto before = doc->a_SymbolPaints.size();

        QVERIFY(!doc->loadSymbolFromFile(nonsense).isEmpty());
        QVERIFY(!doc->loadSymbolFromFile(dir.filePath("nothing-here.sym")).isEmpty());
        QCOMPARE(doc->a_SymbolPaints.size(), before);   // nothing was touched
    }

    void aSavedSymbolIsASymbolFile()
    {
        const QString path = write("plain.sch", subcircuit());
        auto doc = std::make_unique<Schematic>(nullptr, path);
        QVERIFY(doc->load());
        QVERIFY(doc->createSubcircuitSymbol());

        const QString symbol = dir.filePath("plain.sym");
        QVERIFY(doc->saveSymbolToFile(symbol));

        QFile file(symbol);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.startsWith("<Qucs Schematic "));
        QVERIFY(text.contains("<Symbol>"));
        QVERIFY(text.contains("</Symbol>"));
        QCOMPARE(symbolLines(symbol, ".PortSym").size(), 3);
    }
};

int main(int argc, char** argv)
{
    int one = 1;
    QApplication app(one, argv);
    TestSymbolTools test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_symbol_tools.moc"
