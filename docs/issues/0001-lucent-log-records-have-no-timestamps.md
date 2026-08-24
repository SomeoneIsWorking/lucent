---
id: 1
title: Lucent log records have no timestamps
status: resolved
symptom: Lucent sink, stderr, and file records cannot be correlated in time because the formatted line begins with the channel only
tags: reported,logging,timestamps
created: 2026-08-25
updated: 2026-08-25
---

## Root cause


## What was tried / dead ends


## Resolution

### Resolution (2026-08-25)
Lucent now constructs one millisecond UTC ISO 8601 prefix at log.cpp's final sink boundary, before dispatch to installed sinks, files, or stderr; logger and pre-main environment-name tests verify the timestamped format.
