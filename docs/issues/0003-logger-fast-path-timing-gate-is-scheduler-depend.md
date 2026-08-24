---
id: 3
title: Logger fast-path timing gate is scheduler-dependent
status: resolved
symptom: lucent_tests can fail its string-channel ratio threshold with the same unchanged binary, varying from 20.5x failure to 5.7x pass
tags: test,logging,performance,instrument
created: 2026-08-25
updated: 2026-08-25
---

## Root cause


## What was tried / dead ends


## Resolution

### Resolution (2026-08-25)
Removed the scheduler-dependent ratio verdict while retaining its printed benchmark, and replaced it with a deterministic test that holds Lucent's logger mutex inside a sink and proves channel_on completes independently.
