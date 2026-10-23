# MCP implementation log

## Scope

- Initial goal: add native Linux MCP to meshtasticd, matching the walien service.
- Scope refined: keep MCP isolated and avoid broad changes to mesh/radio code or TCP API clients. Chosen design is an in-process Linux module with its own loopback HTTP listener; a separate process would need IPC to access daemon state.
- MCP endpoint: `http://127.0.0.1:<General.MCPPort>/mcp`; the Meshtastic protobuf API remains on `4403`. MCP is opt-in; `General.MCPPort: 0` disables it.

## Changes so far (2026-10-23)

- Added `MCP_NATIVE.md` with build, configuration, protocol, tool, and snapshot details.
- Added Linux-only `McpServer.{h,cpp}`, startup wiring, `MCPPort` config parsing/validation, and example config. Requests are queued through `OSThread`; NodeDB reads run on the firmware scheduler.
- All eight walien-style read-only tools are implemented. Hourly `lastHeard` snapshots persist under the Portduino filesystem at `/mcp/lastheard.jsonl` with 30-day pruning. PSKs and node keys are omitted.
- MCP-FIXES A1-A3, B1-B9, C1-C9, and C11 implemented; focused distance/DST tests pass. Native build and simulated protocol/config/snapshot checks pass. A 200-request flood returned 32 served / 168 busy with no transport failures. A 32 MiB history fixture took ~0.07 s for activity / ~0.12 s for comparison at ~21 MiB peak RSS.
- Full firmware suite timed out after 35/88 suites and was aborted; completed suites passed. Prune-write, timeout/retry, and in-flight shutdown tests remain pending; `MCP-FIXES.md` tracks them.
- MCP uses Ulfius/Jansson. Build uses an isolated PlatformIO core at `~/.local/share/pi-tools/platformio-core`; `libjsoncpp-dev` was installed because the existing native firmware build requires it.
- Port `8767` is occupied by the existing MCP sidecar on this host. During transition, stop that service or set `General.MCPPort` to a free port.
