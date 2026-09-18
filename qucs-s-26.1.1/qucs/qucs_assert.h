/***************************************************************************
                               qucs_assert.h
                              ---------------
    Invariant checks that do not disappear in release builds.
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QUCS_ASSERT_H
#define QUCS_ASSERT_H

#include <cassert>
#include <QtGlobal>

/*!
 * QUCS_ASSERT(cond) replaces assert(cond) throughout the application.
 *
 * In a Debug build it is assert(): the process aborts with the condition,
 * file and line, which is what you want while developing.
 *
 * In a Release build - every binary users run - assert() compiles to nothing,
 * so the invariants it protects were violated silently and the corrupted
 * state surfaced as an unrelated crash much later (upstream closes many such
 * reports as "cannot reproduce"). QUCS_ASSERT instead logs the violation and
 * lets execution continue exactly as it did before: the behaviour of the
 * program is unchanged, but the log (and the crash report, see
 * crashhandler.h) now says which invariant broke first.
 */
#ifdef NDEBUG
#define QUCS_ASSERT(cond)                                                  \
  do {                                                                     \
    if (!(cond))                                                           \
      qWarning("Invariant violated: %s (%s:%d)", #cond, __FILE__, __LINE__); \
  } while (false)
#else
#define QUCS_ASSERT(cond) assert(cond)
#endif

#endif // QUCS_ASSERT_H
