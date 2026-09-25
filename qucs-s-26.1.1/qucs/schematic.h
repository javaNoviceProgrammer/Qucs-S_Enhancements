/***************************************************************************
                               schematic.h
                              -------------
    begin                : Sat Mar 11 2006
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

#ifndef SCHEMATIC_H
#define SCHEMATIC_H

// maybe in another place...
#ifdef NDEBUG
// cast without overhead
#  define prechecked_cast static_cast
#else
// cast safely, for debugging purposes
#  define prechecked_cast dynamic_cast
#endif

#include "qucsdoc.h"
#include "wire_planner.h"
#include "schematic_selection.h"
#include "biaslabels.h"
#include "oppoint.h"

#include "qt3_compat/q3scrollview.h"
#include <QVector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <QStringList>

class QTextStream;
class QTextEdit;
class QPlainTextEdit;
class QDragMoveEvent;
class QDropEvent;
class QDragLeaveEvent;
class QWheelEvent;
class QMouseEvent;
class QDragEnterEvent;
class QPainter;

class Element;
class Component;
class Conductor;
class Diagram;
class Marker;
class Node;
class Painting;
class Wire;
class WireLabel;
namespace qucs_s { class InsertionIndex; }

// digital signal data
struct DigSignal {
  DigSignal() { Name=""; Type=""; }
  DigSignal(const QString& _Name, const QString& _Type = "")
    : Name(_Name), Type(_Type) {}
  QString Name; // name
  QString Type; // type of signal
};
typedef QMap<QString, DigSignal> DigMap;
typedef enum {_NotRop, _Rect, _SelectionRect, _Line, _Ellipse, _Arc, _DotLine, _DotRect, _Translate, _Scale} PE;
typedef struct {PE pe; int x1; int y1;int x2;int y2;int a; int b; bool PaintOnViewport;}PostedPaintEvent;

// subcircuit, vhdl, etc. file structure
struct SubFile {
  SubFile() { Type=""; File=""; PortTypes.clear(); }
  SubFile(const QString& _Type, const QString& _File)
    : Type(_Type), File(_File) { PortTypes.clear(); }
  QString Type;          // type of file
  QString File;          // file name identifier
  QStringList PortTypes; // data types of in/out signals
};
typedef QMap<QString, SubFile> SubMap;

enum class FrameSize : int {
    // No frame
    None              = 0,

    // DIN A formats
    A6_Landscape      = 9,
    A6_Portrait       = 10,
    A5_Landscape      = 1,
    A5_Portrait       = 2,
    A4_Landscape      = 3,
    A4_Portrait       = 4,
    A3_Landscape      = 5,
    A3_Portrait       = 6,

    // US Letter
    Letter_Landscape  = 7,
    Letter_Portrait   = 8,
};


class Schematic : public Q3ScrollView, public QucsDoc {
  Q_OBJECT

public:
  Schematic(QucsApp*, const QString&);
 ~Schematic();

  void setName(const QString&);
  void setChanged(bool, bool fillStack=false, char Op='*');
  void print(QPrinter*, QPainter*, bool printAll, bool fitToPage, QMargins margins={});
  // What print() draws: everything (with the frame, when one is shown) or
  // the selection, and the margins.
  QRect printedArea(bool printAll, QMargins margins={});

  void paintSchToViewpainter(QPainter* painter, bool printAll);

  void PostPaintEvent(PE pe, int x1=0, int y1=0, int x2=0, int y2=0, int a=0, int b=0,bool PaintOnViewport=false);

  bool sizeOfFrame(int&, int&);

  /**
    Iterates over all elements of schematic to find the size of the smallest
    rectangle able to fit all elements. The bounds are then stored in members
    @c UsedX1, @c UsedY1, @c UsedX2, @c UsedY2 , i.e. the internal state
    of schematic is updated.
  */
  void updateAllBoundingRect();

  /**
    Returns the smallest rectangle enclosing all elements of schematic
  */
  QRect allBoundingRect();

  using Selection = SchematicSelection;
  Selection  currentSelection() const;
  Selection  elementsToSelection(const std::list<Element*>&) const;
  void  decoupleElements(Selection selection, bool keepNodeLabel=false);
  bool  rotateElements();
  bool  mirrorXComponents();
  bool  mirrorYComponents();
  // Same as above - but with an arbitrary selection
  bool  rotateElements(Selection selection, bool doHeal=true);
  bool  mirrorXComponents(Selection selection, bool doHeal=true);
  bool  mirrorYComponents(Selection selection, bool doHeal=true);

  /** Deletes every element of the document (components, wires, nodes,
      diagrams, paintings) and empties the lists. The symbol is untouched. */
  void deleteAllElements();
  /** Deletes the subcircuit symbol paintings and empties the list. */
  void deleteSymbolPaintings();

  QPoint setOnGrid(const QPoint& p);
  void  setOnGrid(int&, int&);
  bool  elementsOnGrid();
  bool  elementsOnGrid(Selection selection, bool doHeal=true);

  /**
    Zoom around a "zooming center". Zooming center is a point on the canvas,
    which doesn't move relative to a viewport while canvas is being zoomed in or
    out.

    This produces the effect of "concentrating" on a zooming center: with each
    zoom-in step one would get closer and closer to the point, while point's
    surroundings would go "out of sight", beyound the viewport's borders.

    Zooming out works in backwards order: everything is like being "sucked into"
    the zooming center.

    If param \c viewportRelative is \c true then the given coordinates are
    treated as relative to the viewport top-left corner. Otherwise they're
    considered to be absolute i.e. relative to canvas top-left corner.

    @param scaleChange       a multiplier for a current scale value
    @param coords            coordinates of the "zooming center"
    @param viewportRelative  tells if coordinates are absolute or relative to viewport
  */
  void zoomAroundPoint(double scaleChange, QPoint coords, bool viewportRelative);
  double zoomBy(double);
  void  showAll();
  /// Scrolls so that \a modelPoint sits at the centre of the viewport, at
  /// the current scale (the model is widened if the point is outside it).
  void centerOn(const QPoint& modelPoint);

  /**
    Returns a rectangle which describes the model plane of the schematic.
    The rectangle is a copy, changes made to it do not affect schematic state.
  */
  QRect modelRect();

  /**
    Returns a rectangle which describes the viewport. Top-left corner is (0,0),
    width and height are equal to viewport's width and height.
  */
  QRect viewportRect();

  /// How far from the origin the model plane reaches, in model units.
  /// Elements further away (a file or a dialog can put them anywhere an
  /// int reaches) are not scrolled to, and the canvas - the model plane
  /// times the scale, which is at most 10 - stays well within the int
  /// arithmetic of QRect and Q3ScrollView.
  static constexpr int ModelLimit = 1 << 23;
  /// \a rect, \a point cut to the model plane's limits (corner by corner,
  /// without computing a width that could overflow).
  static QRect withinModelLimit(const QRect& rect);
  static QPoint withinModelLimit(const QPoint& point);
  void zoomToSelection();
  void  showNoZoom();
  void  enlargeView(const Element* e);
  void  switchPaintMode();
  int   adjustPortNumbers();
  int   orderSymbolPorts();
  /// The label on the net \a node sits on, empty when it carries none.
  QString netLabelOf(Node* node) const;
  /// The name the netlist gives the pin a subcircuit port makes: the
  /// label of the net the port sits on, or the port's own name when that
  /// net carries none. This is what the symbol writes beside the pin.
  QString portPinName(Component* port) const;
  void  reloadGraphs();
  bool  createSubcircuitSymbol();
  /// Throws the symbol's drawing away and lays the ports out around a
  /// fresh box; the port numbers stay as the schematic has them.
  bool  recreateSubcircuitSymbol();
  void  buildDefaultSymbol(std::size_t port_count);

  /**
    @brief Given cordinates of a model point returns coordinates of this point
           relative to viewport. It's a reverse of @ref Schematic::viewportToModel
  */
  QPoint modelToViewport(const QPoint& modelCoordinates);

  /**
    Given a coordinates of viewport point returns coordinates of the model plane point
    displayed at given location of the viewport.
  */
  QPoint viewportToModel(const QPoint& viewportCoordinates);

  /**
    Given coordinates of a point on the view plane (schematic's canvas), this method
    returns coordinates of a corresponding point on the model plane.

    @param viewCoordinates a point on the view plane
    @return a corresponding point on the model plane
  */
  QPoint contentsToModel(const QPoint& viewCoordinates);

  /**
    Given coordinates of a point on the model plane, this method returns coordinates
    of a corresponding point on the view plane (schematic's canvas).

    @param modelCoordinates a point on the model plane
    @return a corresponding point on the view plane
  */
  QPoint modelToContents(const QPoint& modelCoordinates);

  void    cut();
  void    copy();
  bool    paste(QTextStream*, std::list<Element*>*);
  bool    load();
  int     save();
  bool    writeTo(const QString& path) override;
  /// Writes the symbol of this schematic into its own file.
  bool    saveSymbolToFile(const QString& path);
  /// Takes the drawing of the symbol from another file; the ports stay.
  /// Returns what went wrong, or an empty string.
  QString loadSymbolFromFile(const QString& path);
  int     saveSymbolCpp (void);
  int     saveSymbolJSON (void);
  int     savePropsJSON (void);
  void    becomeCurrent(bool);
  bool    undo();
  bool    redo();
  /// Moving the selection with the cursor keys is one undo step however
  /// many key presses it takes; while it is the latest step, Escape takes
  /// it back (the selection returns to where it was). A mouse press or any
  /// other edit ends the sequence.
  void    noteKeyboardMove();
  bool    cancelKeyboardMove();
  /// Ends the current cursor-key move sequence (a mouse press does this):
  /// the next arrow key starts a new undo step, Escape no longer reverts.
  void    endKeyboardMove() { a_keyboardMoveOpen = false; }

  void scrollUp(int);
  void scrollDown(int);
  void scrollLeft(int);
  void scrollRight(int);

  bool checkDplAndDatNames();

  /*! \brief Get (schematic) file reference */
  QFileInfo getFileInfo (void) { return a_FileInfo; }
  /*! \brief Set reference to file (schematic) */
  void setFileInfo(QString FileName) { a_FileInfo = QFileInfo(FileName); }

  QString getFrame_Text0() const { return a_Frame_Text0; }
  void setFrame_Text0(const QString value) { a_Frame_Text0 = value; }
  QString getFrame_Text1() const { return a_Frame_Text1; }
  void setFrame_Text1(const QString value) { a_Frame_Text1 = value; }
  QString getFrame_Text2() const { return a_Frame_Text2; }
  void setFrame_Text2(const QString value) { a_Frame_Text2 = value; }
  QString getFrame_Text3() const { return a_Frame_Text3; }
  void setFrame_Text3(const QString value) { a_Frame_Text3 = value; }
  FrameSize getShowFrame() const { return a_showFrame; }
  void setShowFrame(int value) {a_showFrame = static_cast<FrameSize>(value);}
  int getViewX1() const { return a_ViewX1; }
  int getViewY1() const { return a_ViewY1; }
  int getGridX() const { return a_GridX; }
  // setOnGrid() divides by the grid: never let it reach 0 (a damaged file,
  // or an emptied field in the document-settings dialog).
  void setGridX(int value) { a_GridX = std::max(1, value); }
  int getGridY() const { return a_GridY; }
  void setGridY(int value) { a_GridY = std::max(1, value); }
  void setGridColor(const QColor& color) { a_GridColor = color; }
  QColor getGridColor() const { return a_GridColor; }
  bool getSymbolMode() const { return a_symbolMode; }
  void setSymbolMode(bool value) { a_symbolMode = value; }
  bool getIsSymbolOnly() const { return a_isSymbolOnly; }
  void setIsSymbolOnly(bool value) { a_isSymbolOnly = value; }
  void clearPostedPaintEvents() { a_PostedPaintEvents.clear(); }

  // The pointers points to the current lists, either to the schematic
  // elements "Doc..." or to the symbol elements "SymbolPaints".
  std::list<Wire*> *a_Wires;
  std::list<Wire*> a_DocWires;
  std::list<Node*>* a_Nodes;
  std::list<Node*> a_DocNodes;
  std::list<Diagram*>* a_Diagrams;
  std::list<Diagram*> a_DocDiags;
  std::list<Painting*>* a_Paintings;
  std::list<Painting*> a_DocPaints;
  std::list<Component*>* a_Components;
  std::list<Component*> a_DocComps;

  std::list<Painting*> a_SymbolPaints;  // symbol definition for subcircuit

  /** Whether \a e is part of what is on screen now - the components,
      wires, nodes, diagrams (with their graphs and markers), paintings
      and labels of the schematic, or of the symbol in symbol mode. For
      holders of an Element* that the document may have freed or replaced
      since (the element under the mouse): a pointer it does not hold must
      not be used. */
  bool holds(const Element* e) const;
  /// Every element holds() is true for, for checking many at once.
  std::unordered_set<const Element*> heldElements() const;

private:
  /// Calls the mouse handler the application has chosen on its
  /// MouseActions (a_App->view).
  template <typename Handler, typename... Args>
  void callView(Handler handler, Args... args);

  // What a_Components, a_Wires, a_Nodes and a_Diagrams point to in symbol
  // mode, where only paintings belong: empty, unless a tool put something
  // there anyway. Each document has its own (they were once shared by all
  // documents, so an element put there showed up in every symbol, and
  // closing one document freed what the others still listed).
  std::list<Wire*> a_SymbolWires;
  std::list<Node*> a_SymbolNodes;
  std::list<Diagram*> a_SymbolDiags;
  std::list<Component*> a_SymbolComps;

  // Set while an IndexedInsertion is alive
  qucs_s::InsertionIndex* a_insertionIndex = nullptr;
  // Set while a BulkNaming is alive: for each name prefix asked for, the
  // number insertComponent() gives the next component named with it
  std::unordered_map<QString, int>* a_nextNumbers = nullptr;

  QList<PostedPaintEvent> a_PostedPaintEvents;

  bool a_symbolMode;  // true if in symbol painting mode
  bool a_isSymbolOnly;

  // Horizontal and vertical grid step, grid color.
  int a_GridX;
  int a_GridY;
  QColor a_GridColor;

  // Variables View* are the coordinates of top-level and bottom-right corners
  // of a rectangle representing the schematic "model". This
  // rectangle may grow and shrink when user scrolls the view, and its
  // coordinates change accordingly. Everything (elements, wires, etc.) lies
  // inside this rectangle. The size of this rectangle is the "logical" size
  // of the schematic. The comment in "renderModel" method describes how
  // these variables ("model") is used to draw the scematic.
  int a_ViewX1;
  int a_ViewY1;
  int a_ViewX2;
  int a_ViewY2;

  FrameSize a_showFrame; // Frame format
  QString a_Frame_Text0;
  QString a_Frame_Text1;
  QString a_Frame_Text2;
  QString a_Frame_Text3;

  // Two of those data sets are needed for Schematic and for symbol.
  // Which one is in "tmp..." depends on "symbolMode".
  double a_tmpScale;
  int a_tmpViewX1;
  int a_tmpViewY1;
  int a_tmpViewX2;
  int a_tmpViewY2;
  QRect a_tmpUsedArea;

  int a_undoActionIdx;
  QVector<QString *> a_undoAction;
  bool a_keyboardMoveOpen = false;   // the top undo entry is an unfinished cursor-key move
  int a_undoSymbolIdx;
  QVector<QString *> a_undoSymbol;    // undo stack for circuit symbol

  bool isImageFilePath(const QString& path); // Detect if a file is an image

signals:
  void signalCursorPosChanged(int, int, QString);
  void signalUndoState(bool);
  void signalRedoState(bool);
  void signalFileChanged(bool);
  /** The content was edited (setChanged(true)): what is checked of it,
      the electrical rules, may have changed. */
  void signalEdited();
  void signalComponentDeleted(Component *);
  /** Emitted after the whole document was replaced (undo, redo, reload).
      Every Element* obtained from this schematic before the signal is
      invalid; holders must drop or re-resolve their pointers. */
  void signalDocumentRebuilt(Schematic *);

protected:
  // overloaded function to get actions of user
  void drawContents(QPainter*, int, int, int, int)override;
  void contentsMouseMoveEvent(QMouseEvent*)override;
  void contentsMousePressEvent(QMouseEvent*)override;
  void contentsMouseDoubleClickEvent(QMouseEvent*)override;
  void contentsMouseReleaseEvent(QMouseEvent*)override;
  void contentsWheelEvent(QWheelEvent*)override;
  void contentsDropEvent(QDropEvent*)override;
  void contentsDragEnterEvent(QDragEnterEvent*)override;
  void contentsDragLeaveEvent(QDragLeaveEvent*)override;
  void contentsDragMoveEvent(QDragMoveEvent*)override;
  void contentsNativeGestureZoomEvent( QNativeGestureEvent* ) override;
  /// The canvas's tooltips (operatingPointTooltip()).
  bool eventFilter(QObject* watched, QEvent* event) override;

protected slots:
  void slotScrollUp();
  void slotScrollDown();
  void slotScrollLeft();
  void slotScrollRight();

private:
  // Describes the area occupied by all elements of schematic, i.e. it is
  // the union of bounding rectangles of all elements.
  // This rectangle exists in the same coordinate system as View*-rectangle
  QRect a_UsedArea;

  // Viewport-realative coordinates of the cursor between mouse movements.
  // Used in "pan with mouse" feature.
  QPoint a_previousCursorPosition;

  bool a_dragIsOkay;
  /*! \brief hold system-independent information about a schematic file */
  QFileInfo a_FileInfo;

  /**
    Tells whether the model should be rerendered. Model should be rendered
    if the given scale differs from the current one or if the point displayed
    at \a viewportCoords in the viewport differs from the point \a modelCoords.
    Otherwise there is no changes and no need to rerender.

    @param scale desired scale
    @param newModelBounds a rectangle describing the desired model bounds
    @param modelCoords coordinates of a point on the model plane
    @param viewportCoords coordinates of a point in the viewport.
  */
  bool shouldRender(const double& scale, const QRect& newModelBounds, const QPoint& modelCoords, const QPoint& viewportCoords);

  /**
    Renders schematic model on Q3ScrollView's contents at a given scale,
    and positions the contents so that the point \a modelPlaneCoords of the
    model is displayed at location \a viewportCoords of the viewport.

    There is no need to call "update" on Q3ScrollView after using this method.
    It is done as a part of rendering process.

    Usage examples:
    1. Imagine you want to handle user's right scroll and you want scrolling
       to be infinite. Each scroll has to "stretch" schematic model to the right.
       First step is to take current model and create a new desired
       model size from it by shifting its right bound.
       After scrolling you want rightmost point of the model to be diplayed
       at right bound of the viewport. Then second step is to take coordinates
       of top-right corner of @b new @b desired model and coordinates of top-rigth
       corner of viewport and pass to @c renderModel along with new model:
       @code
       renderModel(sameScale, newModel, newModel.topRight(), viewportRect().topRight());
       @endcode
    2. Suppose you want to zoom at some element, so that its center would be
       displayed at the center of the viewport after zooming.
       First, find coordinates of the element center
       Second, find coordinates of the viewport center
       Third, call @c renderModel:
       @code
       renderModel(zoomScale, sameModel, elementCenter, viewportRect().center());
       @endcode
    3. Imagine you want to scroll and zoom so that the point currently
       displayed at the center of the viewport would be at the viewport
       top-left corner after.
       @code
       renderModel(zoomScale, sameModel, viewportToModel(viewportRect().center()), viewportRect.topLeft());
       @endcode
    @param  scale            desired new scale. It is clipped when exceeds
                             a lower or upperlimit
    @param  newModelBounds   a rectangle describing the desired model bounds
    @param  modelPlaneCoords coordinates of a point somewhere within
                             \a newModelBounds
    @param  viewportCoords   coordinates of the point on the viewport where
                             \a modelPlaneCoords should be placed after rendering
    @return new scale value
  */
  double renderModel(double scale, QRect newModelBounds, QPoint modelPlaneCoords, QPoint viewportCoords);
  void drawElements(QPainter* painter);

public:
  /// The electrical net of the selected wires: every wire and node
  /// reached from them through nodes, through labels of the same name
  /// (a "Vout" here joins a "Vout" there) and through ground symbols
  /// (all grounds are one net). Empty when no wire is selected. Painted
  /// as a glow under the wires and nodes while the selection lasts.
  struct Net {
    std::unordered_set<Wire*> wires;
    std::unordered_set<Node*> nodes;
    bool empty() const { return wires.empty() && nodes.empty(); }
  };
  Net selectedNet() const;
  /// The net a wire belongs to, whether it is selected or not.
  Net netOf(Wire* wire) const;

  /// Whether the grid is drawn: the document's own flag (getGridOn(),
  /// saved in the file), unless the application's setting shows or hides
  /// the grid of every schematic (QucsSettings.GridMode); a data display
  /// keeps its own.
  bool gridShown() const;

  /// The DC bias labels on show (the values the nodes carry once the DC
  /// bias is shown) and where each goes, for text of these metrics.
  struct BiasLabels {
    QList<qucs_s::bias::Label> labels;
    QStringList texts;
    QList<qucs_s::bias::Placement> placements;
  };
  BiasLabels layoutBiasLabels(const QFontMetrics& metrics) const;

  /// The operating point of every device found by the last DC bias run
  /// (ngspice's "show all", oppoint.h): the Operating Point tab lists it,
  /// and a component's tooltip shows its own while the DC bias is shown.
  void setOperatingPoint(const QList<qucs_s::oppoint::Device>& devices) { a_operatingPoint = devices; }
  const QList<qucs_s::oppoint::Device>& operatingPoint() const { return a_operatingPoint; }
  /// The devices of a component: itself, or those inside a subcircuit.
  QList<const qucs_s::oppoint::Device*> operatingPointOf(const QString& component) const;
  /// The tooltip for the point \a viewportPos of the canvas: the operating
  /// point of the component there, if the DC bias is shown and it has one.
  QString operatingPointTooltip(const QPoint& viewportPos);
private:
  QList<qucs_s::oppoint::Device> a_operatingPoint;
  void drawNetHighlight(QPainter* painter, const Net& net);
  void drawDcBiasPoints(QPainter* painter);
  void drawPostPaintEvents(QPainter* painter);
  void paintFrame(QPainter* painter);
  void drawGrid(QPainter* painter);

/* ********************************************************************
   *****  The following methods are in the file                   *****
   *****  "schematic_element.cpp". They only access the QPtrList  *****
   *****  pointers "Wires", "Nodes", "Diagrams", "Paintings" and  *****
   *****  "Components".                                           *****
   ******************************************************************** */

public:
  // structs for node creation/deletion
  struct NodeDisconnectResult {
    bool disconnected;
    bool removed;
  };
  struct WireDisconnectResult {
    NodeDisconnectResult port1;
    NodeDisconnectResult port2;
  };
  struct CompDisconnectResult {
    std::vector<NodeDisconnectResult> ports;
  };

  Node* createNode(int, int) const;
  Node* createNode(const QPoint& p) const { return createNode(p.x(), p.y()); }
  Node* findNode(int, int) const;
  Node* findNode(const QPoint& p) const { return findNode(p.x(), p.y()); }
  Node* provideNode(int, int);
  Node* provideNode(const QPoint& p) { return provideNode(p.x(), p.y()); }
  Node* selectedNode(int, int);

  // While one is alive, findNode() and provideNode() look nodes and wires up
  // by place instead of going through all of them: for the loader, which
  // inserts every element of a document and moves or deletes none, and the
  // healer's run of node replacements. (Without it, loading took time
  // quadratic in the size of the document.) Nothing may move or delete a
  // node or a wire meanwhile.
  class IndexedInsertion {
  public:
    explicit IndexedInsertion(Schematic* doc);
    ~IndexedInsertion();
    IndexedInsertion(const IndexedInsertion&) = delete;
    IndexedInsertion& operator=(const IndexedInsertion&) = delete;
  private:
    Schematic* m_doc;
    bool m_owner;
  };

  qucs_s::wire::Planner a_wirePlanner;
  std::pair<bool,Node*> connectWithWire(const QPoint& a, const QPoint& b) noexcept;
  std::pair<bool,Node*> connectWithWire(const QPoint& a, const QPoint& b, bool optimize, qucs_s::wire::Planner::PlanType planType) noexcept;
  void showEphemeralWire(const QPoint& a, const QPoint& b) noexcept;
  bool  optimizeWires();
  std::pair<bool,Node*> installWire(Wire* wire);
  void displayMutations();

  struct HealingParams;
  bool heal(const HealingParams* params);
  bool healAfterMousyMutation();
  bool healAfterKeyboardMutation();

  void dumbConnectWithWire(const QPoint& a, const QPoint& b) noexcept;

  void  selectWireLine(Wire*, Node*, bool);
  Wire* selectedWire(int, int);
  Wire* splitWire(Wire*, Node*);
  void  deleteWire(Wire*, bool remove_orphans=true);
  void  deleteWires(const std::vector<Wire*>&, bool remove_orphans=true);
  WireDisconnectResult disconnectWire(Wire*, bool remove_orphans=true, bool keepNodeLabel=false);
  void  decoupleWire(Wire*, bool keepNodeLabel=false, bool remove_orphans=true);

  Marker* setMarker(int, int);
  void    markerLeftRight(bool, const std::vector<Marker*>& markers);
  void    markerUpDown(bool, const std::vector<Marker*>& markers);

  Element* selectElement(float, float, bool, int *index=0);
  void     deselectElements(Element*) const;
  int      selectElements(const QRect&, bool, bool) const;
  void     selectMarkers() const;
  bool     deleteElements();
  bool     aligning(int);
  bool     distributeHorizontal();
  bool     distributeVertical();

  void       setComponentNumber(Component*);
  void       insertRawComponent(Component*, bool noOptimize=true);
  void       recreateComponent(Component*);
  void       insertComponent(Component*);

  // While one is alive, insertComponent() numbers the name of a new
  // component from a table of the numbers in use for each name prefix, not
  // by going through every component (and every name to a number) for each:
  // for pasting many components into a large schematic. Nothing else may
  // rename components meanwhile.
  class BulkNaming {
  public:
    explicit BulkNaming(Schematic* doc);
    ~BulkNaming();
    BulkNaming(const BulkNaming&) = delete;
    BulkNaming& operator=(const BulkNaming&) = delete;
  private:
    Schematic* m_doc;
    bool m_owner;
  };

  void       activateCompsWithinRect(int, int, int, int);
  bool       activateSpecifiedComponent(int, int);
  bool       activateSelectedComponents();
  Component* selectCompText(int, int, int&, int&) const;
  Component* searchSelSubcircuit();
  void       deleteComp(Component*, bool remove_orphans=true);
  void       deleteComps(const std::vector<Component*>&);
  void       detachComp(Component*, bool remove_orphans=true, bool keepNodeLabel=false);
  void       decoupleComp(Component*, bool keepNodeLabel=false, bool remove_orphans=true);
  Component* getComponentByName(const QString& compname) const;
  CompDisconnectResult disconnectComp(Component*, bool remove_orphans=true, bool keepNodeLabel=false);

  void     oneLabel(Node*);
  int      placeNodeLabel(WireLabel*);
  Element* getWireLabel(Node*);

  Painting* selectedPainting(float, float);


private:
  void insertComponentNodes(Component*, bool);
  NodeDisconnectResult disconnectNode(Node*, Element*, bool remove_orphans=true, bool keepNodeLabel=false) const;

/* ********************************************************************
   *****  The following methods are in the file                   *****
   *****  "schematic_file.cpp". They only access the QPtrLists    *****
   *****  and their pointers. ("DocComps", "Components" etc.)     *****
   ******************************************************************** */

public:
  static int testFile(const QString &);
  bool createLibNetlist(QTextStream*, QPlainTextEdit*, int);
  bool createSubNetlist(QTextStream *, int&, QStringList&, QPlainTextEdit*, int);
  void createSubNetlistPlain(QTextStream*, QPlainTextEdit*, int);
  int  prepareNetlist(QTextStream&, QStringList&, QPlainTextEdit*);
  QString createNetlist(QTextStream&, int);
  bool isDigitalCircuit();
  bool loadDocument();
  /// The document as its file would hold it: the one in memory, unsaved
  /// changes and all.
  QString documentText();
  /// Puts the elements of \a text - a schematic file, or any of its
  /// <Components>, <Wires>, <Diagrams> and <Paintings> sections (those
  /// left out stay as they are) - in place of this schematic's, as one
  /// step to undo. False, and why in \a error, when it does not read:
  /// the schematic is then as it was.
  bool replaceContent(const QString& text, QString* error = nullptr);
  void highlightWireLabels (void);
  void clearSignalsAndFileList();
  void clearSignals();

  void setIsAnalog(bool value) { a_isAnalog = value; }
  bool getIsAnalog() const { return a_isAnalog; }
  void setIsVerilog(bool value) { a_isVerilog = value; }
  bool getIsVerilog() const { return a_isVerilog; }
  bool giveNodeNames(QTextStream *, int&, QStringList&, QPlainTextEdit*, int);

private:
  int  saveDocument();
  bool writeDocument(const QString& path);   // the serialisation part of saveDocument()
  void writeDocumentTo(QTextStream& stream);

  bool loadProperties(QTextStream*);
  void simpleInsertComponent(Component*);
  bool loadComponents(QTextStream*, std::list<Component*> *List=0);
  void simpleInsertWire(Wire*);
  bool loadWires(QTextStream*, std::list<Element*> *List=0);
  bool loadDiagrams(QTextStream*, std::list<Diagram*>*);
  bool loadPaintings(QTextStream*, std::list<Painting*>*);
  bool loadIntoNothing(QTextStream*);

  QString createClipboardFile();
  bool    pasteFromClipboard(QTextStream *, std::list<Element*>*);

  QString createUndoString(char);
  bool    rebuild(QString *);
  QString createSymbolUndoString(char);
  bool    rebuildSymbol(QString *);

  static void createNodeSet(QStringList&, int&, Conductor*, Node*);
  void throughAllNodes(bool, QStringList&, int&);
  void nameUnlabelledPortNets(QStringList&, int&);
  void propagateNode(QStringList&, int&, Node*);
  void collectDigitalSignals(void);
  void beginNetlistDigital(QTextStream &);
  void endNetlistDigital(QTextStream &);
  bool throughAllComps(QTextStream *, int&, QStringList&, QPlainTextEdit *, int);

  DigMap a_Signals; // collecting node names for VHDL signal declarations
  QStringList a_PortTypes;

  bool a_isAnalog;
  bool a_isVerilog;
  bool a_creatingLib;
};

#endif
