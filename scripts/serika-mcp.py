#!/usr/bin/env python3
"""Launch Serika's MCP server for an MCP client such as Claude Code (see .mcp.json).

By default the session is *live*: it attaches to the running Serika window (starting the freshly
built app if needed) so the agent edits the open documents and you watch every change. Set
SERIKA_MCP_MODE=isolated for private, invisible documents instead.

Environment:
  SERIKA_EXE        path to SerikaPhotoEdit (default: this checkout's build output)
  SERIKA_MCP_ROOT   folder the agent may read and write (default: <repo>/.serika-workspace)
  SERIKA_MCP_MODE   attach (default) or isolated
stdio is passed straight through; diagnostics go to stderr only.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def executable():
    if os.environ.get("SERIKA_EXE"):
        return Path(os.environ["SERIKA_EXE"])
    for candidate in ("build/SerikaPhotoEdit.exe", "build/SerikaPhotoEdit",
                      "build/SerikaPhotoEdit.app/Contents/MacOS/SerikaPhotoEdit"):
        if (ROOT / candidate).is_file():
            return ROOT / candidate
    sys.exit("serika-mcp: build Serika first (cmake --build build) or set SERIKA_EXE.")


def main():
    exe = executable()
    workspace = Path(os.environ.get("SERIKA_MCP_ROOT", ROOT / ".serika-workspace"))
    workspace.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    # A development build finds Qt and its compiler runtime in the repository's ignored .tools folder.
    tools = list((ROOT / ".tools/Qt").glob("*/mingw_64/bin"))
    tools += list((ROOT / ".tools/Qt/Tools").glob("mingw*/bin"))
    tools += [ROOT / ".tools/deps/install/bin"]
    env["PATH"] = os.pathsep.join([str(p) for p in tools if p.is_dir()] + [env.get("PATH", "")])
    mode = os.environ.get("SERIKA_MCP_MODE", "attach").lower()
    args = [str(exe), "--mcp", "--mcp-root", str(workspace)]
    if mode == "attach":
        args.insert(2, "--attach")
    # Pass stdio explicitly: Serika is a GUI-subsystem program on Windows and otherwise gets none.
    return subprocess.run(args, env=env, stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr).returncode


if __name__ == "__main__":
    sys.exit(main())
