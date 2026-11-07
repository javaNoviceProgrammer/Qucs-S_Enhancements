#!/bin/sh
# ROOT: the repository; $ROOT/build-mesh a Release build of it.
ROOT="${ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}"
# compile $1.cpp against the worktree's fem
B="$ROOT/build-mesh"; S="$ROOT/qucs-s-26.1.1/qucs/multiphysics"
exec c++ -std=c++20 -O2 -DNDEBUG -DQT_NO_DEBUG_OUTPUT -I"$S" -I"$ROOT/qucs-s-26.1.1/third_party/eigen" -I"$ROOT/qucs-s-26.1.1/third_party" -iframework /opt/homebrew/opt/qt/lib -I/opt/homebrew/opt/qt/include -I/opt/homebrew/opt/qt/lib/QtCore.framework/Headers "$1.cpp" -o "$1" -F/opt/homebrew/opt/qt/lib -Wl,-rpath,/opt/homebrew/opt/qt/lib "$B/qucs/multiphysics/libfem.a" "$B/third_party/gdstk/libgdstk.a" -lz -weak_framework Accelerate -framework QtCore -framework DiskArbitration -framework UniformTypeIdentifiers -framework Security -framework AppKit -framework ApplicationServices -framework Foundation -framework IOKit
