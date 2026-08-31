# Project state

## Current focus

S007 is the current focus.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S001 | Typed configuration and one channel-based logger provide portable process diagnostics | verified | — | G001 |
| S002 | Applications resolve and create private OS-native user-data directories | verified | — | G001 |
| S003 | A bounded local HTTP server owns request parsing, framing, concurrency, scope, and lifecycle | verified | — | G001 |
| S004 | Streaming content identity and bounded ZIP discovery/extraction support safe player-owned imports | verified | S002 | G001 |
| S005 | Platform-neutral multi-touch routing owns contact capture, zones, and cancellation | verified | — | G001 |
| S006 | Android helpers stage user-selected content through private storage and persisted grants | partial | S002, S004 | G001 |
| S007 | HTTP responses stream bounded files without loading complete media into memory | partial | S003 | G001 |

## Capability details

### S001 — Configuration and logging

Evidence: production config, log, and channel APIs have focused portability, sink, formatting, and
environment-name tests in the normal C++ suite.

### S002 — User-data paths

Evidence: `platform` resolves per-application storage using native platform conventions and tests
private-directory creation and portability behavior.

### S003 — Local HTTP control transport

Evidence: the server implements bounded loopback and explicit opt-in local-network scope, request
parsing, response framing, concurrent dispatch, and lifecycle in one shared owner with production tests.

### S004 — Content and ZIP safety

Evidence: production content and ZIP APIs have controls for digests, mapped archives, traversal,
entry and size bounds, exact candidate selection, staging, promotion, and rejection cleanup.

### S005 — Touch routing

Evidence: the title-neutral router and its tests cover zones, multi-touch capture, motion, release,
and cancellation while leaving platform events and game actions to consumers.

### S006 — Android content staging

The Android platform seam owns app-private roots, persisted Storage Access Framework grants, bounded
staging, and cleanup while consumers retain title identity and install policy.

Gap: the Android-only implementation is covered by source-contract tests on this host but has not been
compiled and exercised through Lucent's own Android target.

### S007 — Streaming file responses

The current worktree adds bounded streamed file responses to the shared HTTP owner and focused tests for
ranges, framing, and lifecycle behavior.

Gap: the combined repository quality gate has not yet landed this in-flight capability.
