# Images dropped into the Claude Code chat, shown to Claude

The request (2026-10-16): "add support for dropping an image into the claude chat box and asking
claude questions about it. Make sure claude can read the image that I drop in the chat for it."

## What it is

**Attaching.** An image dropped on the chat box, or on the conversation above it, is attached to
the next prompt. So is an image pasted with ⌘V: a screenshot copied, an image's data from a page,
or an image file copied in the Finder. Image files are those Qt reads (PNG, JPEG, GIF, WebP, BMP,
TIFF, SVG...). An image's data is kept as a PNG in the cache (claude-images/, named by its
content). Each attached image shows as a small picture above the box (96 by 56 at most, never
enlarged), its path in the tip and a × that takes it off. A prompt takes at most 20 images, and
the same file once. Text pasted is text. So is text that comes with a picture of itself (a
spreadsheet's cells copied). A drop of anything else is as before: text into the box, nothing on
the conversation. A PDF is not taken: Qt's PDF reader would give its first page alone, as a
picture. With an image attached, Send works with no text too.

**Sent.** The user message's content becomes the image blocks, then the text
(claude::promptContent()): `{"type": "image", "source": {"type": "base64", "media_type":
"image/png", "data": ...}}` for each, as the Messages API has them and as Claude Code's
stream-json input takes them. Claude sees the images. The text says their files too, "(2 images
attached, from: /path/plot.png, /path/big.png)", so Claude can read them again or use them.

An image is given as it is when it is PNG, JPEG, GIF or WebP, 3.75 MB at most (5 MB in base64,
the API's limit), no side over 2000 pixels, and upright. Otherwise it is read (a photo turned by
its EXIF orientation), scaled to 2000 pixels at most a side and written as PNG. When that is too
large (a photo, noise) it is written as JPEG at 88, smaller again until it fits
(claude::imageForClaude()).

**In the conversation** a prompt's images stay in its line, under its text. Each is at most 240
by 180, drawn smoothly (SmoothImages, the other change of today). Where a file is gone, its name
is shown with "its file is gone". They are kept with the conversation and come back with it. In
the Markdown export each is an image link to its file, and in the plain text "[image: path]".
Edit puts a prompt's images back in the box with its text, and Cancel restores the box as it was.
A prompt of images alone takes its title from the first one.

**Checked with the real Claude Code** (2.1.285, once, by hand): a PNG with "7 3 9 4" drawn in it
was sent the way the dock sends it, `claude -p --input-format stream-json` with the image block
and "What digits are written in the image?". The answer was 7394.

## Checked

test_claude_code imagesDroppedAreShownToClaude (new):
- imageForClaude: a PNG as its own bytes; 2400 by 1600 noise as a JPEG of 2000 by 1333 within
  the limit; a schematic not an image, and why. promptContent of no images is the text.
- Files dropped on the box: the image taken and the schematic not, no name put in as text, its
  picture shown, Send enabled. The same file again is taken once. Another image dropped on the
  conversation is taken, then taken off. A schematic alone dragged over the conversation is not
  let in. A PDF is not taken.
- A screenshot pasted is kept in the cache as a PNG, its pixels as they were. Text with a picture
  of itself pastes as text, and so does text. The × takes the screenshot off.
- Sent through the recording fake claude: the content is two image blocks (the PNG's exact bytes,
  the noise as a 2000-pixel JPEG) and then the text with the files said. The box and the strip
  are empty after.
- The prompt's line shows the two pictures (the plot at 240, the other within 240 by 180), and
  its text is the words alone. Kept and brought back in another panel, both are shown. Edit puts
  them back in the box, and Cancel clears it.

Thirteen changes broken on purpose. Eleven were caught at once: text before the images, no
downscale, images not kept, an edit without its images, images left after sending, Send disabled
with images alone, the line without its pictures, a screenshot pasted ignored, the files not said,
a drop on the conversation ignored, a PDF taken. Two were caught after the test was made to see them:
- **Any file taken as an image** in a drop. A later check (the file must read) takes the
  schematic off anyway; the difference is that the conversation let a schematic's drag in. The
  test built that drag but never sent it, and now does.
- **Text with a picture of itself pasted as an image.** The test's picture was the screenshot
  already attached, which is taken once, so the paste fell through to text either way. A new
  picture is used now.

test_claude_code 51 of 51; the Release suite 126 of 126; test_claude_code under the sanitizers,
no report.

## Not done, said

- PDFs are not attached as pictures (only their first page would be). Claude Code's own Read
  tool reads a whole PDF at a path the prompt names.
- An image's drop on the dock's other parts (its header, its cards) is not taken: on the box or
  the conversation only.
- A prompt of images alone, edited later, is not placed in Claude Code's session file by its
  text (the dock places prompts by their text), so it cannot be rewound to after the dock is
  reopened.
