---
id: 5
title: Optional ZIP target forces zlib on core consumers
status: resolved
symptom: An add_subdirectory consumer linking only lucent::lucent must install zlib development headers.
tags: build
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

`lucent_zip` and `find_package(ZLIB REQUIRED)` are unconditional even though ZIP extraction is an
optional subsystem. Any embedded core consumer therefore inherits a zlib prerequisite it does not
use.

## Required fix

Expose ZIP as an explicit optional target, default it on for Lucent's own top-level verification and
off for embedded consumers, and gate its tests and install export on target existence.

### Resolution (2026-08-30)
Made lucent::zip opt-in for embedded consumers while retaining top-level ZIP verification; gated zlib discovery, tests, and install export. Verified an embedded Ubuntu 22.04 Clang 14 build without zlib headers and without an emitted ZIP target.
