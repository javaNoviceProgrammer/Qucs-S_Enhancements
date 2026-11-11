// Included ahead of every C++ file, where cmake/qt_macos.cmake found it
// needed: Qt 6.10's <QtCore/qyieldcpu.h> calls __yield(), which
// <arm_acle.h> declares, and current Apple clang will not take it undeclared.
// ARM only: <arm_acle.h> is an error anywhere else, as in the Intel half of a
// universal build.
#if defined(__aarch64__) || defined(__arm__)
#include <arm_acle.h>
#endif
