@AGENTS.md

## Claude Code specifics

- The `serika` MCP server in `.mcp.json` gives you the live editor: approve it once, then use its
  tools to inspect and edit the documents open in the Serika window. The user sees every change;
  prefer `preview_document` over saving files to check your work.
- Project skills in `.claude/skills/` load on demand; reach for `build-and-test` before any build,
  `release` before touching versions or `CHANGELOG.md`, and `add-mcp-tool` when extending MCP.
- Long builds (a full rebuild takes several minutes) belong in the background; never poll them.
