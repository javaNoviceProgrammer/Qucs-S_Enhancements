# The program would not start on a Mac before macOS 15.5

The user (2026-10-18): *"Looks like the last release of macos intel build crashes"*, with the report `qucs_macos_intel_crash.txt`.

That Mac, an Intel one on macOS 13.5, stopped the v27.0.1 release at launch, before any of its code ran:

```
Termination Reason:    Namespace DYLD, Code 4 Symbol missing
Symbol not found: __SparseDestroyOpaqueNumeric_Complex_Double
Referenced from: /Applications/qucs-s.app/Contents/MacOS/qucs-s
Expected in:     /System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate
```

## The cause

Since `78951cd` (the multiphysics solver's sparse factorizations on Accelerate, in v27.0.1, not v27.0.0), the program imports 25 of Accelerate's sparse functions.

- **Two kinds of function.** The real Cholesky's functions are macOS 10.13's. Its LU and its complex factorizations are macOS 15.5's, and `fem_sparse.cpp` calls them only where `__builtin_available(macOS 15.5, *)` says they are there.
- **Strong imports regardless.** Apple's headers mark the public functions with their versions, but not the internal `_Sparse…` functions those inline functions call. So a normal link makes each of them a symbol the program needs in order to start, whatever guards its calls.
- **Every Mac before 15.5.** The release is built for macOS 13.0 (`DEPLOYMENT_TARGET` in `release.yml`) with the newest SDK. dyld refuses it on any macOS before 15.5, Intel or Apple Silicon alike.

A probe shows it: a call to `SparseCleanup` on a complex factorization inside `__builtin_available(macOS 15.5, *)`, built for macOS 13.0. Linked with `-framework Accelerate`, `nm -m` shows `(undefined) external __SparseDestroyOpaqueNumeric_Complex_Double`. With `-weak_framework Accelerate`, it shows `(undefined) weak external`.

## The fix

Accelerate is linked weakly (`multiphysics/CMakeLists.txt`: `-weak_framework Accelerate`). The program's load of it is `LC_LOAD_WEAK_DYLIB`, and all 25 of its symbols are weak imports.

- **On a macOS before 15.5,** the 15.5 functions are null and are never called; Eigen's LU does that work, as it does on Linux and Windows.
- **The Cholesky's functions** are there on every macOS the release supports.
- **On macOS 15.5 and later** nothing changes. Here, on macOS 26, test_fem_sparse passes 11 of 11 with Accelerate's LU.

## The test

**`test_macos_accelerate_weak`** is a new CTest entry for macOS builds. It checks that the program loads Accelerate weakly and that every symbol from it is a weak import. With the strong link put back, it fails.

**What was not run here:** the program on a macOS before 15.5. This Mac runs macOS 26, and the check is the reporter's Mac.

## Also

The v27.0.1 tag's notes said Accelerate's Cholesky works "from 15.5". It is 10.13's; the notes are corrected.
