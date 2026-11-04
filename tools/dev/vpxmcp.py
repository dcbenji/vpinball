#!/usr/bin/env python3
"""Call one tool of a player started with VPX_MCP_PORT and VPX_MCP_TOKEN (see docs/FastIteration.md). Any MCP client works; this is for shells.

  vpxmcp.py vpx_engine_status
  vpxmcp.py vpx_material action=modify name=PF_Playfield patch='{"roughness":0.2}' confirm=true
  vpxmcp.py vpx_view path=/tmp/a.png
Each key=value is one argument, parsed as JSON when it can be. No tool: lists them. Exit code 1 when the tool reports an error.
"""
import json, os, sys, urllib.request

port = os.environ.get("VPX_MCP_PORT", "9077")
token = os.environ.get("VPX_MCP_TOKEN", "")
def rpc(method, params):
    req = urllib.request.Request(f"http://127.0.0.1:{port}/mcp", json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode(),
                                 {"Authorization": f"Bearer {token}", "Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
    return json.load(urllib.request.urlopen(req))
args = sys.argv[1:]
if not args:
    for tool in rpc("tools/list", {})["result"]["tools"]:
        print(tool["name"], "-", tool["description"])
    sys.exit(0)
arguments = {}
for arg in args[1:]:
    key, _, value = arg.partition("=")
    try:
        arguments[key] = json.loads(value)
    except ValueError:
        arguments[key] = value
result = rpc("tools/call", {"name": args[0], "arguments": arguments})["result"]
for item in result["content"]:
    print(item["text"] if item["type"] == "text" else f"[{item['type']}: {len(item['data'])} base64 chars]")
sys.exit(1 if result.get("isError") else 0)
