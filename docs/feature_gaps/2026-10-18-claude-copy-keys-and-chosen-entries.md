# Cmd+C in the Claude chat, and the File Browser's Copy of several entries

The user (2026-10-18):
- *"cmd+C doesn't work for copying some text in the claude chat GUI and I have to right click and select "copy"."*
- *"in the file browser it seems like selecting multiple files or folders is possible but when right clicking to select "copy" it will only highlight one of the selected items."*

## The chat: the editing keys in text one reads

**Was.** The conversation in the Claude Code dock is a read-only `QTextBrowser`. Qt claims a text view's editing keys ahead of the window's shortcuts only for editable text (`QWidgetTextControl`'s `ShortcutOverride`: `if (interactionFlags & Qt::TextEditable)`). So in the conversation, Cmd+C was the window's Edit > Copy, which copies the document in front: a schematic's selection, or nothing. Right-click > Copy went to the view itself, which is why it worked.

The same held for every key the Edit menu has, and in every read-only text in the window (a log, a preview, a selectable label):
- **Cmd+A** chose the schematic's parts, not the conversation's text.
- **Cmd+X, Backspace and Delete** cut or deleted the schematic's chosen parts, out of sight.
- **Cmd+V** began pasting into the schematic.

**Now.** A filter on the application (`ReadOnlyTextKeys`, `qucs.cpp`) claims those keys for read-only text with the focus: a read-only `QTextEdit` or `QPlainTextEdit`, or a label whose text can be chosen (`misc::isReadOnlyText`).
- **Copy and Select All** are then the text's own; Qt's text control does both in read-only text.
- **Cut, Paste, Delete and Backspace** do nothing there.

Edit > Copy and Select All chosen from the menu also act on the text with the focus when it has a selection (`slotEditCopy`, `slotSelectAll`). Editable text was already its own and is unchanged.

## The File Browser: a right click on the chosen entries

**Was.** A right click on an entry made it the current one (`setCurrentIndex`), and in a view that chooses several that chooses it alone. The others lost their highlight, and the menu built after was for that entry alone: Copy copied one. Qt's own right press keeps the chosen entries when the click is on one of them; the menu's code undid that.

**Now.**
- **On one of the chosen entries,** a right click moves the current entry there without changing what is chosen. The menu is for all of them: "Copy 2 Items".
- **On an entry not chosen,** it is chosen alone, as before.
- **The menu's other items for several entries:**
  - **"Copy N Paths"** copies a path a line, as the Finder's "Copy as Pathname" does.
  - **"Move N Items to Trash…"** asks once for them all, and an entry inside a folder chosen with it goes with the folder.
  - Rename stays with the entry clicked.

## Tests

- **test_claude_code `theConversationsTextTakesTheKeys` (new).** In the application, with a schematic open and all of it chosen, the conversation holds a reply with part of it chosen, and has the focus.
  - Cmd+C copies the chosen text.
  - Cut, Paste, Backspace and Delete leave the schematic's parts as they were.
  - Cmd+A chooses the whole conversation.
  - Edit > Copy from the menu copies the chosen text.
  - It passes on the display too.
- **test_file_browser_drop `aRightClickKeepsTheChosen` (new).** In the List, Tree and Details views, two entries are chosen and the right mouse button is pressed on one of them, then its menu shown.
  - Both stay chosen, the menu reads "Copy 2 Items", "Copy 2 Paths" and "Move 2 Items to Trash…", and Copy copies both.
  - On an entry not chosen, that entry alone.
  - Copy 2 Paths gives a line each.
  - The trash item asks about both.
  - Three entries with one folder's file inside another are trashed with one question.
  - It passes on the display too.

## Breaks

Seven changes broken on purpose, all caught:
- the key filter not installed (the schematic's parts cut);
- Edit > Copy from the menu copying the document;
- both as before;
- the right click choosing one;
- Copy Path copying one;
- the trash item trashing the one clicked;
- an entry inside a folder chosen trashed after it (the box "could not be moved").
