#!/bin/sh
# bench.sh <tree> <out>: the tab timed on each deck, at device pixel ratios 1 and 2
S="${S:?the folder of the decks, decks/ in it}"
: > "$2"
for sf in 1 2; do for d in text10 shapes50 photos200 text3000; do
  HOME=$TMPDIR/home QT_SCALE_FACTOR=$sf QUCS_PPTX_BENCH=$S/decks/$d.pptx ctest --test-dir "$1" -R test_pptx_doc -V 2>&1 | grep -E "slides, open|FAIL!|Failed" | sed 's/^[0-9]*: QINFO  : TestPptxDoc::benchDecksGiven() //' >> "$2"
done; done
cat "$2"
