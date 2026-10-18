# Feature gaps: third parties' names and file formats

*14 October 2026 — on `faaf93d`. The review: `2026-10-14-claude-third-party-names-and-file-formats.md`,
14 October. Like the review, this is not legal advice: it says what was done and what was
checked, so that counsel can answer the review's open questions.*

The review asked whether naming Ansys Lumerical (and Zemax OpticStudio) throughout the
repository, reading and writing the Lumerical file formats, and comparing results with
Ansys's published examples creates legal risk. It set out steps for trademarks, copyright
and interoperability, and left five questions open for counsel.

The steps are done, and the facts they depend on were checked against the repository, its
481 commit messages, and the 564 files downloaded from Ansys that are kept outside it.

## The review's steps

| Review | Now |
|---|---|
| §1 A notice in the README and the About box: the names are ANSYS, Inc.'s trademarks; no affiliation, sponsorship or endorsement | **Done.** The README has a Trademarks section, and the About box a Trademarks tab, in the same words (a test holds them so). They take the review's sentence in the form of the owner's own legend, from its Trademark Usage Guidelines (July 2024, §5.1): the names are registered trademarks or trademarks of ANSYS, Inc. or its subsidiaries in the United States or other countries, and other names are their owners'. They name Synopsys, Inc. too: Ansys's site is now at ansys.synopsys.com, its footer naming both companies. And they say why the names appear: which files the program reads and writes, and which published results it is compared with. |
| §1 Vendor names kept out of package and repo names | **Checked.** None in the repository's name, the app bundle, the DMG, the AppImages, the Windows installers and zips, or the release's name. The names inside the code (`fem_lumerical.cpp`, `test_fem_lumerical`, the export node types `lumericalmode`...) are not products. |
| §1 No wording that suggests affiliation, certification or endorsement; no logos or screenshots | **Checked.** No "official", "certified", "approved", "endorsed" or "compatible" next to the names in the code, the docs or the release notes; the menus say "Import Lumerical Project...", the exports "Lumerical MODE (FDE)". No vendor logo or figure is in the repository: the images this fork added are its bug hunts' plots and a heat sink drawing. |
| §1 Comparisons accurate and fairly stated | The write-ups give both values each time and say where they disagree (the APD's tunnelling, 1.4 to 2 times Lumerical's, is left open). One sentence was fixed: the I/O update check called the gate's work function convention a "(bug)" next to Lumerical's convention. The bug was the importer's, and the sentence now says so. |
| §2 Ansys's example projects, scripts and archives never committed | **Checked, and now checked on every build.** None of the repository's 3877 files is identical to any of the 564 files downloaded from Ansys (example projects, scripts, data, articles, the Zemax sample). The seven files of Lumerical's formats in `qucs/tests/data/` are written by our generators (`lumerical_projects.py`, `lumerical_sample.py`). A new check, `scripts/ci/check-third-party.py`, fails when a file of a vendor's project, script or data format is not one of those seven, when a generator no longer writes its file, or when a text file carries a vendor's copyright line. It runs in ctest (`check_third_party`) and in CI's Linux job before the build. |
| §2 Article text and figures not published | **Checked.** No run of 10 words is shared between the repository's text or commit messages and the 11 article and page texts downloaded. No figure is in the repository. |
| §2 Physical parameters: cite the original papers; copying the material database verbatim is less clear | `fem_materials.cpp` begins with its sources: the paper of each formula and figure set (Vurgaftman's band parameters, Varshni, Caughey and Thomas, Masetti, Slotboom, Klaassen, Fossum, Hurkx, van Overstraeten and de Man, Selberherr, Palankovski, Robertson, and each index's author). It also lists what was read from the Lumerical material database, which cites no sources, so that counsel's answer can be applied: the model sets' figures of the semiconductors but silicon, the II-VIs' masses and χ, the gate dielectrics' and AlP's heat, SiGe's X gap bowing and alloy scattering, and the temperature exponents of silicon's models. The database file is not in the repository: its figures are, in a table of ours. |
| §2 Our importer and exporter free of Ansys code and script text | **Checked.** The exporter wrote the 17 Lumerical scripts the examples allow (5 MODE, 7 CHARGE, 5 MQW). Compared with the 30 Ansys scripts downloaded, neither they nor the importer and exporter sources share a line of 30 characters or more, or a run of 12 tokens. At 18 characters and 8 tokens one line is shared, a loop header (`for (i = 1:length(fields)) {`). Two downloaded scripts that shared whole comments turned out to be Qucs-S's own exports, which the review saved beside the examples. |
| §3 The terms of the website and of the example downloads | **Read; for counsel.** Each download has a licence file saying its content is Ansys's and its use is subject to Ansys's Terms of Service, which it links. That link is now an index at ansys.synopsys.com. Its User Terms of Use (effective 6 October 2023) are the ones for the site's content: their §5 forbids reverse engineering the sites and content, derivative works and commercial use; documents are allowed for informational, non-commercial or personal use; Pennsylvania law governs (§11). The repository holds none of the downloaded files. They were used, in a private workspace, to work out the formats and as benchmarks: that is where the review's question lies. |
| §3 Anyone holding an Ansys licence kept out of the importer's development | Nothing here can check this. The Lumerical software was not installed or run in this work: the formats were read from the downloaded data files and published scripts. |
| §3 The export copies no Ansys example script | **Checked** (above). |

## For counsel, beside the review's open questions

- **The owner's guidelines ask more than the review's referential use.** The Trademark
  Usage Guidelines ask for written permission before any use of the marks (§4.2, §5.2),
  while allowing referential use of the word marks that portrays the relationship
  accurately (§4.1). They also ask:
  - for the marks as adjectives before a noun, never possessive or plural (§5.1, §5.2);
  - for ® or ™ at their first use in each text (§4.3);
  - for no mark in a product's, account's or repository's name.

  The program's messages, help and tool texts write "Lumerical's" about 60 times ("Lumerical's
  MQW solver", "Lumerical's database"). Whether to follow these guidelines there is a
  decision to make; it was not made here. (Made on 18 October: adjectives, not possessives,
  and the marks at first use - `2026-10-18-claude-trademark-adjectives-and-marks.md`.)
- **The User Terms of Use §5** (reverse engineering of content, derivative works) and the
  use of the downloaded examples to work out the formats and to benchmark.
- **The material parameters read from the Lumerical database** (listed above): whether to
  keep them or take each from a publication.

## Tests

- **`test_build_commit` `theTrademarksTab`:** the About box's Trademarks tab holds the notice
  (the names, ANSYS, Inc.'s, no affiliation with ANSYS, Inc. or Synopsys, Inc., others'
  names their owners'), word for word as the README's Trademarks section.
- **`check_third_party`** (ctest, and CI's Linux job): `scripts/ci/check-third-party.py`.

## Breaks

Eight, all caught:

| Of | Breaks |
|---|---|
| the About box | its Trademarks tab not added; another title; Synopsys left out |
| the README | its notice a word apart from the About box's |
| the check | a fixture not listed as ours; its generator no longer writing it; a vendor's copyright line in a tracked file; a vendor's script added to the repository |

## Builds

The full ctest (121 tests, `check_third_party` new) passes; `test_build_commit` under the
sanitizers too.
