# Writes OUTPUT, a header that defines QUCS_COMMIT: the abbreviated hash of
# the commit SOURCE_DIR is checked out at - git finds the repository wherever
# it is above the folder (the source tree may be one folder of it) - else
# FALLBACK (-DGIT, for a source tree with no repository), else "".
#
# Run at every build (the qucs-commit target), it writes the file only when
# the hash changed: the About box, --version and a crash report name the
# commit the build is of. Read once when the tree was configured, the hash
# named that commit long after, and a -DGIT given once stayed in the cache.
#
#   cmake -DSOURCE_DIR=<dir> -DFALLBACK=<hash> -DOUTPUT=<file> -P commit.cmake
set(commit "${FALLBACK}")
find_program(GIT_PROGRAM git)
if(GIT_PROGRAM)
  execute_process(
    COMMAND "${GIT_PROGRAM}" -C "${SOURCE_DIR}" rev-parse --short=7 HEAD
    OUTPUT_VARIABLE head
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE failed
    ERROR_QUIET)
  if(NOT failed AND NOT head STREQUAL "")
    set(commit "${head}")
  endif()
endif()
set(text "// Written at every build by cmake/commit.cmake: the commit this build is of.\n#define QUCS_COMMIT \"${commit}\"\n")
set(old "")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUTPUT}" "${text}")
endif()
