---
id: 4
title: Core Lucent APIs require std::format on consumers that do not format
status: resolved
symptom: A consumer that calls only lucent::log fails to compile on Ubuntu 22.04 because log.h unconditionally includes <format>; http.cpp also uses std::format internally even though HTTP framing does not require it.
tags: portability,logging,http,release
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

`log.h` included `<format>` unconditionally because its convenience templates shared the same
header as the raw logger, channels, and sinks. `http.cpp` also used `std::format` for simple response
framing and diagnostics. A consumer therefore inherited the library's optional presentation helper
even when it called only `lucent::log`.

## What was tried / dead ends

- **Move the release to a newer glibc distribution:** changes the symptom into a runtime
  compatibility failure on older Linux systems; it does not fix Lucent's ownership boundary.
- **Bundle another formatter:** unnecessary duplicate policy for consumers that do not format at
  all. Lucent retains its standard formatting API when the standard library provides it.

## Resolution

### Resolution (2026-08-30)
Feature-detected std::format in log.h, kept raw logging available without it, removed std::format from HTTP framing/diagnostics, and added a forced-no-format consumer test. Verified with Clang 22 quality gates and an Ubuntu 22.04 Clang 14/libstdc++ 11 build.
