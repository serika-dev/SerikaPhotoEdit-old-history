---
name: add-mcp-tool
description: Add or change a tool on Serika's MCP server (src/mcp/McpServer.cpp), including schema, live-window behaviour, tests and docs. Use when extending what agents can do through MCP.
---

# Adding an MCP tool

1. **Schema** — add a `tool(name, description, properties, required, readOnly, destructive)` entry
   in `McpServer::toolDefinitions()`. Keep the list order stable (clients cache it). Use the shared
   `doc`, `layer`, `layers`, `engine`, `number`, `integer()` helpers; `validate()` enforces types,
   ranges and unknown arguments before your code runs. Window-only tools are appended under
   `if (live)`.
2. **Behaviour** — handle it in `McpServer::call()` after the document lookup. Rules:
   - Mutate only through `Document::mutate`, a transaction, or `LayerOperations` so it is one undo
     step; honour `editLock()` for locked layers and groups.
   - Resolve layers by stable ID (`indexForId`), never by index from the client.
   - File paths go through `checkedPath()` (and `readImageInput()` for images): no escaping the
     workspace, no SVG, size limits.
   - Return `textResult(describe(id, document))` (plus extra fields) or `failure("…")` with a
     message an agent can act on.
   - Anything slow must keep the window responsive in live sessions (see `segmentSubject()` and the
     `processEvents` in `run_actions`); never show dialogs.
3. **Tests** — add a slot to `tests/mcp_tests.cpp` using `call()`/`data()`/`failed()` (legacy) or
   `modernCall()` (2026-07-28). Cover success, a rejected input and the undo step. For window
   behaviour, extend `liveBridgeEditsWindowDocuments`.
4. **Docs** — add the row to the Tools table in `docs/MCP-SERVER.md`; new `run_actions` commands
   also go in `McpServer::actionCatalog()` (its examples are executed by `actionCatalogExamplesRun`).
5. **Changelog** — an `Added` line under `[Unreleased]`.
