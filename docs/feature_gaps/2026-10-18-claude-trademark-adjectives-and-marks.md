# The vendor's names as adjectives, and marked

The user (2026-10-18), on the two open choices of `2026-10-14-claude-third-party-names-and-file-formats.md`: *"do the possessive rewording (1) and the ® marking (2) first"*.

The owner's Trademark Usage Guidelines (July 2024) ask for a mark as an adjective before a noun, never possessive or plural (§5.1, §5.2), and for ® or ™ at its first use in each text (§4.3).

## 1. Adjectives, not possessives

**Was.** 72 strings the program shows called the products "Lumerical's", and two tests expected two of them:
- the importer's notes: "Lumerical's %2 mobility not here", "Lumerical's materials as the library's";
- the exporter's notes and the comments it writes into a script: "no material of Lumerical's for it", "Lumerical's PML";
- the help of the exports, the semiconductor models and the quantum wells: "Lumerical's max num cb subbands", "as Lumerical's MQW reports it";
- the tools' texts for Claude: "χ from Lumerical's work function";
- a node's label: "Si (Silicon) (Lumerical's models)".

**Now.** Each names the thing:
- "the Lumerical %2 mobility";
- "no Lumerical material for it";
- "a Lumerical PML";
- "In the Lumerical MQW solver: max num cb subbands";
- "as the Lumerical MQW solver reports it";
- "Si (Silicon) (Lumerical models)".

None is left among the strings in the sources or in the README.

**Kept.** The code's comments and the dated write-ups in `docs/` keep their wording. They record what was done and are not shown by the program.

**Held so.** `scripts/ci/check-third-party.py` (in ctest and on CI's Linux job) now fails on a vendor's name as a possessive in two places: a C++ string literal under `qucs/`, comments passed over, or a line of the README.

## 2. Marked at first use

**Which mark is registered.**
- **ANSYS®** is registered: Ansys's own trademark list prints it so.
- **Lumerical, Zemax and OpticStudio** are not on that list, which dates from March 2018, before Ansys bought Lumerical (2020) and Zemax (2021). No registration of them could be confirmed here. Ansys's product pages write "Ansys Lumerical FDTD" and "Ansys Zemax OpticStudio" with no mark.
- ® on a mark that is not registered is a false marking, while ™ may go with any mark one claims. So they take ™.

**Where:**
- **The legend** (the README's Trademarks section and the About box's Trademarks tab, word for word): "Ansys®, Lumerical™, Zemax™ and OpticStudio™, and any and all ANSYS, Inc. brand, …".
- **The menu:** "Import Lumerical™ Project…", its status tip, its help, and the import's dialogs' titles.
- **The data import:** the kind "Lumerical™ data" and its filter "Lumerical™ data (*.ldf)".
- **The Multiphysics panel:** "Lumerical™ scripts (*.lsf)", "Lumerical™ projects (…)" and the materials import's tooltip.
- **Results > Export:** "Lumerical™ MODE (FDE)", "Lumerical™ CHARGE", "Lumerical™ MQW".
- **Each exported script's first line:** "# x.lsf - a Lumerical™ MODE script, written by Qucs-S …".

Not marked: the notes, warnings and help paragraphs that follow those first uses in the same window or file.

## Tests

- **test_build_commit `theTrademarksTab`:** the legend's marked names; the README and the About tab still equal.
- **test_multiphysics_doc `claudeImportsLumerical`:** the menu entry, its tip and its help carry "Lumerical™"; so do the three export kinds' titles.
- **test_fem_lumerical `aModeScript`, test_multiphysics_doc `claudeExportsLumerical`:** the script's first line, marked.
- **test_fem_lumerical:** the importer's reworded note ("the Lumerical Sellmeier coefficients") and label ("Si (Silicon) (Lumerical models)").

## Breaks

Seven changes broken on purpose, all caught:
- the menu unmarked;
- an export kind unmarked;
- the script's header unmarked;
- the About tab unmarked;
- the README unmarked (it no longer equals the About tab);
- a possessive put back in an importer's note (the lint);
- one put in the README (the lint).

A possessive put in a comment is passed over, as it should be.
