# Native Linux MCP server

## Design

The native Linux `meshtasticd` build can host MCP in-process, without opening a second Meshtastic protobuf API connection. The MCP implementation is isolated in `src/mesh/raspihttp/McpServer.{h,cpp}`. It owns a separate Ulfius HTTP listener; request work is queued to the firmware `OSThread` scheduler so MCP reads use the daemon's in-memory state without changing the radio, router, or mesh packet paths.

The existing Meshtastic protobuf API remains on its configured TCP port (default `4403`). MCP uses Streamable HTTP at `/mcp`, binds to `127.0.0.1`, and exposes read-only tools. It is opt-in: stock native deployments do not start the listener. No MCP operation sends packets or changes node configuration. Loopback limits network exposure but does not isolate the endpoint from other local users.

## Build and configuration

MCP is compiled only for native Linux (`ARCH_PORTDUINO` + `PORTDUINO_LINUX_HARDWARE`) when both Ulfius and Jansson headers are available. Install their development packages to enable it. If either header is absent, the daemon logs that MCP is unavailable.

```yaml
General:
  MCPPort: 8767 # enables the listener; 0 disables it (the default)
```

`MCPPort` must be `0` or in the range `1024..65535`; `meshtasticd --check` validates it. The listener reports a startup error if the port cannot be bound. If the existing Python MCP sidecar is still listening on the chosen port, stop it or choose another free port before starting the integrated listener.

Build the native daemon with:

```sh
pio run -e native
```

The current native build also requires the repository's existing JsonCpp development headers (`libjsoncpp-dev` on Debian/Ubuntu); MCP itself uses Jansson through Ulfius.

## MCP protocol and tools

The endpoint accepts one MCP JSON-RPC request per POST at `/mcp` (batches are unsupported). It implements `initialize`, `ping`, `tools/list`, `tools/call`, and notifications, using protocol version `2024-11-05`; `initialize` includes server instructions. Unsupported GET requests return HTTP 405. Tool results are returned as JSON text content. Pending tool jobs are capped at 32 and request bodies at 64 KiB; overloaded requests receive a JSON-RPC server-busy error.

| Tool                       | Arguments                                                | Result                                                                           |
| -------------------------- | -------------------------------------------------------- | -------------------------------------------------------------------------------- |
| `get_status`               | none                                                     | Local identity, position, device metrics, node counts, and `daemonUptimeSeconds` |
| `get_node_db`              | `only_recent_hours`, `search`, `limit`, `include_detail` | Filtered/sorted node list                                                        |
| `get_node`                 | `node_id`                                                | One node by ID or name substring                                                 |
| `get_nearest`              | `lat`, `lon`, `limit`                                    | Positioned nodes sorted by distance                                              |
| `get_nearest_with_metrics` | `lat`, `lon`, `limit`                                    | Nearest nodes with battery/TX metrics                                            |
| `get_channels`             | none                                                     | Channel index/name/role and settings                                             |
| `get_activity_summary`     | `hours`                                                  | Hourly activity derived from local snapshots                                     |
| `compare_days`             | `day_a`, optional `day_b`, `limit`                       | Active-node set comparison for local calendar days                               |

Channel names and settings are returned, but PSKs are not. Public keys and MAC addresses are omitted. Node position/telemetry satellite data is read through NodeDB copy-out accessors.

## Snapshot history

When enabled, the MCP scheduler writes a `lastHeard` snapshot at startup unless the latest stored snapshot is already in the current hour, then writes at most hourly to `/mcp/lastheard.jsonl` in the Portduino filesystem. Pruning retains 30 days. The history tools stream the JSONL file; history before collection began is unavailable. `compare_days` uses DST-aware local calendar-day bounds, with `day_b` defaulting to the previous day. It also accepts snapshots for up to one hour after a day boundary to catch the next hourly sample. Snapshots may contain node IDs and last-heard timestamps.

## Verification status

`pio run -e native` passes. Simulated checks cover initialize/list/calls, notifications, GET 405, legacy-float snapshots, configuration merge/type/range handling, same-hour restart deduplication, and a 200-request flood (32 served, 168 busy, no transport failures). A 32 MiB/720-snapshot fixture processed in about 0.07 s (`get_activity_summary`) and 0.12 s (`compare_days`), with daemon peak RSS about 21 MiB. The four distance/DST unit tests pass. The full firmware suite timed out after 35 of 88 suites; completed suites were passing. Normal process exit invokes MCP shutdown; SIGTERM/SIGINT use default process termination. See `MCP_IMPLEMENTATION_LOG.md` for the concise scope/change log.
