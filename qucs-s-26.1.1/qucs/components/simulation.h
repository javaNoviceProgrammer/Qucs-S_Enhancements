#ifndef SIMULATION_H
#define SIMULATION_H
#include "component.h"
#include <QString>

namespace qucs::component {
class SimulationComponent : public Component {
private:
     QString label_text;
     void updateComponentBounds(const QRect& label_bounds);
     QPen pen() const;
     /// The box drawSymbol() draws around the label, measured by the font
     /// alone: the same before the component is first painted as after.
     QRect labelBox() const;
protected:
     void initSymbol(const QString& label);
     void drawSymbol(QPainter* p) override;
public:
     /// Loaded, its bounds and text place as a paint makes them - not the
     /// file's until the first paint (which made arrange lay a schematic
     /// out one way or another, as the paint came before it or after).
     bool load(const QString& s) override;
     // Override in your subclass if you want other color
     // for your simulation component
     virtual Qt::GlobalColor color() const { return Qt::darkBlue; }
};
}
#endif