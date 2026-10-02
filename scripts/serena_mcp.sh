#!/bin/bash
# MCP server entry point for Serena (registered in .mcp.json).
#
# Runs Serena for the checkout Claude Code was started in: --project-from-cwd
# resolves the nearest .serena/project.yml or .git, so each git worktree is
# its own project. When Serena is not installed it serves an empty MCP server
# instead, so the session starts without a failed-server warning.

# uv installs tools to ~/.local/bin, which a non-login PATH may lack.
PATH="$PATH:$HOME/.local/bin"

if command -v serena >/dev/null 2>&1; then
  exec serena start-mcp-server --context claude-code --project-from-cwd \
    --open-web-dashboard false
fi

# Minimal stdio MCP server with no tools: answers the handshake and tools/list,
# ignores notifications, rejects anything else.
exec python3 -c '
import json, sys
for line in sys.stdin:
    try:
        msg = json.loads(line)
    except ValueError:
        continue
    if "id" not in msg:
        continue
    method = msg.get("method")
    if method == "initialize":
        version = msg.get("params", {}).get("protocolVersion", "2025-06-18")
        result = {"protocolVersion": version, "capabilities": {"tools": {}},
                  "serverInfo": {"name": "serena (not installed)", "version": "0"}}
        reply = {"result": result}
    elif method == "tools/list":
        reply = {"result": {"tools": []}}
    elif method == "ping":
        reply = {"result": {}}
    else:
        reply = {"error": {"code": -32601, "message": "Serena is not installed"}}
    reply.update(jsonrpc="2.0", id=msg["id"])
    sys.stdout.write(json.dumps(reply) + "\n")
    sys.stdout.flush()
'
