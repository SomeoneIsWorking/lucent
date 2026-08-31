---
id: C001
kind: claim
status: holds
created: 2026-08-20
tags:
depends: src/http.cpp#Server::start
reconfirmed: 2026-08-31
verified_at: 2026-08-31 16:29:16+00:00
---

## Claim

Lucent's HTTP server defaults to a bounded loopback control channel, rejects malformed and oversized
requests before dispatch, and serves connections concurrently. A consumer can explicitly request a
local-network listener for an authenticated, user-visible sharing route.

## Evidence

scratch/build-http/lucent_http_tests: POST /echo returned 200 with the parsed method/query/body; invalid Content-Length returned 400 and a 33-byte body over a 32-byte cap returned 413 with the handler-call count unchanged; while /slow was held on a promise, /echo?fast=1 returned 200 before /slow was released.

## What would falsify it

Any valid request fails, malformed or oversized traffic reaches the handler, a default listener
accepts a connection addressed to a non-loopback interface, an explicitly local-network listener
does not accept one, or one blocked handler prevents another connection from completing.

## Re-confirmed 2026-08-31

2026-08-31 lucent_http_tests used the machine non-loopback IPv4 address: default Loopback refused it, explicit LocalNetwork accepted it, and malformed/body-limit/concurrent dispatch tests passed.

## Re-confirmed 2026-08-31

The complete Ninja/Clang CTest gate passed all 12 tests on 2026-08-31. lucent_http_tests again proved bounded framing, malformed and over-cap requests rejected before dispatch, concurrent requests, default loopback refusal on the non-loopback IPv4 address, explicit LocalNetwork acceptance, and exact 128 KiB streamed file bytes.
