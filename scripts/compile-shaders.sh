#!/bin/sh
# Compiles the GPU viewers' shaders for Qt's RHI: each
# qucs-s-26.1.1/qucs/shaders/*.vert and *.frag (Vulkan-style GLSL 440) into
# its .qsb - SPIR-V for Vulkan, GLSL for OpenGL (core and ES), HLSL for
# Direct3D, MSL for Metal - kept in the repository beside it, so that a build
# needs no Qt Shader Tools. Run it after a shader is changed; it writes
# manifest.sha256, the sources' digests, which scripts/ci/check-shaders.py
# checks against the sources (a .qsb older than its source fails CI).
#   QSB=<qsb> scripts/compile-shaders.sh
set -e
QSB=${QSB:-qsb}
cd "$(dirname "$0")/../qucs-s-26.1.1/qucs/shaders"
for s in *.vert *.frag; do
    "$QSB" --glsl "300 es,130,150,330" --hlsl 50 --msl 12 -o "$s.qsb" "$s"
done
shasum -a 256 *.vert *.frag > manifest.sha256
echo "compiled: $(ls *.qsb | wc -l | tr -d ' ') shaders"
