# Project state

## Comparison baseline

The baseline is each native application independently reimplementing logging, configuration, user-data
paths, local HTTP transport, safe archive import, and touch routing. Lucent provides those
title-neutral runtime capabilities once, with bounded behavior and tested ownership boundaries.

## Current focus

S007 is the current focus.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S001 | One channel-based logger provides configurable portable process diagnostics | verified | — | G001 |
| S002 | Applications resolve and create private OS-native user-data directories | verified | — | G001 |
| S003 | A bounded local HTTP server owns request parsing, framing, concurrency, scope, and lifecycle | verified | — | G001 |
| S004 | Streaming content identity validates player-owned files without loading them completely into memory | verified | S002 | G001 |
| S005 | Platform-neutral multi-touch routing owns contact capture, zones, and cancellation | verified | — | G001 |
| S006 | Android helpers stage user-selected content through private storage and persisted grants | partial | S002, S004 | G001 |
| S007 | HTTP responses stream bounded files without loading complete media into memory | verified | S003 | G001 |
| S008 | Typed configuration reads named application settings from portable process inputs | verified | — | G001 |
| S009 | Bounded ZIP discovery and extraction safely imports exactly one selected payload | verified | S002, S004 | G001 |

## Capability details

### S001 — Logging

Evidence: production log and channel APIs have focused portability, sink, and formatting tests in the
normal C++ suite.

### S002 — User-data paths

Evidence: `platform` resolves per-application storage using native platform conventions and tests
private-directory creation and portability behavior.

### S003 — Local HTTP control transport

Evidence: the server implements bounded loopback and explicit opt-in local-network scope, request
parsing, response framing, concurrent dispatch, and lifecycle in one shared owner with production tests.

### S004 — Content identity

Evidence: production content APIs stream digests and validate measured identity without requiring a
whole-file allocation.

### S005 — Touch routing

Evidence: the title-neutral router and its tests cover zones, multi-touch capture, motion, release,
and cancellation while leaving platform events and game actions to consumers.

### S006 — Android content staging

The Android platform seam owns app-private roots, persisted Storage Access Framework grants, bounded
staging, and cleanup while consumers retain title identity and install policy.

Gap: the Android-only implementation is covered by source-contract tests on this host but has not been
compiled and exercised through Lucent's own Android target.

### S007 — Streaming file responses

Evidence: `lucent::http::Response::file` opens a consumer-authorised file only when the handler
returns it, frames its exact length, and streams it through the existing bounded response path in
64 KiB blocks. `lucent_http_tests` writes a 128 KiB binary with non-text bytes, fetches it through
the shipping server, and compares the complete response body byte-for-byte.

### S008 — Typed configuration

Evidence: the production config API and its tests cover typed values, defaults, and portable
environment-name handling.

### S009 — Safe ZIP imports

Evidence: production ZIP APIs have controls for mapped archives, traversal, entry and size bounds,
exact candidate selection, staging, promotion, and rejection cleanup.
