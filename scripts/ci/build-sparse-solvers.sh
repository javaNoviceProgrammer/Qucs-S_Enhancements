#!/usr/bin/env bash
#
# The multiphysics solver's optional sparse direct solvers, built from their
# sources for CI and the Linux releases (docs/feature_gaps/
# 2026-11-09-linux-parallel-solvers.md): MUMPS - sequential (its MPI stubs),
# multithreaded by OpenMP, METIS's and PORD's orderings - and STRUMPACK -
# OpenMP, METIS, no MPI, no GPU -, each a static library of position-
# independent code, into <prefix>/include and <prefix>/lib: linked into the
# programs, the AppImage carries no library of theirs, only the system's
# they need (OpenBLAS, METIS, libgfortran, libgomp), which linuxdeploy
# copies. (MUMPS's own Makefile gives its shared libraries no SONAME: linked,
# a program would ask for them by this build's paths.) Each source is
# checked against its SHA-256 before it is unpacked. Debian's and Ubuntu's
# libmumps-seq-dev is built without OpenMP (its threads only its BLAS's),
# and no distribution packages STRUMPACK: the releases carry these.
#
# Usage:
#   build-sparse-solvers.sh <prefix> [mumps] [strumpack]     (both if neither)
# Then: cmake ... -DMUMPS_ROOT=<prefix> -DMUMPS_EXTRA_LIBRARIES="metis;openblas;gfortran;gomp"
#                 -DSTRUMPACK_DIR=<prefix>/lib/cmake/STRUMPACK
#
# Needs a C, C++ and Fortran compiler (CC, CXX, FC: gcc, g++, gfortran by
# default), make, cmake, ninja, curl, METIS (libmetis-dev) and an OpenMP
# BLAS and LAPACK (libopenblas-openmp-dev: OpenBLAS's pthreads flavour would
# start its own threads inside the solvers' OpenMP ones). METIS_LIBRARY and
# BLAS_LIBRARY name the libraries where they are not found under
# /usr/lib*/; JOBS the build's parallelism.
set -euo pipefail

prefix="${1:?usage: build-sparse-solvers.sh <prefix> [mumps] [strumpack]}"
shift
want_mumps=0
want_strumpack=0
if [ "$#" -eq 0 ]; then
  want_mumps=1
  want_strumpack=1
fi
for what in "$@"; do
  case "$what" in
    mumps) want_mumps=1 ;;
    strumpack) want_strumpack=1 ;;
    *) echo "unknown solver: $what (mumps, strumpack)" >&2; exit 2 ;;
  esac
done

MUMPS_VERSION=5.9.1
MUMPS_SHA256=659c9b57646b5a003ac618baa1faf9dd2044e46c732b3daaccbc7158003e1b46
STRUMPACK_VERSION=8.1.0
STRUMPACK_SHA256=f86a05d3ad9e87ad8a6e1008d15bf3e628f133bfe0a52d4268354a342cbe03d0

CC="${CC:-gcc}"
CXX="${CXX:-g++}"
FC="${FC:-gfortran}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
mkdir -p "$prefix/include" "$prefix/lib"
prefix="$(cd "$prefix" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The first of a library's files under the system's library folders.
find_library_file() {
  find /usr/lib /usr/lib64 /usr/local/lib -name "$1" 2>/dev/null | sort | head -n 1
}
METIS_LIBRARY="${METIS_LIBRARY:-$(find_library_file libmetis.so)}"
BLAS_LIBRARY="${BLAS_LIBRARY:-$(find_library_file libopenblas.so)}"
[ -n "$METIS_LIBRARY" ] || { echo "no libmetis.so: install libmetis-dev or set METIS_LIBRARY" >&2; exit 1; }
[ -n "$BLAS_LIBRARY" ] || { echo "no libopenblas.so: install libopenblas-openmp-dev or set BLAS_LIBRARY" >&2; exit 1; }
METIS_INCLUDE="${METIS_INCLUDE:-/usr/include}"
echo "METIS: $METIS_LIBRARY; BLAS and LAPACK: $BLAS_LIBRARY; compilers: $CC, $CXX, $FC"

fetch() {   # fetch <url> <file> <sha256>
  curl -fsSL --retry 3 --retry-delay 5 -o "$2" "$1"
  echo "$3  $2" | sha256sum -c -
}

if [ "$want_mumps" -eq 1 ]; then
  fetch "https://mumps-solver.org/MUMPS_${MUMPS_VERSION}.tar.gz" "$work/mumps.tar.gz" "$MUMPS_SHA256"
  tar -xzf "$work/mumps.tar.gz" -C "$work"
  src="$work/MUMPS_${MUMPS_VERSION}"
  blas_dir="$(dirname "$BLAS_LIBRARY")"
  metis_dir="$(dirname "$METIS_LIBRARY")"
  # (Its own Makefile.inc, after Make.inc/Makefile.debian.SEQ: METIS and
  # PORD, OpenMP, position-independent code - CI's tests link a shared
  # library of the core.)
  cat > "$src/Makefile.inc" <<EOF
LPORDDIR = \$(topdir)/PORD/lib/
IPORD    = -I\$(topdir)/PORD/include/
LPORD    = -L\$(LPORDDIR) -lpord\$(PLAT)
LMETIS   = -L$metis_dir -lmetis
IMETIS   = -I$METIS_INCLUDE
ORDERINGSF = -Dmetis -Dpord
ORDERINGSC = \$(ORDERINGSF)
LORDERINGS = \$(LMETIS) \$(LPORD)
IORDERINGSF =
IORDERINGSC = \$(IMETIS) \$(IPORD)
PLAT    =
LIBEXT  = .a
OUTC    = -o
OUTF    = -o
RM      = /bin/rm -f
CC      = $CC
FC      = $FC
FL      = $FC
AR      = ar vr
RANLIB  = ranlib
LAPACK  = -L$blas_dir -lopenblas
INCSEQ  = -I\$(topdir)/libseq
LIBSEQ  = \$(LAPACK) -L\$(topdir)/libseq -lmpiseq\$(PLAT)
LIBBLAS = -L$blas_dir -lopenblas
LIBOTHERS = -lpthread
CDEFS   = -DAdd_
OPTF    = -O2 -fPIC -fopenmp -fallow-argument-mismatch
OPTL    = -O2 -fopenmp
OPTC    = -O2 -fPIC -fopenmp
INCS    = \$(INCSEQ)
LIBS    = \$(LIBSEQ)
LIBSEQNEEDED = libseqneeded
EOF
  make -C "$src" -j "$JOBS" d z
  cp "$src"/include/*.h "$prefix/include/"
  for lib in dmumps zmumps mumps_common pord mpiseq; do
    cp "$src/lib/lib$lib.a" "$prefix/lib/" 2>/dev/null || cp "$src"/*/"lib$lib.a" "$prefix/lib/"
  done
  echo "MUMPS $MUMPS_VERSION in $prefix:"
  ls -l "$prefix"/lib/lib*mumps*.a "$prefix"/lib/libpord.a "$prefix"/lib/libmpiseq.a
fi

if [ "$want_strumpack" -eq 1 ]; then
  fetch "https://github.com/pghysels/STRUMPACK/archive/refs/tags/v${STRUMPACK_VERSION}.tar.gz" "$work/strumpack.tar.gz" "$STRUMPACK_SHA256"
  tar -xzf "$work/strumpack.tar.gz" -C "$work"
  cmake -S "$work/STRUMPACK-${STRUMPACK_VERSION}" -B "$work/strumpack-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_Fortran_COMPILER="$FC" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
    -DSTRUMPACK_USE_MPI=OFF -DSTRUMPACK_USE_OPENMP=ON \
    -DTPL_ENABLE_SLATE=OFF -DTPL_ENABLE_PARMETIS=OFF -DTPL_ENABLE_SCOTCH=OFF -DTPL_ENABLE_PTSCOTCH=OFF \
    -DTPL_ENABLE_BPACK=OFF -DTPL_ENABLE_ZFP=OFF \
    -DTPL_BLAS_LIBRARIES="$BLAS_LIBRARY" -DTPL_LAPACK_LIBRARIES="$BLAS_LIBRARY" \
    -DTPL_METIS_INCLUDE_DIRS="$METIS_INCLUDE" -DTPL_METIS_LIBRARIES="$METIS_LIBRARY"
  cmake --build "$work/strumpack-build" --parallel "$JOBS"
  cmake --install "$work/strumpack-build"
  echo "STRUMPACK $STRUMPACK_VERSION in $prefix:"
  ls -l "$prefix"/lib/libstrumpack*
fi
