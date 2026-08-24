---
id: 2
title: Lucent rejects compatible non-Clang consumers at configure time
status: resolved
symptom: A C++20 consumer configured with GCC cannot use Lucent because Lucent's CMake exits before compiler feature detection
tags: build,portability,consumer
created: 2026-08-25
updated: 2026-08-25
---

## Root cause


## What was tried / dead ends


## Resolution

### Resolution (2026-08-25)
Lucent now treats Clang as the verification toolchain without rejecting conforming GNU or AppleClang consumers; CMake's C++20 feature requirement remains authoritative.
