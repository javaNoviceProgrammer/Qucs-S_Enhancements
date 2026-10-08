# qucs/diagrams library

INCLUDE_DIRECTORIES( ${CMAKE_CURRENT_SOURCE_DIR} )

#INCLUDES = $(X11_INCLUDES) $(QT_INCLUDES) -I$(top_srcdir)/qucs

SET(DIAGRAMS_HDRS
curvediagram.h
diagram.h
diagramdialog.h
dataexportpanel.h
dataimportpanel.h
diagrams.h
diagramtheme.h
graph.h
marker.h
markerdialog.h
polardiagram.h
psdiagram.h
rect3ddiagram.h
rectdiagram.h
histogramdiagram.h
eyediagram.h
stackeddiagram.h
speclimits.h
polezerodiagram.h
bodediagram.h
nicholsdiagram.h
spectrumdiagram.h
bathtubdiagram.h
contourdiagram.h
spectrogramdiagram.h
tornadodiagram.h
boxplotdiagram.h
smithcircles.h
constellationdiagram.h
smithdiagram.h
tabdiagram.h
timingdiagram.h
truthdiagram.h
)

SET(DIAGRAMS_SRCS
curvediagram.cpp	graph.cpp		polardiagram.cpp	smithdiagram.cpp
diagram.cpp		marker.cpp		psdiagram.cpp		tabdiagram.cpp
diagramdialog.cpp	markerdialog.cpp	rect3ddiagram.cpp	timingdiagram.cpp
rectdiagram.cpp		truthdiagram.cpp	histogramdiagram.cpp	eyediagram.cpp
diagramtheme.cpp
stackeddiagram.cpp
speclimits.cpp
polezerodiagram.cpp
bodediagram.cpp
nicholsdiagram.cpp
spectrumdiagram.cpp
bathtubdiagram.cpp
contourdiagram.cpp
spectrogramdiagram.cpp
tornadodiagram.cpp
boxplotdiagram.cpp
smithcircles.cpp
constellationdiagram.cpp
dataexportpanel.cpp
dataimportpanel.cpp
)

SET(DIAGRAMS_MOC_HDRS
diagramdialog.h
dataexportpanel.h
dataimportpanel.h
markerdialog.h
)

qt6_wrap_cpp( DIAGRAMS_MOC_SRCS ${DIAGRAMS_MOC_HDRS} )

ADD_LIBRARY(diagrams STATIC ${DIAGRAMS_HDRS} ${DIAGRAMS_SRCS} ${DIAGRAMS_MOC_SRCS})
