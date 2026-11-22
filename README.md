# Qucs-S Enhancements
**Owner: Dr. Meisam Bahadori**

[![CI](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/ci.yml/badge.svg)](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/ci.yml)
[![Release](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/release.yml/badge.svg)](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/release.yml)

A hardened and extended build of [Qucs-S](https://github.com/ra3xdh/qucs_s),
the Qt circuit simulator front-end for ngspice, Xyce and SPICE OPUS. It
starts from the upstream 26.1.1 source, fixes the crash classes that source
had, and adds workflow features of its own — with a headless test
suite, sanitizer builds and fuzzers behind every change. The work is done
with Claude Code, reviewed and driven by a human.

- [ARCHITECTURE.md](ARCHITECTURE.md) — how the upstream code base is put together
- [ENHANCEMENT_PROPOSAL.md](ENHANCEMENT_PROPOSAL.md) — crash root causes, the plan, and what has been done
- `qucs-s-26.1.1/` — the vendored upstream source (ra3xdh/qucs_s 26.1.1) with the changes applied

## License

Qucs-S is © its authors and released under the GNU General Public License,
version 2 or (at your option) any later version — see
[`qucs-s-26.1.1/COPYING`](qucs-s-26.1.1/COPYING). The changes in this
repository are released under the GNU General Public License, version 3
([`LICENSE`](LICENSE)), as the upstream "or any later version" terms allow;
the combined work is therefore distributed under GPL-3.0.

All the code added to Qucs-S as part of this enhancement project is owned
and copyrighted by Dr. Meisam Bahadori.
