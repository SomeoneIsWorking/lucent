# Project goals

## G001 — Shared portable runtime infrastructure

Provide small, cohesive C++20 owners for cross-project runtime concerns so consuming applications do
not duplicate logging, local control transport, user-data paths, content validation, archive safety,
or touch routing. Each owner must remain title-neutral, bounded, portable, and directly testable.
