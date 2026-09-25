#include "property.h"
#include <QPainter>


// x and y are relative to component's x and y
void Property::paint(int x, int y, QPainter* p)
{
  p->drawText(x, y, 1, 1, Qt::TextDontClip, displayText(), &br);
}

QString Property::displayText() const
{
  if (Value.size() <= MaxShownValue) return Name + "=" + Value;
  return Name + "=" + Value.left(MaxShownValue) + QChar(0x2026);
}