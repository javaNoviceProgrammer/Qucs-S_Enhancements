/*
 * thread_clock.h - the calling thread's processor time, for the tests that
 *                  judge how a time grows with its input (four times the
 *                  text, under eight times the time)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_TESTS_THREAD_CLOCK_H
#define QUCS_TESTS_THREAD_CLOCK_H

#include <QElapsedTimer>
#include <QtGlobal>

#if defined(Q_OS_UNIX)
#include <time.h>
#endif

namespace qucs_test {

/// The calling thread's processor time, in nanoseconds. A wall clock counts
/// the time the thread waited for a processor too: on CI's macOS runner,
/// three processors and three tests at once, a run past the scheduler's
/// quantum (some 10 ms) lost one or more to the others where a run of 3 ms
/// fitted in one, and four times the text read as 8 to 10 times the time
/// (test_claude_code's aLongReplyIsReadInLinearTime 3.2 then 33.3 ms on 10
/// November), its best of five alike; this Mac's 24 processors kept busy
/// by 32 loops did the same (19.8 then 221 ms). (Where no such clock is -
/// Windows - the wall clock's.)
inline qint64 threadNanoseconds()
{
#if defined(Q_OS_UNIX) && defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return qint64(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#else
    static QElapsedTimer since;
    if (!since.isValid()) since.start();
    return since.nsecsElapsed();
#endif
}

/// QElapsedTimer's start() and nsecsElapsed() by threadNanoseconds(): for
/// work done on the calling thread alone.
class ThreadClock
{
public:
    void start() { a_from = threadNanoseconds(); }
    qint64 nsecsElapsed() const { return threadNanoseconds() - a_from; }

private:
    qint64 a_from = threadNanoseconds();
};

} // namespace qucs_test

#endif
