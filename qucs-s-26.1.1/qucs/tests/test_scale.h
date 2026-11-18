/*
 * test_scale.h - a test's own waits scaled as the build scales its limits
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * QUCS_TEST_TIMEOUT_SCALE multiplies each test's ctest limit and QtTest's
 * limit on one function (qucs/tests/CMakeLists.txt); CI's sanitized Debug
 * job sets 3. It reaches a test as QUCS_TEST_SCALE, where it is not 1, so
 * that a wait the test sets itself - a QTRY's timeout, a solve waited for -
 * grows with it: the build's work takes that much longer, not the thing
 * waited for that much longer to happen. Elsewhere 1: a Release build's
 * waits are as written.
 */
#pragma once

#include <QtGlobal>

#include <algorithm>

namespace qucs_test {

/// The build's scale of its tests' limits: 1 unless it says otherwise.
inline int scale()
{
    static const int s = std::max(1, qEnvironmentVariableIntValue("QUCS_TEST_SCALE"));
    return s;
}

/// \a ms of waiting, as this build's limits are scaled.
inline int scaled(int ms)
{
    return ms * scale();
}

} // namespace qucs_test
