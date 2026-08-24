---
id: C002
kind: claim
status: holds
created: 2026-08-25
tags:
depends: src/log.cpp#timestamp_now, src/log.cpp#log
reconfirmed: 2026-08-25
verified_at: 2026-08-25 01:04:02
---

## Claim

Every Lucent log record sent to an installed sink, a log file, or stderr begins with a millisecond UTC ISO 8601 timestamp

## Evidence

Lucent's six-test CTest suite passed on 2026-08-25; test_lucent validates every captured prefix's complete digit/separator shape, and test_channel_env_name captures a timestamped record emitted before main.

## What would falsify it

Any change to timestamp_now or log output assembly in src/log.cpp, or any sink path that bypasses lucent::log.

## Re-confirmed 2026-08-25

Lucent's six-test Clang CTest suite passed after commit 95b27df; logger tests validate the complete millisecond UTC prefix and pre-main sink path.
