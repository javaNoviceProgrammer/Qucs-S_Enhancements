# gdstk in Qucs-S

The C++ library of [gdstk](https://github.com/heitzmann/gdstk), which reads and
writes GDSII and OASIS layouts, for the layout viewer (`qucs/layout.h`,
`qucs/layoutdoc.h`).

- **Version**: 1.0.1, commit `2e468cdab1aa380af559eb7ff4125e1924bbad1f`
  (16 July 2026).
- **Licence**: Boost Software License 1.0 (`LICENSE`), as is the Clipper 6.4.2
  it carries (`external/clipper`). Both may be built into a GPL program.
- **What is here**: `include/`, `src/` and `external/clipper/` of that commit;
  not its Python module, tests, documentation or build files. `CMakeLists.txt`
  is ours: the library alone, static, with zlib.

## What was changed

Every change is marked `Qucs-S` in the source.

**No Qhull.** `convex_hull()` in `src/utils.cpp` used Qhull, whose licence is
its own; it is a monotone chain of our own instead, so Qhull is not linked.

**A damaged file is refused, not a crash.** A viewer opens whatever file it is
given, and gdstk's readers trusted their input. Fuzzing them under
AddressSanitizer and UBSan - every cut of a GDSII and two OASIS files, and
thousands of them with bytes changed at random - found these, each fixed so
the read ends with an error (or what was read before the damage):

- OASIS, `src/oasis.cpp`:
  - `oasis_read()` read past a compressed block's data, and left what a short
    read did not fill as it was: it stops at the block's end and zeroes the
    rest.
  - `oasis_read_string()` wrote its terminating zero through a null pointer
    when a string was cut short: such a string is empty.
  - A count read from the file - a string's length, a point list's, an
    explicit repetition's - was allocated as given: `oasis_count_fits()`
    checks it against the bytes left (each takes one at least).
- OASIS, `read_oas()` in `src/library.cpp`:
  - Names, texts, placed cells and property names and values given by a
    table's number were kept as that number in a pointer until the END
    record, and read and freed as pointers when the file ended before it:
    they are resolved once, each number checked against its table, at END
    or wherever the file ends.
  - An element before any cell; a placement, a text or a property of a
    modal value not set yet; a path of no points after its start; a circle
    of no radius (`ellipse()` asserts it); a table number, or a compressed
    block's sizes, beyond what a file of its size has.
  - The file's size is kept in `OasisStream::file_size`, for those checks.
- GDSII, `read_gds()` in `src/library.cpp`:
  - Each record it uses is read only if it has the data type and length it is
    read as (an XY of two-byte integers was read as four-byte ones, a name
    of no characters at its index -1, more Raith data than its struct).
  - A boundary of no points; a path's XY of its first point only; an array
    placement with fewer than three corners; a placement with no cell name;
    columns or rows of none or fewer (divided by); a structure named twice
    (listed twice, then freed twice).
- `include/gdstk/utils.hpp`: `FTELL64`, beside `FSEEK64`.

`test_layout_doc` reads every cut of its sample files and 400 changed copies
of each, the same each run, in CI's sanitizer build too.

**Built without UBSan's alignment check** (`CMakeLists.txt`): the GDSII
reader reads 8-byte reals 4 bytes into a record, as x86-64 and ARM64 do.

To take a newer version: copy those three folders from its tag over these,
make the same changes again (or check that its own fixes cover them), and run
`test_layout_doc` under the sanitizers.
