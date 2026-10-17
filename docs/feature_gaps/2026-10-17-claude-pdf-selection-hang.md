# Selecting text in a PDF: seconds of no answer

The report (2026-10-17): *"When I open a pdf in the pdfviewer and try to select some text it hangs for a few seconds and becomes unresponsive. why?"*

## Reproduced

The Liu 2013 paper (~/QucsWorkspace/liu2013_phase_shifter/Liu2013_JLT.pdf, copied), one drag on each of its first three pages:

| Page | Characters selected (of the page's) | First selection on it, before | After |
|---|---|---|---|
| 1 | 3,977 (of 6,283) | 11.2 to 11.5 s | 21 ms |
| 2 | 736 | 4.7 s | 14 ms |
| 3 | 3,572 | 11.7 to 12.2 s | 21 ms |

Later drags on a page took 1 to 2 ms before and take 5 to 10 ms now. The program did not answer during the first: it ran on the GUI thread. The GPU's drawing had no part in it.

**Why.**
- To find the character under the mouse - the nearest, taken once the drag is past its middle - the view built every character's box on the page first (`charBoxes`), each by `QPdfDocument::getSelectionAtIndex(page, i, 1)`, cached for the page.
- Qt's PDF module loads the page and its text in pdfium again at each such call: about 1.8 ms each, so 6,283 calls took 11 s.

## Now

- **pdfium's own hit test,** through `QPdfDocument::getSelection(page, from, to)`, finds the character under a point.
  - As probed: a selection from the page's first character (one with a box) to the point ends at the point's character. On that first character itself, the selection from it to the page's last character starts there.
  - Off the text, beyond pdfium's tolerance, there is none.
- **Off the text** (a margin, between lines or columns): the point is brought into the nearest run of the page's text (`getAllText`'s boxes: a line, or its part in one font, the line first, as before). On a gap there (a space) the search goes along the run a step at a time out from the point, then to the next nearest runs.
- **What a page needs,** once:
  - its text's runs (4 ms);
  - its first and last characters with a box.
- **A drag costs a few calls a move,** and the press point's answer is kept for the drag.
- **Over every 17 by 13 points of three pages,** against the boxes as before:
  - **87 to 90 % of the points find the same character or its neighbour.** pdfium's hit test and the nearest box differ at the boundary between two characters, and the hit test can return the space between words, which has no box of its own in Qt.
  - **None find nothing.** The search along the run fixed the 41 points of page 3 that had first found nothing.

## Tests

- **test_pdf_viewer `aFullPageIsSelectedAtOnce` (new),** on a page of 4,181 characters:
  - its first selection 31 ms on this machine, held to under 1 s;
  - the next under 0.2 s;
  - from the middle of "quick" to the end of "fox": "ick brown fox";
  - past the middle of its 'i', short of the middle of the 'x': "ck brown fo";
  - begun in the margin left of the line: the whole line.
- **`textIsSelectedAndCopied`:** begun beside the text, as before.

Four changes broken on purpose, all caught:

- every character's box asked again (13.6 s);
- the hit one character off;
- the past-the-middle rule dropped (caught once the test pressed and released inside characters);
- nothing tried off the text.

## Runs

- **test_pdf_viewer offscreen:** 24 of 24 (its GPU comparison skipped there).
- **On the display:** the selection tests pass.
- **Under the sanitizers:** the selection tests, no report (53 ms).
- **The lints pass.**
