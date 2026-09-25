/***************************************************************************
                              schematic.cpp
                             ---------------
    begin                : Sat Mar 3 2006
    copyright            : (C) 2006 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <algorithm>
#include <QString>
#include <unordered_set>

#include "components/vafile.h"
#include "components/verilogfile.h"
#include "components/vhdlfile.h"
#include "diagrams/diagram.h"
#include "main.h"
#include "mouseactions.h"
#include "node.h"
#include "wire.h"
#include "paintings/paintings.h"
#include "schematic.h"
#include "statusbar.h"
#include "ink.h"
#include <QHelpEvent>
#include <QToolTip>
#include "settings.h"
#include "textdoc.h"

#include "misc.h"
#include "qucs_assert.h"

/**
    If \c point does not lie within \c rect then returns a new
    rectangle made by enlarging the source rectangle to include
    the \c point. Otherwise returns a rectangle of the same size.
*/
inline QRect includePoint(const QRect& rect, const QPoint& point) {
  return rect.contains(point)
       ? rect
       : rect.united(QRect{point, point});
}

Schematic::Schematic(QucsApp *App_, const QString &Name_) :
    QucsDoc(App_, Name_),
    a_Wires(&a_DocWires),
    a_DocWires(),
    a_Nodes(&a_DocNodes),
    a_DocNodes(),
    a_Diagrams(&a_DocDiags),
    a_DocDiags(),
    a_Paintings(&a_DocPaints),
    a_DocPaints(),
    a_Components(&a_DocComps),
    a_DocComps(),
    a_SymbolPaints(),
    a_PostedPaintEvents(),
    a_symbolMode(false),
    a_isSymbolOnly(false),
    a_GridX(10),
    a_GridY(10),
    a_ViewX1(0),
    a_ViewY1(0),
    a_ViewX2(1),
    a_ViewY2(1),
    a_showFrame(FrameSize::None),
    a_Frame_Text0(tr("Title")),
    a_Frame_Text1(tr("Drawn By:")),
    a_Frame_Text2(tr("Date:")),
    a_Frame_Text3(tr("Revision:")),
    a_tmpScale(1.0),
    a_tmpViewX1(-200),
    a_tmpViewY1(-200),
    a_tmpViewX2(200),
    a_tmpViewY2(200),
    a_undoActionIdx(0),
    // The 'i' means state for being unchanged.
    a_undoAction((QVector<QString*>() << new QString(" i\n</>\n</>\n</>\n</>\n"))),
    a_undoSymbolIdx(0),
    // The 'i' means state for being unchanged.
    a_undoSymbol((QVector<QString*>() << new QString(" i\n</>\n</>\n</>\n</>\n"))),
    a_previousCursorPosition(),
    a_dragIsOkay(false),
    a_FileInfo(),
    a_Signals(),
    a_PortTypes(),
    a_isAnalog(false),
    a_isVerilog(false),
    a_creatingLib(false)
{
    setFont(QucsSettings.font);
    a_GridColor = _settings::Get().item<QString>("GridColor");

    a_tmpPosX = a_tmpPosY = -100;

    setVScrollBarMode(Q3ScrollView::AlwaysOn);
    setHScrollBarMode(Q3ScrollView::AlwaysOn);
    misc::setWidgetBackgroundColor(viewport(), misc::paperColor());
    viewport()->setMouseTracking(true);
    viewport()->setAcceptDrops(true); // enable drag'n drop

    // to repair some strange  scrolling artefacts
    connect(this, SIGNAL(horizontalSliderReleased()), viewport(), SLOT(update()));
    connect(this, SIGNAL(verticalSliderReleased()), viewport(), SLOT(update()));
    if (App_) {
        connect(this,SIGNAL(signalCursorPosChanged(int, int, QString)),App_,SLOT(printCursorPosition(int, int, QString)));

        connect(this, SIGNAL(horizontalSliderPressed()), App_, SLOT(slotHideEdit()));
        connect(this, SIGNAL(verticalSliderPressed()), App_, SLOT(slotHideEdit()));
        connect(this, SIGNAL(signalUndoState(bool)), App_, SLOT(slotUpdateUndo(bool)));
        connect(this, SIGNAL(signalRedoState(bool)), App_, SLOT(slotUpdateRedo(bool)));
        connect(this, SIGNAL(signalFileChanged(bool)), App_, SLOT(slotFileChanged(bool)));
        connect(this, SIGNAL(signalDocumentRebuilt(Schematic*)), App_, SLOT(slotDocumentRebuilt(Schematic*)));
    }
}

Schematic::~Schematic()
{
    deleteAllElements();
    deleteSymbolPaintings();
    for (auto* pc : a_SymbolComps) delete pc;
    for (auto* pw : a_SymbolWires) delete pw;
    for (auto* pn : a_SymbolNodes) delete pn;
    for (auto* pd : a_SymbolDiags) delete pd;
    qDeleteAll(a_undoAction);
    qDeleteAll(a_undoSymbol);
}

void Schematic::deleteAllElements()
{
    // Wires and components refer to nodes only through raw pointers and
    // nodes own nothing, so the order of destruction does not matter.
    for (auto* pc : a_DocComps) delete pc;
    for (auto* pw : a_DocWires) delete pw;
    for (auto* pn : a_DocNodes) delete pn;
    for (auto* pd : a_DocDiags) delete pd;
    for (auto* pp : a_DocPaints) delete pp;
    a_DocComps.clear();
    a_DocWires.clear();
    a_DocNodes.clear();
    a_DocDiags.clear();
    a_DocPaints.clear();
}

std::unordered_set<const Element*> Schematic::heldElements() const
{
    std::unordered_set<const Element*> held;
    held.insert(a_Components->begin(), a_Components->end());
    held.insert(a_Paintings->begin(), a_Paintings->end());
    for (const Wire* w : *a_Wires) {
        held.insert(w);
        if (w->label() != nullptr) held.insert(w->label());
    }
    for (const Node* n : *a_Nodes) {
        held.insert(n);
        if (n->label() != nullptr) held.insert(n->label());
    }
    for (const Diagram* d : *a_Diagrams) {
        held.insert(d);
        for (const Graph* g : d->Graphs) {
            held.insert(g);
            held.insert(g->Markers.begin(), g->Markers.end());
        }
    }
    return held;
}

bool Schematic::holds(const Element* e) const
{
    if (e == nullptr) return false;
    const auto in = [e](const auto& list) {
        return std::find(list.begin(), list.end(), e) != list.end();
    };
    if (in(*a_Components) || in(*a_Paintings)) return true;
    for (const Wire* w : *a_Wires)
        if (w == e || (w->label() != nullptr && w->label() == e)) return true;
    for (const Node* n : *a_Nodes)
        if (n == e || (n->label() != nullptr && n->label() == e)) return true;
    for (const Diagram* d : *a_Diagrams) {
        if (d == e) return true;
        for (const Graph* g : d->Graphs) {
            if (g == e) return true;
            if (std::find(g->Markers.begin(), g->Markers.end(), e) != g->Markers.end()) return true;
        }
    }
    return false;
}

void Schematic::deleteSymbolPaintings()
{
    for (auto* pp : a_SymbolPaints) delete pp;
    a_SymbolPaints.clear();
}

// ---------------------------------------------------
bool Schematic::createSubcircuitSymbol()
{
    // If the number of ports is not equal, remove or add some.
    const std::size_t port_count = adjustPortNumbers();

    // If a symbol does not yet exist, create one.
    if (a_SymbolPaints.size() != port_count) return false;

    buildDefaultSymbol(port_count);
    return true;
}

// ---------------------------------------------------
// Throws the symbol's drawing away and lays the ports out around a fresh
// box. The ports themselves are kept - their numbers belong to the
// schematic - and their names and directions are read again.
bool Schematic::recreateSubcircuitSymbol()
{
    for (auto painting = a_SymbolPaints.begin(); painting != a_SymbolPaints.end();) {
        if ((*painting)->Name == ".PortSym ") {
            ++painting;
            continue;
        }
        delete *painting;
        painting = a_SymbolPaints.erase(painting);
    }

    const std::size_t port_count = adjustPortNumbers();
    if (port_count == 0) return false;

    buildDefaultSymbol(port_count);
    return true;
}

// ---------------------------------------------------
// A box with the ports around it: inputs on the left and outputs on the
// right when the pin directions are shown, left and right in turn
// otherwise, and wide enough for the names the pins carry.
void Schematic::buildDefaultSymbol(std::size_t port_count)
{
    if (port_count == 0) return;

    std::vector<PortSymbol*> ports;
    ports.reserve(port_count);
    for (auto* painting : a_SymbolPaints)
        if (painting->Name == ".PortSym ") ports.push_back(static_cast<PortSymbol*>(painting));
    if (ports.empty()) return;

    // Which side each port goes on.
    std::vector<PortSymbol*> left, right;
    const bool bySide = QucsSettings.ShowPinDirections
                        && std::any_of(ports.begin(), ports.end(), [](const PortSymbol* p) {
                               const QString d = p->dirStr.toLower();
                               return d == QLatin1String("in") || d == QLatin1String("out");
                           });
    bool onTheLeft = true;
    for (PortSymbol* port : ports) {
        const QString dir = port->dirStr.toLower();
        if (bySide && dir == QLatin1String("in")) {
            left.push_back(port);
        } else if (bySide && dir == QLatin1String("out")) {
            right.push_back(port);
        } else {
            (onTheLeft ? left : right).push_back(port);
            onTheLeft = !onTheLeft;
        }
    }

    const int rows = int(std::max(left.size(), right.size()));
    const int half_height = 30 * (rows - 1) + 10;

    // Room for the names the pins are drawn with inside the box.
    int half_width = 20;
    if (QucsSettings.ShowPinNames) {
        const QFontMetrics metrics(misc::pinFont(), nullptr);
        const auto widest = [&metrics](const std::vector<PortSymbol*>& side) {
            int width = 0;
            for (const PortSymbol* port : side)
                width = std::max(width, metrics.horizontalAdvance(
                                            port->nameStr.isEmpty() ? port->numberStr : port->nameStr));
            return width;
        };
        const int marks = QucsSettings.ShowPinDirections ? 2 * 12 : 0;
        half_width = std::max(half_width, (widest(left) + widest(right) + marks + 24) / 2);
        half_width = ((half_width + 4) / 5) * 5;   // keep the box on the grid
    }

    const auto place = [&](const std::vector<PortSymbol*>& side, bool isLeft) {
        int port_y = 10 - half_height;
        for (PortSymbol* port : side) {
            const int edge = isLeft ? -half_width : half_width;
            const int stub = isLeft ? edge - 10 : edge + 10;
            port->moveCenterTo(stub, port_y);
            if (!isLeft) port->mirrorY();
            a_SymbolPaints.push_back(new GraphicLine(edge, port_y, stub, port_y, QPen(Qt::darkBlue, 2)));
            port_y += 60;
        }
    };
    place(left, true);
    place(right, false);

    a_SymbolPaints.push_front(new ID_Text(-half_width, half_height + 4));

    a_SymbolPaints.push_back(new GraphicLine(-half_width, -half_height, half_width, -half_height, QPen(Qt::darkBlue, 2)));
    a_SymbolPaints.push_back(new GraphicLine(half_width, -half_height, half_width, half_height, QPen(Qt::darkBlue, 2)));
    a_SymbolPaints.push_back(new GraphicLine(-half_width, half_height, half_width, half_height, QPen(Qt::darkBlue, 2)));
    a_SymbolPaints.push_back(new GraphicLine(-half_width, -half_height, -half_width, half_height, QPen(Qt::darkBlue, 2)));
}

// ---------------------------------------------------
void Schematic::becomeCurrent(bool update)
{
    emit signalCursorPosChanged(0, 0, "");

    // update appropriate menu entry (there is none without an application,
    // e.g. in the command-line modes and in the unit tests)
    if (a_App != nullptr) {
        if (a_symbolMode) {
            a_App->symEdit->setText(tr("Edit Schematic"));
            a_App->symEdit->setStatusTip(tr("Edits the schematic"));
            a_App->symEdit->setWhatsThis(tr("Edit Schematic\n\nEdits the schematic"));
        } else {
            a_App->symEdit->setText(tr("Edit Circuit Symbol"));
            a_App->symEdit->setStatusTip(tr("Edits the symbol for this schematic"));
            a_App->symEdit->setWhatsThis(
                tr("Edit Circuit Symbol\n\nEdits the symbol for this schematic"));
        }
    }

    if (a_symbolMode) {
        a_Nodes = &a_SymbolNodes;
        a_Wires = &a_SymbolWires;
        a_Diagrams = &a_SymbolDiags;
        a_Paintings = &a_SymbolPaints;
        a_Components = &a_SymbolComps;

        // "Schematic" is used to edit usual schematic files (containing
        // a schematic and a subcircuit symbol) and *.sym files (which
        // contain *only* a symbol definition). If we're dealing with
        // symbol file, then there is no need to create a subcircuit
        // symbol, a symbol is already there.
        if (!a_DocName.endsWith(".sym") && createSubcircuitSymbol()) {
            updateAllBoundingRect();
            setChanged(true, true);
        }

        emit signalUndoState(a_undoSymbolIdx != 0);
        emit signalRedoState(a_undoSymbolIdx != a_undoSymbol.size() - 1);
    } else {
        a_Nodes = &a_DocNodes;
        a_Wires = &a_DocWires;
        a_Diagrams = &a_DocDiags;
        a_Paintings = &a_DocPaints;
        a_Components = &a_DocComps;

        emit signalUndoState(a_undoActionIdx != 0);
        emit signalRedoState(a_undoActionIdx != a_undoAction.size() - 1);
        if (update)
            reloadGraphs(); // load recent simulation data
    }
}

// ---------------------------------------------------
void Schematic::setName(const QString &Name_)
{
    a_DocName = Name_;
    QFileInfo Info(a_DocName);
    QString base = Info.completeBaseName();
    QString ext = Info.suffix();
    a_DataSet = base + ".dat";
    a_Script = base + ".m";
    if (ext != "dpl")
        a_DataDisplay = base + ".dpl";
    else
        a_DataDisplay = base + ".sch";
}

// ---------------------------------------------------
// Sets the document to be changed or not to be changed.
void Schematic::setChanged(bool c, bool fillStack, char Op)
{
    if ((!a_DocChanged) && c)
        emit signalFileChanged(true);
    else if (a_DocChanged && (!c))
        emit signalFileChanged(false);
    a_DocChanged = c;
    if (c)
        emit signalEdited();

    a_showBias = -1; // schematic changed => bias points may be invalid

    if (!fillStack)
        return;

    // ................................................
    if (a_symbolMode) { // for symbol edit mode
        while (a_undoSymbol.size() > a_undoSymbolIdx + 1) {
            delete a_undoSymbol.last();
            a_undoSymbol.pop_back();
        }

        a_undoSymbol.append(new QString(createSymbolUndoString(Op)));
        a_undoSymbolIdx++;

        emit signalUndoState(true);
        emit signalRedoState(false);

        // The current state must always stay on the stack, whatever the
        // configured depth is (0 would pop the entry just pushed).
        while (static_cast<unsigned int>(a_undoSymbol.size())
               > std::max(1u, QucsSettings.maxUndo)) { // "while..." because
            delete a_undoSymbol.first();
            a_undoSymbol.pop_front();
            a_undoSymbolIdx--;
        }
        return;
    }

    // ................................................
    // for schematic edit mode
    while (a_undoAction.size() > a_undoActionIdx + 1) {
        delete a_undoAction.last();
        a_undoAction.pop_back();
    }

    if (Op == 'm') { // only one for move marker
        if (a_undoAction.at(a_undoActionIdx)->at(0) == Op) {
            delete a_undoAction.last();
            a_undoAction.pop_back();
            a_undoActionIdx--;
        }
    }
    // ...and one for a sequence of cursor-key moves: the previous step of
    // the sequence is replaced by the state after this key press
    if (Op == 'k' && a_keyboardMoveOpen && a_undoAction.at(a_undoActionIdx)->at(0) == Op) {
        delete a_undoAction.last();
        a_undoAction.pop_back();
        a_undoActionIdx--;
    }
    a_keyboardMoveOpen = (Op == 'k');

    a_undoAction.append(new QString(createUndoString(Op)));
    a_undoActionIdx++;

    emit signalUndoState(true);
    emit signalRedoState(false);

    while (static_cast<unsigned int>(a_undoAction.size())
           > std::max(1u, QucsSettings.maxUndo)) { // "while..." because
        delete a_undoAction.first();   // "maxUndo" could be decreased meanwhile
        a_undoAction.pop_front();
        a_undoActionIdx--;
    }
    return;
}

// -----------------------------------------------------------
bool Schematic::sizeOfFrame(int &xall, int &yall)
{
    // Values exclude border of 1.5cm at each side.
    switch (a_showFrame) {
    // DIN A STANDARD FORMATS
    case FrameSize::A5_Landscape: xall = 1020; yall =  765; break; // DIN A5 landscape
    case FrameSize::A5_Portrait:  xall =  765; yall = 1020; break; // DIN A5 portrait
    case FrameSize::A4_Landscape: xall = 1530; yall = 1020; break; // DIN A4 landscape
    case FrameSize::A4_Portrait:  xall = 1020; yall = 1530; break; // DIN A4 portrait
    case FrameSize::A3_Landscape: xall = 2295; yall = 1530; break; // DIN A3 landscape
    case FrameSize::A3_Portrait:  xall = 1530; yall = 2295; break; // DIN A3 portrait

    // These standard sizes were implemented later (2025), so the code values need to be > 8
    // to avoid breaking backward compatibility with older versions of Qucs-S.
    case FrameSize::A6_Landscape: xall =  660; yall =  465; break; // DIN A6 landscape
    case FrameSize::A6_Portrait:  xall =  465; yall =  660; break; // DIN A6 portrait
    // A7 and above formats are too small and the title box doesn't fit in the frame
    // A0, A1, and A2 are huge

    // US letter format
    case FrameSize::Letter_Landscape: xall = 1414; yall = 1054; break; // Letter landscape
    case FrameSize::Letter_Portrait:  xall = 1054; yall = 1414; break; // Letter portrait

    case FrameSize::None:
    default:
        return false;
    }
    return true;
}



void Schematic::paintFrame(QPainter* painter) {
    // dimensions:  X cm / 2.54 * 144
    int frame_width, frame_height;
    if (!sizeOfFrame(frame_width, frame_height))
        return;

    painter->save();
    painter->setPen(qucs_s::ink::on(QPen(Qt::darkGray, 1)));

    // Width of stripe along frame border in column and row labels are placed
    const int frame_margin = painter->fontMetrics().lineSpacing() + 4;

    // Outer rect
    painter->drawRect(0, 0, frame_width, frame_height);
    // a bit smaller than outer rect
    painter->drawRect(frame_margin, frame_margin, frame_width - 2 * frame_margin, frame_height - 2 * frame_margin);

    // Column labels
    {
      const int h_step = frame_width / ((frame_width + 127) / 255);
      uint column_number = 1;

      for (int x = h_step; x <= frame_width; x += h_step) {
        painter->drawLine(x, 0, x, frame_margin);
        painter->drawLine(x, frame_height - frame_margin, x, frame_height);

        auto cn = QString::number(column_number);
        auto tx = x - h_step / 2 + 5;
        painter->drawText(tx, 3, 1, 1, Qt::TextDontClip, cn);
        painter->drawText(tx, frame_height - frame_margin + 3, 1, 1, Qt::TextDontClip, cn);

        column_number++;
      }
    }

    // Row labels
    {
      const int v_step = frame_height / ((frame_height + 127) / 255);
      char row_letter = 'A';

      for (int y = v_step; y <= frame_height; y += v_step) {
        painter->drawLine(0, y, frame_margin, y);
        painter->drawLine(frame_width - frame_margin, y, frame_width, y);

        auto rl = QString::fromLatin1(&row_letter, 1);
        auto ty = y - v_step/2 + 5;
        painter->drawText(5, ty, rl);
        painter->drawText(frame_width - frame_margin + 5, ty, rl);

        row_letter++;
      }
    }

    // draw text box with text
    int x1_ = frame_width - 340 - frame_margin;
    int y1_ = frame_height - 3 - frame_margin;
    int x2_ = frame_width - frame_margin - 3;
    int y2_ = frame_height - frame_margin - 3;

    const int d = 6;
    const double z = 200.0;
    y1_ -= painter->fontMetrics().lineSpacing() + d;
    painter->drawLine(x1_, y1_, x2_, y1_);
    painter->drawText(x1_ + d, y1_ + (d >> 1), 1, 1, Qt::TextDontClip, a_Frame_Text2);
    painter->drawLine(x1_ + z, y1_, x1_ + z, y1_ + painter->fontMetrics().lineSpacing() + d);
    painter->drawText(x1_ + d + z, y1_ + (d >> 1), 1, 1, Qt::TextDontClip, a_Frame_Text3);
    y1_ -= painter->fontMetrics().lineSpacing() + d;
    painter->drawLine(x1_, y1_, x2_, y1_);
    painter->drawText(x1_ + d, y1_ + (d >> 1), 1, 1, Qt::TextDontClip, a_Frame_Text1);
    y1_ -= (a_Frame_Text0.count('\n') + 1) * painter->fontMetrics().lineSpacing() + d;
    painter->drawRect(x2_, y2_, x1_ - x2_ - 1, y1_ - y2_ - 1);
    painter->drawText(x1_ + d, y1_ + (d >> 1), 1, 1, Qt::TextDontClip, a_Frame_Text0);

    painter->restore();
}

// -----------------------------------------------------------
// Is called when the content (schematic or data display) has to be drawn.
void Schematic::drawContents(QPainter *p, int, int, int, int)
{
    QTransform trf{p->transform()};
    trf
        .scale(a_Scale, a_Scale)
        .translate(-a_ViewX1, -a_ViewY1);
    p->setTransform(trf);

    auto renderHints = p->renderHints();
    renderHints
        .setFlag(QPainter::Antialiasing)
        .setFlag(QPainter::TextAntialiasing)
        .setFlag(QPainter::SmoothPixmapTransform);
    p->setRenderHints(renderHints);

    // What is drawn on the canvas is drawn on its paper: on a dark one,
    // the colours meant for light paper are fitted to it (ink.h).
    const qucs_s::ink::Paper paper(viewport()->palette().color(viewport()->backgroundRole()));

    p->setFont(QucsSettings.font);
    drawGrid(p);

    if (!a_symbolMode)
        paintFrame(p);

    drawElements(p);
    if (a_showBias > 0) {
        drawDcBiasPoints(p);
    }

    drawPostPaintEvents(p);
}

Schematic::Net Schematic::netOf(Wire* start) const {
    Net net;
    if (start == nullptr) return net;
    std::vector<Node*> todo;
    auto reach = [&](Node* n) { if (n != nullptr && !net.nodes.count(n)) todo.push_back(n); };
    net.wires.insert(start);
    reach(start->Port1);
    reach(start->Port2);

    // Labels seen so far, and whether a ground is on the net: what joins
    // wires that do not touch.
    std::unordered_set<QString> labels;
    bool grounded = false;
    auto noteLabel = [&](const Conductor* c) {
        if (c->hasLabel() && !c->label()->Name.isEmpty()) labels.insert(c->label()->Name);
    };
    noteLabel(start);

    for (;;) {
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (!net.nodes.insert(n).second) continue;
            noteLabel(n);
            for (Wire* w : n->wires()) {
                if (!net.wires.insert(w).second) continue;
                noteLabel(w);
                reach(w->Port1);
                reach(w->Port2);
            }
            for (Component* c : n->components())
                if (c->Model == QLatin1String("GND")) grounded = true;
        }
        // Bring in what the labels and the ground join; go round again
        // when that reached something new.
        const size_t before = net.nodes.size() + net.wires.size();
        for (auto* w : *a_Wires) {
            if (net.wires.count(w)) continue;
            if (w->hasLabel() && labels.count(w->label()->Name)) { reach(w->Port1); reach(w->Port2); }
        }
        for (auto* n : *a_Nodes) {
            if (net.nodes.count(n)) continue;
            bool joins = n->hasLabel() && labels.count(n->label()->Name);
            if (!joins && grounded)
                for (Component* c : n->components())
                    if (c->Model == QLatin1String("GND")) { joins = true; break; }
            if (joins) reach(n);
        }
        if (todo.empty() && before == net.nodes.size() + net.wires.size()) break;
    }
    return net;
}

Schematic::Net Schematic::selectedNet() const {
    Net net;
    for (auto* wire : *a_Wires) {
        if (!wire->isSelected || net.wires.count(wire)) continue;
        Net part = netOf(wire);
        net.wires.merge(part.wires);
        net.nodes.merge(part.nodes);
    }
    return net;
}

void Schematic::drawNetHighlight(QPainter* painter, const Net& net) {
    if (net.empty()) return;
    painter->save();
    // A translucent glow: readable on a light or a dark paper, and the
    // wires paint over it as usual.
    const QColor glow(255, 140, 0, 120);
    painter->setPen(QPen(glow, 9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (Wire* w : net.wires)
        painter->drawLine(w->x1, w->y1, w->x2, w->y2);
    painter->setPen(Qt::NoPen);
    painter->setBrush(glow);
    for (Node* n : net.nodes)
        painter->drawEllipse(QPoint(n->x(), n->y()), 6, 6);
    painter->restore();
}

void Schematic::drawElements(QPainter* painter) {
    for (auto* component : *a_Components) {
        component->paint(painter);
    }

    if (!a_symbolMode)
        drawNetHighlight(painter, selectedNet());

    for (auto* wire : *a_Wires) {
        wire->paint(painter);
        if (wire->hasLabel()) {
            wire->label()->paint(painter); // separate because of paintSelected
        }
    }

    for (auto* node : *a_Nodes) {
        node->paint(painter);
        if (node->hasLabel()) {
            node->label()->paint(painter); // separate because of paintSelected
        }
    }

    for (auto* diagram : *a_Diagrams) {
        if (qucs_s::ink::darkPaper()) {
            // On dark paper a diagram is a light card, drawn as it is
            // printed: its axes, grid, texts and header bars keep their
            // colours on it.
            const qucs_s::ink::Paper card(Qt::white);
            painter->fillRect(diagram->boundingRect(), Qt::white);
            diagram->paint(painter);
        } else {
            diagram->paint(painter);
        }
    }

    for (auto* painting : *a_Paintings) {
        painting->paint(painter);
    }
}

QList<const qucs_s::oppoint::Device*> Schematic::operatingPointOf(const QString& component) const
{
    QList<const qucs_s::oppoint::Device*> devices;
    for (const qucs_s::oppoint::Device& d : a_operatingPoint)
        if (d.component == component) devices << &d;
    return devices;
}

QString Schematic::operatingPointTooltip(const QPoint& viewportPos)
{
    if (a_showBias <= 0 || a_operatingPoint.isEmpty()) return QString();
    const QPoint at = viewportToModel(viewportPos);
    // The smallest component there that has an operating point (a
    // transistor drawn over a larger symbol's box).
    const Component* chosen = nullptr;
    qint64 chosenArea = 0;
    for (Component* c : a_DocComps) {
        if (!c->getSelected(at.x(), at.y())) continue;
        const QRect box = c->boundingRect();
        const qint64 area = qint64(box.width()) * box.height();
        if (chosen != nullptr && area >= chosenArea) continue;
        if (operatingPointOf(c->Name).isEmpty()) continue;
        chosen = c;
        chosenArea = area;
    }
    return chosen == nullptr ? QString() : qucs_s::oppoint::tooltip(operatingPointOf(chosen->Name));
}

bool Schematic::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::ToolTip && watched == viewport()) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const QString text = operatingPointTooltip(help->pos());
        if (text.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), text, viewport());
        return true;
    }
    return Q3ScrollView::eventFilter(watched, event);
}

Schematic::BiasLabels Schematic::layoutBiasLabels(const QFontMetrics& metrics) const {
    // The labels: a value per node that has one (a current, marked with
    // 0x10, at a probe's or a source's node), in a box of the text's size.
    BiasLabels result;
    for (auto* pn : *a_Nodes) {
        if (pn->Name.isEmpty())
            continue;
        const QString value = misc::formatValue(pn->Name, 4);
        const QRect textRect = metrics.boundingRect(value);
        result.labels << qucs_s::bias::Label{QPoint(pn->cx, pn->cy),
                                             QSize(textRect.width() + 6, textRect.height() + 4),
                                             (pn->x1 & 0x10) != 0};
        result.texts << value;
    }
    if (result.labels.isEmpty())
        return result;

    // What they had better not cover (upstream #1692): each goes beside
    // its node where it covers the least of this.
    qucs_s::bias::Obstacles obstacles;
    for (auto* pc : *a_Components) {
        obstacles.boxes << pc->boundingRect();
        int textWidth, textHeight;
        pc->textSize(textWidth, textHeight);
        if (textWidth > 0 && textHeight > 0)
            obstacles.boxes << QRect(pc->tx, pc->ty, textWidth, textHeight).translated(pc->center());
    }
    for (auto* pw : *a_Wires) {
        obstacles.wires << QLine(pw->x1, pw->y1, pw->x2, pw->y2);
        if (pw->hasLabel())
            obstacles.boxes << pw->label()->boundingRect();
    }
    for (auto* pn : *a_Nodes) {
        obstacles.boxes << QRect(pn->cx - 3, pn->cy - 3, 6, 6);   // its dot
        if (pn->hasLabel())
            obstacles.boxes << pn->label()->boundingRect();
    }
    for (auto* pd : *a_Diagrams)
        obstacles.boxes << pd->boundingRect();
    for (auto* pp : *a_Paintings)
        obstacles.boxes << pp->boundingRect();

    result.placements = qucs_s::bias::place(result.labels, obstacles);
    return result;
}

void Schematic::drawDcBiasPoints(QPainter* painter) {
    painter->save();
    const BiasLabels bias = layoutBiasLabels(painter->fontMetrics());
    auto ink = [&bias](int i) { return bias.labels.at(i).current ? QColor(Qt::darkGreen) : QColor(Qt::blue); };
    // A label set apart from its node: a line to the nearest point of its
    // box. All of them first, so that none runs over another's box.
    for (int i = 0; i < bias.labels.size(); ++i) {
        if (!bias.placements.at(i).leader)
            continue;
        const QRect box = bias.placements.at(i).box;
        const QPoint a = bias.labels.at(i).anchor;
        painter->setPen(QPen(qucs_s::ink::on(ink(i)), 1));   // on the paper, unlike the boxes
        painter->drawLine(a, QPoint(qBound(box.left(), a.x(), box.right()),
                                    qBound(box.top(), a.y(), box.bottom())));
    }
    for (int i = 0; i < bias.labels.size(); ++i) {
        const QRect box = bias.placements.at(i).box;
        painter->setBrush(QBrush(QColor(230,230,230)));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(QRectF(box), 15, 15, Qt::RelativeSize);

        painter->setPen(ink(i));
        painter->drawText(box, Qt::AlignCenter, bias.texts.at(i));
    }
    painter->restore();
}



void Schematic::drawPostPaintEvents(QPainter* painter) {
    painter->save();
    /*
   * The following events used to be drawn from mouseactions.cpp, but since Qt4
   * Paint actions can only be called from within the paint event, so they
   * are put into a QList (PostedPaintEvents) and processed here
   */
    const QColor ink = qucs_s::ink::on(Qt::black);
    for (auto p : a_PostedPaintEvents) {
        QPen pen(ink);
        painter->setPen(ink);
        switch (p.pe) {
        case _NotRop:
            painter->setCompositionMode(QPainter::RasterOp_SourceAndNotDestination);
            break;
        case _Rect:
            painter->drawRect(p.x1, p.y1, p.x2, p.y2);
            break;
        case _SelectionRect:
            pen.setCosmetic(true);
            pen.setStyle(Qt::DashLine);
            pen.setColor(qucs_s::ink::on(QColor(50, 50, 50, 100)));
            painter->setPen(pen);
            painter->fillRect(p.x1, p.y1, p.x2, p.y2, QColor(200, 220, 240, 100));
            painter->drawRect(p.x1, p.y1, p.x2, p.y2);
            break;
        case _Line: {
            painter->save();
            QPen lp{painter->pen()};
            lp.setWidth(p.a == 0 ? 1 : p.a);
            painter->setPen(lp);
            painter->drawLine(p.x1, p.y1, p.x2, p.y2);
            painter->restore();
            break;
        }
        case _Ellipse:
            painter->drawEllipse(p.x1, p.y1, p.x2, p.y2);
            break;
        case _Arc:
            painter->drawArc(p.x1, p.y1, p.x2, p.y2, p.a, p.b);
            break;
        case _DotLine:
            painter->setPen(QPen(ink, 1, Qt::DotLine));
            painter->drawLine(p.x1, p.y1, p.x2, p.y2);
            break;
        case _DotRect:
            painter->setPen(QPen(ink, 1, Qt::DotLine));
            painter->drawRect(p.x1, p.y1, p.x2, p.y2);
            break;
        case _Translate:; //painter2.translate(p.x1, p.y1);
        case _Scale:; //painter2.scale(p.x1,p.y1);
            break;
        }
    }
    a_PostedPaintEvents.clear();
    painter->restore();
}

void Schematic::PostPaintEvent(
    PE pe, int x1, int y1, int x2, int y2, int a, int b, bool PaintOnViewport)
{
    PostedPaintEvent p = {pe, x1, y1, x2, y2, a, b, PaintOnViewport};
    a_PostedPaintEvents.push_back(p);
    viewport()->update();
    update();
}

// ---------------------------------------------------
// The mouse handler the application has chosen, called on its MouseActions:
// the handler and the object fetched first, then the call. Written as one
// expression, (a_App->view->*(a_App->MousePressAction))(...), GCC's
// -fsanitize=vptr reported the first press on a canvas as a member access
// through a stack address that is no QucsApp (CI, Linux, GCC 13) - although
// a_App had passed the same check two lines before, and clang's vptr check
// never fired on the same walk; in two steps it passes.
template <typename Handler, typename... Args>
void Schematic::callView(Handler handler, Args... args)
{
    MouseActions* view = a_App->view;
    (view->*handler)(args...);
}

void Schematic::contentsMouseMoveEvent(QMouseEvent *Event)
{
    a_App->view->dropStaleElements(this);
    const QPoint modelPos = contentsToModel(Event->pos());
    auto xpos = modelPos.x();
    auto ypos = modelPos.y();
    QString text = "";


    if (a_Diagrams == nullptr) return; // fix for crash on document closing; appears time to time

    for (Diagram* diagram : *a_Diagrams) {
        // BUG: Obtaining the diagram type by name is marked as a bug elsewhere (to be solved separately).
        // TODO: Currently only rectangular diagrams are supported.
        if (diagram->getSelected(xpos, ypos) && (diagram->Name == "Rect" || diagram->Name == "Histogram")) {
            // Each axis by its variable, with the unit it tells (statusbar.h).
            const QPointF mouseClickPoint(xpos - diagram->cx, diagram->cy - ypos);
            text = qucs_s::status::readout(diagram, diagram->pointToValue(mouseClickPoint));
            break;
        }
    }

    emit signalCursorPosChanged(xpos, ypos, text);

    // Perform "pan with mouse"
    if (Event->buttons() & Qt::MiddleButton) {
        const QPoint currentCursorPosition = contentsToViewport(Event->pos());

        const int dx = currentCursorPosition.x() - a_previousCursorPosition.x();
        if (dx < 0) {
            scrollRight(std::abs(dx));
        } else if (dx > 0) {
            scrollLeft(dx);
        }

        const int dy = currentCursorPosition.y() - a_previousCursorPosition.y();
        if (dy < 0) {
            scrollDown(std::abs(dy));
        } else if (dy > 0) {
            scrollUp(dy);
        }

        a_previousCursorPosition = currentCursorPosition;
    }

    if (a_App->MouseMoveAction)
        callView(a_App->MouseMoveAction, this, Event);
}

// -----------------------------------------------------------
void Schematic::contentsMousePressEvent(QMouseEvent *Event)
{
    a_App->view->dropStaleElements(this);
    a_App->editText->setHidden(true); // disable text edit of component property
    this->setFocus();
    endKeyboardMove();   // the cursor-key move, if any, is done
    if (    a_App->MouseReleaseAction == &MouseActions::MReleasePaste
        ||  a_App->MouseReleaseAction == &MouseActions::MReleaseMoveFree) {
        return;
    }

    const QPoint inModel = contentsToModel(Event->pos());

    if (Event->button() == Qt::RightButton) {
        // In some modes right-button menu is unavailable
        if (   a_App->MouseMoveAction  == &MouseActions::MMoveMoving2
            || a_App->MousePressAction == &MouseActions::MPressElement
            || a_App->MousePressAction == &MouseActions::MPressWire2)
        {
            if (a_App->MousePressAction) {
                callView(a_App->MousePressAction, this, Event, inModel.x(), inModel.y());
            }
            return;
        }

        // show menu on right mouse button
        a_App->view->rightPressMenu(this, Event, inModel.x(), inModel.y());
        if (a_App->MouseReleaseAction)
            // Is not called automatically because menu has focus.
            callView(a_App->MouseReleaseAction, this, Event);
        return;
    }

    // Begin "pan with mouse" action. Panning starts if *only*
    // the middle button is pressed.
    if (Event->button() == Qt::MiddleButton) {
        a_previousCursorPosition = contentsToViewport(Event->pos());
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (a_App->MousePressAction)
        callView(a_App->MousePressAction, this, Event, inModel.x(), inModel.y());
}

// -----------------------------------------------------------
void Schematic::contentsMouseReleaseEvent(QMouseEvent *Event)
{
    a_App->view->dropStaleElements(this);
    // End "pan with mouse" action.
    if (Event->button() == Qt::MiddleButton) {
        unsetCursor();
        return;
    }

    if (a_App->MouseReleaseAction)
        callView(a_App->MouseReleaseAction, this, Event);
}

// -----------------------------------------------------------
void Schematic::contentsMouseDoubleClickEvent(QMouseEvent *Event)
{
    a_App->view->dropStaleElements(this);
    if (a_App->MouseDoubleClickAction)
        callView(a_App->MouseDoubleClickAction, this, Event);
}

void Schematic::print(QPrinter*, QPainter* painter, bool printAll,
                      bool fitToPage, QMargins margins) {
    painter->save();

    const QRectF pageSize{0, 0, static_cast<double>(painter->device()->width()),
                          static_cast<double>(painter->device()->height())};

    const QRect printedArea = this->printedArea(printAll, margins);

    double scale = 1.0;
    if (fitToPage) {
        scale = std::min(pageSize.width() / printedArea.width(),
                         pageSize.height() / printedArea.height());
    } else {
        QFontInfo printerFontInfo{QFont{QucsSettings.font, painter->device()}};
        QFontInfo schematicFontInfo{QucsSettings.font};

        scale = static_cast<double>(printerFontInfo.pixelSize()) /
                static_cast<double>(schematicFontInfo.pixelSize());
    }
    painter->scale(scale, scale);

    painter->translate(-printedArea.left(), -printedArea.top());

    // put picture in center
    {
        auto w = pageSize.width() / scale;
        if (printedArea.width() <= w) {
            auto d = (w - printedArea.width()) / 2;
            painter->translate(d, 0);
        }

        auto h = pageSize.height() / scale;
        if (printedArea.height() <= h) {
            auto d = (h - printedArea.height()) / 2;
            painter->translate(0, d);
        }
    }

    // User chose a font with size in points while looking at the font
    // on screen. The same font size in *points* equals to different
    // amount of pixels when shown on screen or printed on paper,
    // because underlying painting devices have different resolutions.
    // To preserve the ratio of text sizes and elements' sizes
    // font size has to be set in pixels here. This makes lines, squares,
    // circles, etc. and size of font be measured in same units and scale
    // them equally.
    auto f = QucsSettings.font;
    QFontInfo fi{f};
    f.setPixelSize(fi.pixelSize());
    painter->setFont(f);

    paintSchToViewpainter(painter, printAll);

    painter->restore();
}

QRect Schematic::printedArea(bool printAll, QMargins margins) {
    QRect area = printAll ? allBoundingRect() : withinModelLimit(currentSelection().bounds);

    if (printAll && a_showFrame != FrameSize::None) {
        int frame_width, frame_height;
        sizeOfFrame(frame_width, frame_height);
        area |= QRect{0, 0, frame_width, frame_height};
    }

    return area.marginsAdded(margins);
}

namespace {
// helper to be used in Schematic::paintSchToViewpainter
template <typename T> void draw_preserve_selection(T* elem, QPainter* p) {
    bool selected = elem->isSelected;
    elem->isSelected = false;
    elem->paint(p);
    elem->isSelected = selected;
}
} // namespace

void Schematic::paintSchToViewpainter(QPainter* painter, bool printAll) {
    if (printAll && a_showFrame != FrameSize::None && !a_symbolMode) {
        paintFrame(painter);
    }

    const auto should_draw = [=](Element* drawable) {
      return printAll || drawable->isSelected;
    };

    for (auto* component : *a_Components) {
        if (should_draw(component)) {
            draw_preserve_selection(component, painter);
        }
    }

    for (auto* wire : *a_Wires) {
        if (should_draw(wire)) {
            draw_preserve_selection(wire, painter);
        }

        if (auto* label = wire->label()) {
            if (should_draw(label)) {
                draw_preserve_selection(label, painter);
            }
        }
    }

    for (auto* node : *a_Nodes) {
        if (std::ranges::any_of(node->wires(), should_draw) || std::ranges::any_of(node->components(), should_draw)) {
            draw_preserve_selection(node, painter);
        }

        if (auto* label = node->label()) {
            if (should_draw(label)) {
                draw_preserve_selection(label, painter);
            }
        }
    }

    for (auto* painting : *a_Paintings) {
        if (should_draw(painting)) {
            draw_preserve_selection(painting, painter);
        }
    }

    for (auto* diagram : *a_Diagrams) {
        if (!should_draw(diagram)) {
            continue;
        }

        // if graph or marker is selected, deselect during printing
        for (Graph* pg : diagram->Graphs) {
            if (pg->isSelected) {
                pg->Type |= 1; // remember selection
            }
            pg->isSelected = false;
            for (Marker* pm : pg->Markers) {
                if (pm->isSelected) {
                    pm->Type |= 1; // remember selection
                }
                pm->isSelected = false;
            }
        }
        draw_preserve_selection(diagram, painter);

        // revert selection of graphs and markers
        for (Graph* pg : diagram->Graphs) {
            if (pg->Type & 1) {
                pg->isSelected = true;
            }
            pg->Type &= -2;
            for (Marker* pm : pg->Markers) {
                if (pm->Type & 1) {
                    pm->isSelected = true;
                }
                pm->Type &= -2;
            }
        }
    }

    if (a_showBias > 0) { // show DC bias points in schematic ?
        drawDcBiasPoints(painter);
    }
}

void Schematic::zoomAroundPoint(double offeredScaleChange, QPoint coords, bool viewportRelative=true)
{
    const double desiredScale = a_Scale * offeredScaleChange;
    auto viewportCoords =
        viewportRelative ? coords : coords - QPoint{contentsX(), contentsY()};
    // A button released outside the canvas (it keeps the mouse while
    // dragging) or a wheel turned on its border: zoom around the nearest
    // point of the canvas; renderModel() needs one inside it.
    viewportCoords.setX(std::clamp(viewportCoords.x(), 0, std::max(0, viewport()->width() - 1)));
    viewportCoords.setY(std::clamp(viewportCoords.y(), 0, std::max(0, viewport()->height() - 1)));
    const auto focusPoint = viewportToModel(viewportCoords);
    const auto model = includePoint(modelRect(), focusPoint);

    renderModel(desiredScale, model, focusPoint, viewportCoords);
}

// -----------------------------------------------------------
double Schematic::zoomBy(double s)
{
    // Change scale and keep the point displayed in the center
    // of the viewport at same place after scaling.

    const double newScale = a_Scale * s;
    const auto vpCenter = viewportRect().center();
    const auto centerPoint = viewportToModel(vpCenter);
    const auto model = includePoint(modelRect(), centerPoint);

    return renderModel(newScale, model, centerPoint, vpCenter);
}

void Schematic::centerOn(const QPoint& modelPoint)
{
    renderModel(a_Scale, includePoint(modelRect(), modelPoint), modelPoint, viewportRect().center());
}

// ---------------------------------------------------
void Schematic::showAll()
{
    const auto usedArea = allBoundingRect();

    if (usedArea.isNull()) {
        return;
    }

    const QRect newModelBounds = usedArea.marginsAdded({40, 40, 40, 40});

    // The shape of the model plane may not fit the shape of the viewport,
    // so we looking for a scale value which enables to fit the whole model
    // into the viewport
    const double xScale = static_cast<double>(viewport()->width()) /
                          static_cast<double>(newModelBounds.width());
    const double yScale = static_cast<double>(viewport()->height()) /
                          static_cast<double>(newModelBounds.height());
    const double newScale = std::min(xScale, yScale);

    renderModel(newScale, newModelBounds, newModelBounds.center(), viewportRect().center());
}

// ------------------------------------------------------
void Schematic::zoomToSelection() {
    const auto usedArea = allBoundingRect();

    if (usedArea.isNull()) {
        // No elements present – nothing is selected; quit
        return;
    }

    const QRect selectedBoundingRect(withinModelLimit(currentSelection().bounds));

    if (selectedBoundingRect.width() == 0 || selectedBoundingRect.height() == 0) {
        // If nothing is selected, then what should be shown? Probably it's best
        // to do nothing.
        return;
    }

    const QRect modelBounds = usedArea.marginsAdded({40, 40, 40, 40});

    // Find out the scale at which selected area's longest side would fit
    // into the viewport
    const double xScale = static_cast<double>(viewport()->width()) /
                          static_cast<double>(selectedBoundingRect.width());
    const double yScale = static_cast<double>(viewport()->height()) /
                          static_cast<double>(selectedBoundingRect.height());
    const double newScale = std::min(xScale, yScale);

    renderModel(newScale, modelBounds, selectedBoundingRect.center(), viewportRect().center());
}

// ---------------------------------------------------
void Schematic::showNoZoom()
{
    constexpr double noScale = 1.0;
    const QPoint vpCenter = viewportRect().center();
    const QPoint displayedInCenter = viewportToModel(vpCenter);

    const auto usedArea = allBoundingRect();

    if (usedArea.isNull()) {
      // If there is no elements in schematic, then just set scale 1.0
      // at the place we currently in.
      renderModel(noScale, includePoint(modelRect(), displayedInCenter), displayedInCenter, vpCenter);
      return;
    }

    const QRect newModelBounds = usedArea.marginsAdded({40, 40, 40, 40});

    // If a part of "used" area is currently displayed in the center of the
    // viewport, then keep it in the same place after scaling. Otherwise focus
    // on the center of the used area after scale change.
    if (usedArea.contains(displayedInCenter)) {
        renderModel(noScale, newModelBounds, displayedInCenter, vpCenter);
    } else {
        renderModel(noScale, newModelBounds, usedArea.center(), vpCenter);
    }
 }

void Schematic::enlargeView(const Element* e) {
    const auto br = withinModelLimit(e->boundingRect());

    a_UsedArea = withinModelLimit(a_UsedArea | br);

    QRect newModel = modelRect()
        .united(br.marginsAdded({40, 40, 40, 40}));

    const auto vpCenter = viewportRect().center();
    const auto displayedInCenter = viewportToModel(vpCenter);
    newModel = includePoint(newModel, displayedInCenter);
    renderModel(a_Scale, newModel, displayedInCenter, vpCenter);
 }

 QPoint Schematic::setOnGrid(const QPoint& p) {
   QPoint snappedToGrid{p.x(), p.y()};
   setOnGrid(snappedToGrid.rx(), snappedToGrid.ry());
   return snappedToGrid;
 }

// ---------------------------------------------------
// Sets an arbitrary coordinate onto the next grid coordinate.
void Schematic::setOnGrid(int &x, int &y)
{
    if (x < 0)
        x -= (a_GridX >> 1) - 1;
    else
        x += a_GridX >> 1;
    x -= x % a_GridX;

    if (y < 0)
        y -= (a_GridY >> 1) - 1;
    else
        y += a_GridY >> 1;
    y -= y % a_GridY;
}

bool Schematic::gridShown() const {
    if (QucsDoc::fileSuffix(getDocName()) == QLatin1String("dpl"))
        return a_GridOn;
    switch (QucsSettings.GridMode) {
    case 1: return false;   // hidden in every schematic
    case 2: return true;    // shown in every schematic
    default: return a_GridOn;
    }
}

void Schematic::drawGrid(QPainter* painter) {
    if (!gridShown())
        return;

    painter->save();
    // Painter might have been scaled somewhere upstream in call stack,
    // and we don't grid points to change their size or thickness
    // depending on zoom level. Thus we remove any transformations
    // and draw on "raw" painter, controlling all offsets manually
    painter->setTransform(QTransform{});

    // A grid drawn with pen of 1.0 width reportedly looks good both
    // on standard and HiDPI displays.
    // See here for details https://github.com/ra3xdh/qucs_s/pull/524
    painter->setPen(QPen{ misc::gridColor(a_GridColor), 1.0 });

    {
        // Draw small cross at origin of coordinates
        const QPoint origin = modelToViewport(QPoint{0, 0});
        painter->drawLine(origin.x() - 3, origin.y(), origin.x() + 4, origin.y());  // horizontal stick
        painter->drawLine(origin.x(), origin.y() - 3, origin.x(), origin.y() + 4);  // vertical stick
    }

    // Grid is drawn as a set of nodes, each node looks like a point and a node
    // is located at every horizontal and vertical step:
    // .  .  .  .  .
    // .  .  .  .  .
    // .  .  .  .  .
    // .  .  .  .  .
    //
    // To find out where to start drawing grid nodes, we find a point
    // which is currently shown at the top left corner of the viewport
    // and then find a grid-node nearest to this point. We then convert these
    // grid-node coordinates back to viewport-coordinates. This gives us
    // coordinates of a point somewhere around the top-left corner of the
    // viewport. This point corresponds to a grid-node. The same is done to a
    // bottom-right corner. Two resulting points decsribe the area where
    // grid-nodes should be drawn — where to start and where to finish drawing
    // these nodes.

    QPoint topLeft = viewportToModel(viewportRect().topLeft());
    const QPoint gridTopLeft = modelToViewport(setOnGrid(topLeft));

    QPoint bottomRight = viewportToModel(viewportRect().bottomRight());
    const QPoint gridBottomRight = modelToViewport(setOnGrid(bottomRight));

    // This is the minimal distance between drawn grid-nodes. No matter how
    // a user scales the view, any two adjacent nodes must have at least this
    // amount of "free space" between them.
    constexpr double minimalVisibleGridStep = 8.0;

    // In some scales drawing a point for every step may lead to a very dense
    // grid without much space between nodes. But we want to have some minimal
    // distance between them. In such cases nodes shouldn't be drawn for every
    // grid-step, but instead for every two grid-steps, or every three, and so on.
    //
    // To find out how frequently grid nodes should be drawn, we start from single
    // grid-step and grow it until its "size in scale" gets larger than the minimal
    // distance between points.

    double horizontalStep{ a_GridX * a_Scale };
    for (int n = 2; horizontalStep < minimalVisibleGridStep; n++) {
        horizontalStep = n * a_GridX * a_Scale;
    }

    double verticalStep{ a_GridY * a_Scale };
    for (int n = 2; verticalStep < minimalVisibleGridStep; n++) {
        verticalStep = n * a_GridY * a_Scale;
    }

    // Finally draw the grid-nodes
    for (double x = gridTopLeft.x(); x <= gridBottomRight.x(); x += horizontalStep) {
        for (double y = gridTopLeft.y(); y <= gridBottomRight.y(); y += verticalStep) {
            painter->drawPoint(std::round(x), std::round(y));
        }
    }

    painter->restore();
}

QRect Schematic::allBoundingRect() {
    updateAllBoundingRect();
    return a_UsedArea;
}

namespace internal {

void unite(std::optional<QRect>& left, const QRect& right)
{
    if (left) {
        (*left) |= right;
    } else {
        left.emplace(right);
    }
}

}

// ---------------------------------------------------
void Schematic::updateAllBoundingRect()
{
    std::optional<QRect> totalBounds = std::nullopt;

    for (auto* pc : *a_Components) {
        internal::unite(totalBounds, pc->boundingRectIncludingProperties());
    }

    for (auto* pw : *a_Wires) {
        internal::unite(totalBounds, pw->boundingRect());
        if (pw->hasLabel()) internal::unite(totalBounds, pw->label()->boundingRect());
    }

    for (auto* pn : *a_Nodes) {
        if (pn->hasLabel()) internal::unite(totalBounds, pn->label()->boundingRect());
    }

    for (auto* pd : *a_Diagrams) {
        internal::unite(totalBounds, pd->boundingRect());

        for (auto* pg : pd->Graphs)
            for (auto* pm : pg->Markers) {
                internal::unite(totalBounds, pm->boundingRect());
            }
    }

    for (auto* pp : *a_Paintings) {
        internal::unite(totalBounds, pp->boundingRect());
    }

    // Within the model plane's limits: what lies beyond is not shown, and
    // its width would overflow.
    a_UsedArea = totalBounds.has_value() ? withinModelLimit(*totalBounds) : QRect();
}

Schematic::Selection Schematic::currentSelection() const {

    std::optional<QRect> totalBounds = std::nullopt;

    Selection selection;

    for (auto* pc : *a_Components) {
        if (!pc->isSelected) continue;

        selection.components.push_back(pc);
        internal::unite(totalBounds, pc->boundingRectIncludingProperties());
    }

    for (auto* pw : *a_Wires) {
        if (pw->isSelected) {
            selection.wires.push_back(pw);
            internal::unite(totalBounds, pw->boundingRect());
        }

        if (pw->hasLabel() && pw->label()->isSelected) { // check position of wire label
            selection.labels.push_back(pw->label());
            internal::unite(totalBounds, pw->label()->boundingRect());
        }
    }

    for (auto* pn : *a_Nodes) {
        if (std::ranges::all_of(pn->wires(), [](auto* e) { return e->isSelected; })
            && std::ranges::all_of(pn->components(), [](auto* e) { return e->isSelected; }))
        {
            selection.nodes.push_back(pn);
        }
        else if (pn->isSelected)
        {
            selection.nodes.push_back(pn);
            // also add to seperate isolated nodes container
            selection.isoNodes.push_back(pn);
            totalBounds = std::optional<QRect>{pn->boundingRect()};
        }

        if (pn->hasLabel() && pn->label()->isSelected) { // check position of node label
            selection.labels.push_back(pn->label());
            internal::unite(totalBounds, pn->label()->boundingRect());
        }
    }

    for (auto* pd : *a_Diagrams) {
        if (pd->isSelected) {
            selection.diagrams.push_back(pd);
            internal::unite(totalBounds, pd->boundingRect());
        }

        for (Graph* pg : pd->Graphs) {
            for (Marker* pm : pg->Markers) {
                if (!pm->isSelected) continue;
                selection.markers.push_back(pm);
                internal::unite(totalBounds, pm->boundingRect());
            }
        }
    }

    for (auto* pp : *a_Paintings) {
        if (!pp->isSelected) continue;
        selection.paintings.push_back(pp);
        internal::unite(totalBounds, pp->boundingRect());
    }

    if (!totalBounds) {
        return {};
    }

    selection.bounds = *totalBounds;
    return selection;
}

// Convert an element list to Selection
Schematic::Selection Schematic::elementsToSelection(const std::list<Element*> &elements) const
{
        std::optional<QRect> totalBounds = std::nullopt;
        std::unordered_set<Node*> ownedNodes;
        Selection selection;

        // A helper to simplify uniting bounding boxes.
        auto addElement = [&](auto* element, auto& list) {
            list.push_back(element);
            internal::unite(totalBounds, element->boundingRect());
        };

        for (Element* element : elements) {
            if (element == nullptr) {
                continue;
            }

            if (auto* pc = dynamic_cast<Component*>(element)) {
                addElement(pc, selection.components);
                // add all port nodes to ownedNodes set
                for (auto* port : pc->Ports) {
                    ownedNodes.emplace(port->Connection);
                }
            } else if (auto* pw = dynamic_cast<Wire*>(element)) {
                addElement(pw, selection.wires);
                // add ports/nodes to ownedNodes set
                ownedNodes.emplace(pw->Port1);
                ownedNodes.emplace(pw->Port2);
            } else if (auto* pn = dynamic_cast<Node*>(element)) {
                addElement(pn, selection.nodes);
            } else if (auto* pl = dynamic_cast<WireLabel*>(element)) {
                addElement(pl, selection.labels);
            } else if (auto* pd = dynamic_cast<Diagram*>(element)) {
                addElement(pd, selection.diagrams);
            } else if (auto* pm = dynamic_cast<Marker*>(element)) {
                addElement(pm, selection.markers);
            } else if (auto* pp = dynamic_cast<Painting*>(element)) {
                addElement(pp, selection.paintings);
            }
        }

        if(!totalBounds) {
            return {};
        }

        // nodes that are not owned by either a component or a wire
        // gets added to a separate @isoNodes container
        for (auto* pn : selection.nodes) {
            if (!ownedNodes.contains(pn)) {
                selection.isoNodes.push_back(pn);
            }
        }

        selection.bounds = *totalBounds;
        return selection;
}

// ---------------------------------------------------
// Updates the graph data of all diagrams (load from data files).
void Schematic::reloadGraphs()
{
    QFileInfo Info(a_DocName);
    for (Diagram *pd : *a_Diagrams)
        pd->loadGraphData(Info.path() + QDir::separator() + a_DataSet);
}

// Copy function,
void Schematic::copy()
{
    QString s = createClipboardFile();
    QClipboard *cb = QApplication::clipboard(); // get system clipboard
    if (!s.isEmpty()) {
        cb->setText(s, QClipboard::Clipboard);
    }
}

// ---------------------------------------------------
// Cut function, copy followed by deletion
void Schematic::cut()
{
    copy();
    deleteElements(); //delete selected elements
    viewport()->update();
}

// ---------------------------------------------------
// Performs paste function from clipboard
bool Schematic::paste(QTextStream *stream, std::list<Element*> *pe)
{
    return pasteFromClipboard(stream, pe);
}

// ---------------------------------------------------
// Loads this Qucs document.
bool Schematic::load()
{
    deleteAllElements();
    deleteSymbolPaintings();

    const bool loaded = loadDocument();
    emit signalDocumentRebuilt(this);
    if (!loaded)
        return false;
    a_lastSaved = QDateTime::currentDateTime();

    while (!a_undoAction.isEmpty()) {
        delete a_undoAction.last();
        a_undoAction.pop_back();
    }
    a_undoActionIdx = 0;
    while (!a_undoSymbol.isEmpty()) {
        delete a_undoSymbol.last();
        a_undoSymbol.pop_back();
    }
    a_symbolMode = true;
    setChanged(false, true); // "not changed" state, but put on undo stack
    a_undoSymbolIdx = 0;
    a_undoSymbol.at(a_undoSymbolIdx)->replace(1, 1, 'i');
    a_symbolMode = false;
    setChanged(false, true); // "not changed" state, but put on undo stack
    a_undoActionIdx = 0;
    a_undoAction.at(a_undoActionIdx)->replace(1, 1, 'i');

    // The undo stack of the circuit symbol is initialized when first
    // entering its edit mode.

    // have to call this to avoid crash at sizeOfAll
    becomeCurrent(false);

    showAll();
    a_tmpViewX1 = a_tmpViewY1 = -200; // was used as temporary cache
    return true;
}

// ---------------------------------------------------
// Saves this Qucs document. Returns the number of subcircuit ports.
int Schematic::save()
{
    int result = 0;
    // When saving *only* a symbol, there is no corresponding schematic:
    // and thus ports in symbol don't have corresponding ports in schematic.
    // There is just nothing to adjust.
    //
    // In other cases we want to delete any dangling ports from symbol
    // and invoke "adjustPortNumbers" for it.
    if (!a_isSymbolOnly) {
        result = adjustPortNumbers(); // same port number for schematic and symbol
    } else {
        orderSymbolPorts();
    }
    if (saveDocument() < 0)
        return -1;

    QFileInfo Info(a_DocName);
    a_lastSaved = Info.lastModified();

    if (result >= 0) {
        setChanged(false);

        QVector<QString *>::iterator it;
        for (it = a_undoAction.begin(); it != a_undoAction.end(); it++) {
            (*it)->replace(1, 1, ' '); //at(1) = ' '; state of being changed
        }
        //(1) = 'i';   // state of being unchanged
        a_undoAction.at(a_undoActionIdx)->replace(1, 1, 'i');

        for (it = a_undoSymbol.begin(); it != a_undoSymbol.end(); it++) {
            (*it)->replace(1, 1, ' '); //at(1) = ' '; state of being changed
        }
        //at(1) = 'i';   // state of being unchanged
        a_undoSymbol.at(a_undoSymbolIdx)->replace(1, 1, 'i');
    }

    return result;
}

// ---------------------------------------------------
// If the port number of the schematic and of the symbol are not
// equal add or remove some in the symbol.
int Schematic::adjustPortNumbers()
{
    QRect usedArea;
    // get size of whole symbol to know where to place new ports
    if (a_symbolMode)
        usedArea = allBoundingRect();
    else {
        a_Components = &a_SymbolComps;
        a_Wires = &a_SymbolWires;
        a_Nodes = &a_SymbolNodes;
        a_Diagrams = &a_SymbolDiags;
        a_Paintings = &a_SymbolPaints;
        usedArea = allBoundingRect();
        a_Components = &a_DocComps;
        a_Wires = &a_DocWires;
        a_Nodes = &a_DocNodes;
        a_Diagrams = &a_DocDiags;
        a_Paintings = &a_DocPaints;
    }
    int x1 = usedArea.left() + 40;
    int y2 = usedArea.bottom() + 20;
    setOnGrid(x1, y2);

    // delete all port names in symbol
    for (auto* pp : a_SymbolPaints)
        if (pp->Name == ".PortSym ")
            ((PortSymbol *) pp)->nameStr = "";

    QString Str;
    int countPort = 0;

    QFileInfo Info(a_DataDisplay);
    QString Suffix = Info.suffix();

    // handle VHDL file symbol
    if (Suffix == "vhd" || Suffix == "vhdl") {
        QStringList::iterator it;
        QStringList Names, GNames, GTypes, GDefs;
        int Number;

        // get ports from VHDL file
        QFileInfo Info(a_DocName);
        QString Name = Info.path() + QDir::separator() + a_DataDisplay;

        // obtain VHDL information either from open text document or the
        // file directly
        VHDL_File_Info VInfo;
        TextDoc *d = a_App ? a_App->findTextDoc(Name) : nullptr;
        if (d)
            VInfo = VHDL_File_Info(d->document()->toPlainText());
        else
            VInfo = VHDL_File_Info(Name, true);

        if (!VInfo.PortNames.isEmpty())
            Names = VInfo.PortNames.split(",", Qt::SkipEmptyParts);

        for (auto* pp : a_SymbolPaints)
            if (pp->Name == ".ID ") {
                ID_Text *id = (ID_Text *) pp;
                id->prefix = VInfo.EntityName.toUpper();
                id->subParameters.clear();
                if (!VInfo.GenNames.isEmpty())
                    GNames = VInfo.GenNames.split(",", Qt::SkipEmptyParts);
                if (!VInfo.GenTypes.isEmpty())
                    GTypes = VInfo.GenTypes.split(",", Qt::SkipEmptyParts);
                if (!VInfo.GenDefs.isEmpty())
                    GDefs = VInfo.GenDefs.split(",", Qt::SkipEmptyParts);
                ;
                for (Number = 1, it = GNames.begin(); it != GNames.end(); ++it) {
                    id->subParameters.push_back(
                        std::make_unique<SubParameter>(
                                         true,
                                         *it + "=" + GDefs[Number - 1],
                                         tr("generic") + " " + QString::number(Number),
                                         GTypes[Number - 1]));
                    Number++;
                }
            }

        for (Number = 1, it = Names.begin(); it != Names.end(); ++it, Number++) {
            countPort++;

            Str = QString::number(Number);
            // search for matching port symbol
            Painting* pp = nullptr;
            for (auto* painting : a_SymbolPaints)
                if (painting->Name == ".PortSym ")
                    if (((PortSymbol *) painting)->numberStr == Str) {
                        pp = painting;
                        break;
                    }

            if (pp)
                ((PortSymbol *) pp)->nameStr = *it;
            else {
                a_SymbolPaints.push_back(new PortSymbol(x1, y2, Str, *it));
                y2 += 40;
            }
        }
    }
    // handle Verilog-HDL file symbol
    else if (Suffix == "v") {
        QStringList::iterator it;
        QStringList Names;
        int Number;

        // get ports from Verilog-HDL file
        QFileInfo Info(a_DocName);
        QString Name = Info.path() + QDir::separator() + a_DataDisplay;

        // obtain Verilog-HDL information either from open text document or the
        // file directly
        Verilog_File_Info VInfo;
        TextDoc *d = a_App ? a_App->findTextDoc(Name) : nullptr;
        if (d)
            VInfo = Verilog_File_Info(d->document()->toPlainText());
        else
            VInfo = Verilog_File_Info(Name, true);
        if (!VInfo.PortNames.isEmpty())
            Names = VInfo.PortNames.split(",", Qt::SkipEmptyParts);

        for (auto* pp : a_SymbolPaints)
            if (pp->Name == ".ID ") {
                ID_Text *id = (ID_Text *) pp;
                id->prefix = VInfo.ModuleName.toUpper();
                id->subParameters.clear();
            }

        for (Number = 1, it = Names.begin(); it != Names.end(); ++it, Number++) {
            countPort++;

            Str = QString::number(Number);
            // search for matching port symbol
            Painting* pp = nullptr;
            for (auto* painting : a_SymbolPaints)
                if (painting->Name == ".PortSym ")
                    if (((PortSymbol *) painting)->numberStr == Str) {
                        pp = painting;
                        break;
                    }

            if (pp)
                ((PortSymbol *) pp)->nameStr = *it;
            else {
                a_SymbolPaints.push_back(new PortSymbol(x1, y2, Str, *it));
                y2 += 40;
            }
        }
    }
    // handle Verilog-A file symbol
    else if (Suffix == "va") {
        QStringList::iterator it;
        QStringList Names;
        int Number;

        // get ports from Verilog-A file
        QFileInfo Info(a_DocName);
        QString Name = Info.path() + QDir::separator() + a_DataDisplay;

        // obtain Verilog-A information either from open text document or the
        // file directly
        VerilogA_File_Info VInfo;
        TextDoc *d = a_App ? a_App->findTextDoc(Name) : nullptr;
        if (d)
            VInfo = VerilogA_File_Info(d->toPlainText());
        else
            VInfo = VerilogA_File_Info(Name, true);

        if (!VInfo.PortNames.isEmpty())
            Names = VInfo.PortNames.split(",", Qt::SkipEmptyParts);

        for (auto* pp : a_SymbolPaints)
            if (pp->Name == ".ID ") {
                ID_Text *id = (ID_Text *) pp;
                id->prefix = VInfo.ModuleName.toUpper();
                id->subParameters.clear();
            }

        for (Number = 1, it = Names.begin(); it != Names.end(); ++it, Number++) {
            countPort++;

            Str = QString::number(Number);
            // search for matching port symbol
            Painting* pp = nullptr;
            for (auto* painting : a_SymbolPaints)
                if (painting->Name == ".PortSym ")
                    if (((PortSymbol *) painting)->numberStr == Str) {
                        pp = painting;
                        break;
                    }

            if (pp)
                ((PortSymbol *) pp)->nameStr = *it;
            else {
                a_SymbolPaints.push_back(new PortSymbol(x1, y2, Str, *it));
                y2 += 40;
            }
        }
    }
    // handle schematic symbol
    else {
        // go through all components in a schematic
        for (Component* pc : a_DocComps) {
            if (pc->Model == "Port") {
                countPort++;

                Str = pc->Props.front()->Value;
                // search for matching port symbol
                Painting* pp = nullptr;
                for (auto* painting : a_SymbolPaints) {
                    if (painting->Name == ".PortSym ") {
                        if (((PortSymbol *) painting)->numberStr == Str) {
                            pp = painting;
                            break;
                        }
                    }
                }

                const QString pinName = portPinName(pc);
                // The second property of a port is its type; the symbol
                // shows which way the pin points from it.
                const QString pinDir = pc->Props.count() > 1 ? pc->Props.at(1)->Value : QString();
                if (!pp) {
                    pp = new PortSymbol(x1, y2, Str, pinName);
                    a_SymbolPaints.push_back(pp);
                    y2 += 40;
                }
                ((PortSymbol *) pp)->setPortName(pinName);
                ((PortSymbol *) pp)->dirStr = pinDir;
            }
        }
    }

    a_SymbolPaints.remove_if([](Painting* pp) {
        return pp->Name == ".PortSym " && (((PortSymbol *) pp)->nameStr.isEmpty());
    });

    return countPort;
}

// The netlist calls a pin of a subcircuit after the net the port sits on
// (see AbstractSpiceKernel::createSubNetlist), so that is what the symbol
// writes beside the pin: the label of the net, or - for a net with no
// label of its own, which the netlister then names after the port - the
// port's name. Walks the net from the port's own node, so it works in
// either view mode.
QString Schematic::netLabelOf(Node* start) const
{
    if (start == nullptr) return QString();

    std::unordered_set<const Node*> seen;
    std::vector<Node*> todo{start};
    while (!todo.empty()) {
        Node* node = todo.back();
        todo.pop_back();
        if (node == nullptr || !seen.insert(node).second) continue;

        if (node->hasLabel() && !node->label()->Name.isEmpty())
            return node->label()->Name;

        for (Wire* wire : node->wires()) {
            if (wire->hasLabel() && !wire->label()->Name.isEmpty())
                return wire->label()->Name;
            todo.push_back(wire->Port1);
            todo.push_back(wire->Port2);
        }
    }

    return QString();
}

QString Schematic::portPinName(Component* port) const
{
    if (port == nullptr || port->Ports.isEmpty()) return QString();

    const QString label = netLabelOf(port->Ports.first()->Connection);
    return label.isEmpty() ? port->Name : label;
}

int Schematic::orderSymbolPorts()
{
  int countPorts = 0;
  QSet<int> port_numbers, existing_numbers, free_numbers;
  int max_port_number = 0;
  for (auto* pp : a_SymbolPaints) {
    if (pp->Name == ".PortSym ") {
      countPorts++;
      QString numstr = ((PortSymbol *) pp)->numberStr;
      if (numstr != "0") {
        if (numstr.toInt() > max_port_number) {
          max_port_number = numstr.toInt();
        }
        existing_numbers.insert(numstr.toInt());
      }
    }
  }

  max_port_number = std::max(countPorts,max_port_number);
  for (int i = 1; i <= max_port_number; i++) {
    port_numbers.insert(i);
  }

  free_numbers = port_numbers - existing_numbers;

  // Assign new numbers only if port number is empty; Preserve ports order.
  for (auto* pp : a_SymbolPaints) {
    if (pp->Name == ".PortSym ") {
      QString numstr = ((PortSymbol *) pp)->numberStr;
      if (numstr == "0") {
        int free_num = *free_numbers.constBegin();
        free_numbers.remove(free_num);
        ((PortSymbol *) pp)->numberStr = QString::number(free_num);
      }
    }
  }

  return countPorts;
}

// ---------------------------------------------------
void Schematic::noteKeyboardMove()
{
    setChanged(true, true, 'k');
}

bool Schematic::cancelKeyboardMove()
{
    if (a_symbolMode || !a_keyboardMoveOpen || a_undoActionIdx <= 0
        || a_undoAction.at(a_undoActionIdx)->at(0) != 'k')
        return false;
    a_keyboardMoveOpen = false;
    return undo();
}

bool Schematic::undo()
{
    a_keyboardMoveOpen = false;
    if (a_symbolMode) {
        if (a_undoSymbolIdx == 0) {
            return false;
        }

        rebuildSymbol(a_undoSymbol.at(--a_undoSymbolIdx));

        emit signalUndoState(a_undoSymbolIdx != 0);
        emit signalRedoState(a_undoSymbolIdx != a_undoSymbol.size() - 1);

        if (a_undoSymbol.at(a_undoSymbolIdx)->at(1) == 'i'
            && a_undoAction.at(a_undoActionIdx)->at(1) == 'i') {
            setChanged(false, false);
            return true;
        }

        setChanged(true, false);
        return true;
    }

    // ...... for schematic edit mode .......
    if (a_undoActionIdx == 0) {
        return false;
    }

    rebuild(a_undoAction.at(--a_undoActionIdx));
    reloadGraphs(); // load recent simulation data

    emit signalUndoState(a_undoActionIdx != 0);
    emit signalRedoState(a_undoActionIdx != a_undoAction.size() - 1);

    if (a_undoAction.at(a_undoActionIdx)->at(1) == 'i') {
        if (a_undoSymbol.isEmpty()) {
            setChanged(false, false);
            return true;
        } else if (a_undoSymbol.at(a_undoSymbolIdx)->at(1) == 'i') {
            setChanged(false, false);
            return true;
        }
    }

    setChanged(true, false);
    return true;
}

// ---------------------------------------------------
bool Schematic::redo()
{
    a_keyboardMoveOpen = false;
    if (a_symbolMode) {
        if (a_undoSymbolIdx == a_undoSymbol.size() - 1) {
            return false;
        }

        rebuildSymbol(a_undoSymbol.at(++a_undoSymbolIdx));
        adjustPortNumbers(); // set port names

        emit signalUndoState(a_undoSymbolIdx != 0);
        emit signalRedoState(a_undoSymbolIdx != a_undoSymbol.size() - 1);

        if (a_undoSymbol.at(a_undoSymbolIdx)->at(1) == 'i'
            && a_undoAction.at(a_undoActionIdx)->at(1) == 'i') {
            setChanged(false, false);
            return true;
        }

        setChanged(true, false);
        return true;
    }

    //
    // ...... for schematic edit mode .......
    if (a_undoActionIdx == a_undoAction.size() - 1) {
        return false;
    }

    rebuild(a_undoAction.at(++a_undoActionIdx));
    reloadGraphs(); // load recent simulation data

    emit signalUndoState(a_undoActionIdx != 0);
    emit signalRedoState(a_undoActionIdx != a_undoAction.size() - 1);

    if (a_undoAction.at(a_undoActionIdx)->at(1) == 'i') {
        if (a_undoSymbol.isEmpty()) {
            setChanged(false, false);
            return true;
        } else if (a_undoSymbol.at(a_undoSymbolIdx)->at(1) == 'i') {
            setChanged(false, false);
            return true;
        }
    }

    setChanged(true, false);
    return true;
}

// ---------------------------------------------------
void Schematic::switchPaintMode()
{
    a_symbolMode = !a_symbolMode; // change mode

    std::swap(a_Scale, a_tmpScale);
    int x = contentsX();
    int y = contentsY();
    setContentsPos(a_tmpPosX, a_tmpPosY);
    a_tmpPosX = x;
    a_tmpPosY = y;
    std::swap(a_ViewX1, a_tmpViewX1);
    std::swap(a_ViewY1, a_tmpViewY1);
    std::swap(a_ViewX2, a_tmpViewX2);
    std::swap(a_ViewY2, a_tmpViewY2);
    std::swap(a_UsedArea, a_tmpUsedArea);
}

// *********************************************************************
// **********                                                 **********
// **********      Function for serving mouse wheel moving    **********
// **********                                                 **********
// *********************************************************************
void Schematic::contentsWheelEvent(QWheelEvent *Event)
{
    a_App->editText->setHidden(true); // disable edit of component property

    // A mouse wheel angle delta of a single step is typically 120,
    // but other devices may produce various values. For example,
    // angle values produced by a touchpad depend on how fast user
    // moves their fingers.
    //
    // When used for scrolling the view here angle delta is divided by
    // some number ("2" at the moment). There is nothing special about
    // this number, its sole purpose is to reduce a scroll-step
    // to a reasonable size.

    // Mouse may have a special wheel for horizontal scrolling
    const int horizontalWheelAngleDelta = Event->angleDelta().x();
    const int verticalWheelAngleDelta = Event->angleDelta().y();

    // Scroll horizontally
    // Horizontal scroll is performed either by a special wheel
    // or by usual mouse wheel with Shift pressed down.
    if ((Event->modifiers() & Qt::ShiftModifier) || horizontalWheelAngleDelta) {
        int delta = (horizontalWheelAngleDelta ? horizontalWheelAngleDelta : verticalWheelAngleDelta) / 2;
        if (delta > 0) {
            scrollLeft(delta);
        } else {
            scrollRight(-delta);
        }
    }
    // Zoom in or out
    else if (Event->modifiers() & Qt::ControlModifier) {
        // zoom factor scaled according to the wheel delta, to accommodate
        //  values different from 60 (slower or faster zoom)
        double scaleCoef = pow(1.1, verticalWheelAngleDelta / 60.0);
        const QPoint pointer{
            static_cast<int>(Event->position().x()),
            static_cast<int>(Event->position().y())};
        zoomAroundPoint(scaleCoef, pointer);
    }
    // Scroll vertically
    else {
        int delta = verticalWheelAngleDelta / 2;
        if (delta > 0) {
            scrollUp(delta);
        } else {
            scrollDown(-delta);
        }
    }
    Event->accept(); // QScrollView must not handle this event
}

// Scrolls the visible area upwards and enlarges or reduces the view
// area accordingly.
void Schematic::scrollUp(int step)
{
    QUCS_ASSERT(step >= 0);

    // Y-axis is directed "from top to bottom": the higher a point is
    // located, the smaller its y-coordinate and vice versa. Keep this in mind
    // while reading the code below.

    const int stepInModel = static_cast<int>(std::round(step/a_Scale));
    const QPoint viewportTopLeft = viewportRect().topLeft();

    // A point currently displayed in top left corner
    QPoint mtl = viewportToModel(viewportTopLeft);
    // A point that should be displayed in top left corner after scrolling
    mtl.setY(mtl.y() - stepInModel);

    QRect modelBounds = modelRect();

    // If the "should-be-displayed" point is located higher than model upper bound,
    // then extend the model
    modelBounds.setTop(std::min(mtl.y(), modelBounds.top()));

    // Cut off a bit of unused model space from its bottom side.
    const auto b = modelBounds.bottom() - stepInModel;
    if (b > a_UsedArea.bottom()) {
        modelBounds.setBottom(b);
    }

    renderModel(a_Scale, modelBounds, mtl, viewportTopLeft);
}

// Scrolls the visible area downwards and enlarges or reduces the view
// area accordingly.
void Schematic::scrollDown(int step)
{
    QUCS_ASSERT(step >= 0);

    // Y-axis is directed "from top to bottom": the lower a point is
    // located, the bigger its y-coordinate and vice versa. Keep this in mind
    // while reading the code below.

    const int stepInModel = static_cast<int>(std::round(step/a_Scale));
    const QPoint viewportBottomLeft = viewportRect().bottomLeft();

    // A point currently displayed in bottom left corner
    QPoint mbl = viewportToModel(viewportBottomLeft);
    // A point that should be displayed in bottom left corner after scrolling
    mbl.setY(mbl.y() + stepInModel);

    QRect modelBounds = modelRect();

    // If the "should-be-displayed" point is lower than model bottom bound,
    // then extend the model
    modelBounds.setBottom(std::max(mbl.y(), modelBounds.bottom()));

    // Cut off a bit of unused model space from its top side.
    const auto t = modelBounds.top() + stepInModel;
    if (t < a_UsedArea.top()) {
        modelBounds.setTop(t);
    }

    // Render model in its new size and position point in the top left corner of viewport
    renderModel(a_Scale, modelBounds, mbl, viewportBottomLeft);
}

// Scrolls the visible area to the left and enlarges or reduces the view
// area accordingly.
void Schematic::scrollLeft(int step)
{
    QUCS_ASSERT(step >= 0);

    // X-axis is directed "from left to right": the more to the left a point is
    // located, the smaller its x-coordinate and vice versa. Keep this in mind
    // while reading the code below.

    const int stepInModel = static_cast<int>(std::round(step/a_Scale));
    const QPoint viewportTopLeft = viewportRect().topLeft();

    // A point currently displayed in top left corner
    QPoint mtl = viewportToModel(viewportTopLeft);
    // A point that should be displayed in top left corner after scrolling
    mtl.setX(mtl.x() - stepInModel);

    QRect modelBounds = modelRect();

    // If the "should-be-displayed" point is to the left of model left bound,
    // then extend the model
    modelBounds.setLeft(std::min(mtl.x(), modelBounds.left()));

    // Cut off a bit of unused model space from its right side.
    const auto r = modelBounds.right() - stepInModel;
    if (r > a_UsedArea.right()) {
        modelBounds.setRight(r);
    }

    renderModel(a_Scale, modelBounds, mtl, viewportTopLeft);
}

// Scrolls the visible area to the right and enlarges or reduces the
// view area accordingly.
void Schematic::scrollRight(int step)
{
    QUCS_ASSERT(step >= 0);

    // X-axis is directed "from left to right": the more to the right a point is
    // located, the bigger its x-coordinate and vice versa. Keep this in mind
    // while reading the code below.

    const int stepInModel = static_cast<int>(std::round(step/a_Scale));
    const QPoint viewportTopRight = viewportRect().topRight();

    // A point currently displayed in top right corner
    QPoint mtr = viewportToModel(viewportTopRight);
    // A point that should be displayed in top right corner after scrolling
    mtr.setX(mtr.x() + stepInModel);

    QRect modelBounds = modelRect();

    // If the "should-be-displayed" point is to the right of the model right bound,
    // then extend the model
    modelBounds.setRight(std::max(mtr.x(), modelBounds.right()));

    // Cut off a bit of unused model space from its left side.
    const auto l = modelBounds.left() + stepInModel;
    if (l < a_UsedArea.left()) {
        modelBounds.setLeft(l);
    }

    renderModel(a_Scale, modelBounds, mtr, viewportTopRight);
}

// -----------------------------------------------------------
// Is called if the scroll arrow of the ScrollBar is pressed.
void Schematic::slotScrollUp()
{
    a_App->editText->setHidden(true); // disable edit of component property
    scrollUp(verticalScrollBar()->singleStep());
}

// -----------------------------------------------------------
// Is called if the scroll arrow of the ScrollBar is pressed.
void Schematic::slotScrollDown()
{
    a_App->editText->setHidden(true); // disable edit of component property
    scrollDown(verticalScrollBar()->singleStep());
}

// -----------------------------------------------------------
// Is called if the scroll arrow of the ScrollBar is pressed.
void Schematic::slotScrollLeft()
{
    a_App->editText->setHidden(true); // disable edit of component property
    scrollLeft(horizontalScrollBar()->singleStep());
}

// -----------------------------------------------------------
// Is called if the scroll arrow of the ScrollBar is pressed.
void Schematic::slotScrollRight()
{
    a_App->editText->setHidden(true); // disable edit of component property
    scrollRight(horizontalScrollBar()->singleStep());
}

// *********************************************************************
// **********                                                 **********
// **********        Function for serving drag'n drop         **********
// **********                                                 **********
// *********************************************************************

// Is called if an object is dropped (after drag'n drop).
void Schematic::contentsDropEvent(QDropEvent *Event)
{
  if (a_dragIsOkay) {
    QList<QUrl> urls = Event->mimeData()->urls();
    if (urls.isEmpty()) {
      return;
    }
    Event->setDropAction(Qt::CopyAction);
    Event->accept();

    QStringList toOpen;
    for (const QUrl &url : urls) {
      QString filePath = QDir::toNativeSeparators(url.toLocalFile());

             // Check if file is a supported image
      if (qucs_s::EmbeddedImage::isSupportedFile(filePath)) {
        // Insert ImagePainting at drop position
        auto ev_pos = Event->position();
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
        QPoint inModel = contentsToModel(ev_pos.toPoint());
#else
        QPoint inModel = contentsToModel(ev_pos.toPoint());
#endif
        ImagePainting* imgPaint = new ImagePainting();
        imgPaint->setImageFromPath(filePath);
        imgPaint->setPlacement(inModel.x(), inModel.y(),
                               inModel.x() + imgPaint->getImageWidth(),
                               inModel.y() + imgPaint->getImageHeight());
        this->a_Paintings->push_back(imgPaint);
        viewport()->update();
        setChanged(true, true);
        continue; // allow dropping multiple images at once
      }
      // Any other file is opened in its viewer, once this event is over
      // (this schematic may be the untitled document that gets closed).
      toOpen.append(filePath);
    }

    if (a_App) a_App->openDroppedFiles(toOpen, this);
    return;
  }

         // Not a document drag, fallback to component insertion
  auto ev_pos = Event->position();
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
  QPoint inModel = contentsToModel(ev_pos.toPoint());
#else
  QPoint inModel = contentsToModel(ev_pos.toPoint());
#endif

  QMouseEvent e(QEvent::MouseButtonPress, ev_pos, mapToGlobal(ev_pos), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);

  a_App->view->MPressElement(this, &e, inModel.x(), inModel.y());

  delete a_App->view->selElem;
  a_App->view->selElem = nullptr; // no component selected

  if (formerAction) {
    formerAction->setChecked(true);
  } else {
    QucsMain->select->setChecked(true); // restore old action
  }
}

// ---------------------------------------------------
void Schematic::contentsDragEnterEvent(QDragEnterEvent *Event)
{
    //FIXME: the function of drag library component seems not working?
    formerAction = nullptr;
    a_dragIsOkay = false;

    // file dragged in ?
    if (Event->mimeData()->hasUrls()) {
        a_dragIsOkay = true;
        Event->accept();
        return;
    }

    // drag library component
    if (Event->mimeData()->hasText()) {
        QString s = Event->mimeData()->text();
        if (s.left(15) == "QucsComponent:<") {
            s = s.mid(14);
            a_App->view->selElem = getComponentFromName(s);
            if (a_App->view->selElem) {
                Event->accept();
                return;
            }
        }
        Event->ignore();
        return;
    }


    // drag component from listview
    if (Event->mimeData()->hasFormat("application/x-qabstractitemmodeldatalist")) {
        QListWidgetItem *Item = a_App->CompComps->currentItem();
        if (Item) {
            formerAction = a_App->activeAction;
            a_App->slotSelectComponent(Item); // also sets drawn=false
            a_App->MouseMoveAction = nullptr;
            a_App->MousePressAction = nullptr;

            Event->accept();
            return;
        }
    }
    //  }

    Event->ignore();
}

// ---------------------------------------------------
void Schematic::contentsDragLeaveEvent(QDragLeaveEvent *)
{
    if (formerAction)
        formerAction->setChecked(true); // restore old action
}

void Schematic::contentsNativeGestureZoomEvent( QNativeGestureEvent* Event) {
  a_App->editText->setHidden(true); // disable edit of component property

  const auto factor = 1.0 + Event->value();
  const auto pointer = mapFromGlobal(Event->globalPosition().toPoint());
  zoomAroundPoint(factor,pointer);
}

// ---------------------------------------------------
void Schematic::contentsDragMoveEvent(QDragMoveEvent *Event)
{
    if (!a_dragIsOkay) {
        if (a_App->view->selElem == nullptr) {
            Event->ignore();
            return;
        }

        auto ev_pos = Event->position();
        QMouseEvent e(QEvent::MouseButtonPress, ev_pos, mapToGlobal(ev_pos), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        a_App->view->MMoveElement(this, &e);
    }

    Event->accept();
}

bool Schematic::checkDplAndDatNames()
{
    QFileInfo Info(a_DocName);
    if (!a_DocName.isEmpty() && a_DataSet.size() > 4 && a_DataDisplay.size() > 4) {
        QString base = Info.completeBaseName();
        QString base_dat = a_DataSet;
        base_dat.chop(4);
        QString base_dpl = a_DataDisplay;
        base_dpl.chop(4);
        if (base != base_dat || base != base_dpl) {
            QString msg = QObject::tr(
                "The schematic name and dataset/display file name is not matching! "
                "This may happen if schematic was copied using the file manager "
                "instead of using File->SaveAs. Correct dataset and display names "
                "automatically?\n\n");
            msg += QString(QObject::tr("Schematic file: ")) + base + ".sch\n";
            msg += QString(QObject::tr("Dataset file: ")) + a_DataSet + "\n";
            msg += QString(QObject::tr("Display file: ")) + a_DataDisplay + "\n";
            auto r = QMessageBox::information(this,
                                              QObject::tr("Open document"),
                                              msg,
                                              QMessageBox::Yes,
                                              QMessageBox::No);
            if (r == QMessageBox::Yes) {
                a_DataSet = base + ".dat";
                a_DataDisplay = base + ".dpl";
                return true;
            }
        }
    } else {
        return false;
    }
    return false;
}
