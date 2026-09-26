/*
 * levelofdetail.cpp - what the canvas leaves out when zoomed out too far
 *                     to show it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "levelofdetail.h"

namespace qucs_s::lod {

namespace {
bool& hidden()
{
    static bool texts = false;
    return texts;
}
} // namespace

HiddenTexts::HiddenTexts(bool hide) : m_previous(hidden())
{
    hidden() = hide;
}

HiddenTexts::~HiddenTexts() { hidden() = m_previous; }

bool textsShown() { return !hidden(); }

} // namespace qucs_s::lod
