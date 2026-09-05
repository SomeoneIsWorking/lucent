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
| S010 | Native CMake and CTest verification runs on supported desktop hosts without game assets | partial | S001, S002, S003, S004, S005, S007, S008, S009 | G001 |
| S011 | Linux applications select one local file asynchronously through a native GTK chooser | partial | — | G001 |
| S012 | Linux applications read regular files asynchronously within a strict byte budget | verified | — | G001 |

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

### S010 — Hosted native verification

The workflow configures and builds Lucent with CMake/Ninja, then runs the production CTest suite on
Linux x64 and arm64, Windows x64, and macOS Intel and arm64. The Windows matrix intentionally builds
the core target without the optional zlib-backed ZIP target because the workflow has no pinned native
zlib dependency; Linux and macOS exercise the ZIP target.

Gap: the first hosted run for the workflow is still required before this item can be marked verified.
Android is blocked rather than represented by a fake job: Lucent contains title-neutral Android Java
runtime sources but no standalone Gradle/package target. The shared `android-port` owner supplies the
build/package boundary for consuming ports.

### S011 — Linux native file chooser

Evidence: optional `lucent::file_dialog` builds against GTK3 without adding dependencies to core.
`lucent_file_dialog_tests` drives the real GTK native chooser's fallback window in an isolated Xvfb
display and verifies exact-file selection, native and programmatic cancellation, busy-request refusal,
close suppression, callback reopening/self-destruction, and pending-owner teardown. A separate
missing-display process verifies explicit deferred initialization failure.
Independent X11 connections verify chooser windows are unmapped immediately after completion or
close, without another GTK iteration; native teardown explicitly flushes queued window-system work.

The GTK fork in `dependencies/gtk.json` corrects immediate child visibility at
reveal start and late child insertion without changing animation timing. Its
four lifecycle regressions and twelve existing sizing cases pass; upstream
3.24.52 fails both immediate-reveal and late-insertion regressions. The unchanged
30-event XTest corpus lost all 13 characters with 28 criticals against upstream;
the installed fork preserves all text with zero criticals at the same capped
two-iteration/60 Hz dispatch. `tools/gtk_runtime.py` cold-built the pinned source
with Clang 22.1.8, X11 and Wayland enabled, through the consumer's locked Meson
1.11.1 environment. Fifteen provisioning tests supplement the passing 18-test CTest
gate. A warm invocation without compiler environment overrides retains the configured
Clang toolchain and performs no compilations; explicit changes remain refused.
Consumers must select this prefix; arbitrary system GTK is not covered by
the corrected keyboard-lifecycle evidence.

Gap: the desktop-portal presentation has not been exercised. Linux CI enables the optional target;
its hosted result remains pending. Windows/macOS pickers are unsupported; Android retains its
separate `LucentDocumentImport` SAF owner.

### S012 — Linux bounded asynchronous file reads

Evidence: `lucent_file_read_tests` exercises the production GIO reader with exact-limit 128 KiB
binary content, empty files with zero-byte budgets, oversize and missing inputs, directories/FIFOs,
invalid paths/limits, cancellation, and destroyed owners. A regular procfs file with zero reported
size proves the read-time max+1 refusal independently of metadata-size checks. Errors discard all
partial bytes; cancelled callbacks retain internal state only, never the reader or consumer.
The descriptor-open discriminator swaps a regular path to a FIFO at the syscall boundary and proves
nonblocking refusal plus descriptor closure. Replacing a regular pathname after open also proves
reads retain the original validated descriptor rather than reopen the substituted path.
