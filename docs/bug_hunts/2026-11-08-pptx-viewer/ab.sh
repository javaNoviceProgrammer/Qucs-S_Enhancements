#!/bin/sh
# ab.sh: the tab timed with the sources before (HEAD) and after, two passes each
S="${S:?the scratch folder: decks/, ab/before and ab/after (the sources of each side and test_pptx_doc.cpp)}"
W="${W:?the worktree}"
Q="$W/qucs-s-26.1.1/qucs"
export PATH="$(brew --prefix bison)/bin:$PATH"
put() { for f in pptx.cpp pptx.h pptxdoc.cpp pptxdoc.h zipfile.cpp; do cp "$S/ab/$1/$f" "$Q/$f"; done; cp "$S/ab/$1/test_pptx_doc.cpp" "$Q/tests/test_pptx_doc.cpp"; ninja -C "$W/build" test_pptx_doc > "$S/ab/build-$1.log" 2>&1 || echo "BUILD FAILED $1"; }
for pass in 1 2; do
  for side in before after; do
    put $side
    S=$S sh "$(dirname "$0")/bench.sh" "$W/build" "$S/ab/$side-$pass.txt" > /dev/null
    echo "$side $pass done"
  done
done
put after
echo restored
