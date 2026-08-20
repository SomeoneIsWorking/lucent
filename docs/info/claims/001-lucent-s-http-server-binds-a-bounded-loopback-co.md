---
id: C001
kind: claim
status: holds
created: 2026-08-20
tags: 
depends: src/http.cpp#read_request, src/http.cpp#accept_connections, src/http.cpp#Server::start
---

## Claim

Lucent's HTTP server binds a bounded loopback control channel, rejects malformed and oversized requests before dispatch, and serves connections concurrently

## Evidence

scratch/build-http/lucent_http_tests: POST /echo returned 200 with the parsed method/query/body; invalid Content-Length returned 400 and a 33-byte body over a 32-byte cap returned 413 with the handler-call count unchanged; while /slow was held on a promise, /echo?fast=1 returned 200 before /slow was released.

## What would falsify it

Any valid request fails, malformed or oversized traffic reaches the handler, the listener becomes remotely reachable, or one blocked handler prevents another connection from completing.
