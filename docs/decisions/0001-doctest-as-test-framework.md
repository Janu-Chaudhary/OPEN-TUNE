# 0001 — doctest as the test framework
**Date:** 2026-09-04 · **Resolves:** part of T0.1 · **Status:** accepted

## Context
Constitution VI requires a failing test before every implementation, so a test framework
was the project's first dependency decision. Constitution I restricts us to permissive
licences, which rules out much of the C++ tooling ecosystem before we compare features.

## Options
- **Catch2** — the most widely used C++ test framework, BSL-1.0. Lost because v3 is no
  longer header-only; it needs its own CMake build, which is friction for a project whose
  whole point is being buildable in five minutes.
- **GoogleTest** — BSD-3, powerful, mature. Lost on weight: it brings a build system, a
  mocking framework we do not need, and a heavier compile than a header does.
- **doctest** — MIT, single header, the fastest to compile of the three. Won.

## Decision
doctest v2.4.11, vendored unmodified to `third_party/doctest/` with its licence.

## Consequences
- Adding a test file costs one `#include` and one line in `tests/CMakeLists.txt`.
- `main()` lives in exactly one place (`tests/main.cpp`), so test files stay trivial.
- We give up Catch2's richer matchers and GoogleTest's mocking. Neither matters for DSP
  testing, where assertions are numeric comparisons against synthetic known-answer input.
- If we ever need mocking, that is a signal the design has grown untestable seams — worth
  treating as a design smell rather than reaching for a framework.
