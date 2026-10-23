# MCP fix checklist - in-process native meshtasticd

Review checklist and fix progress. Source review: 4-person panel (final report at
`/home/rkansy/.pi/pi-teammate/reviewers/01a167c2-021d-74e9-8d54-6968e8156da0/final-report-mcp.md`).

**Repo:** `/home/rkansy/firmware-clone` · **Branch:** `develop` · **State:** uncommitted changes
**Status:** A1-A3, B1-B9, C1-C9, and C11 changes implemented. Native build, distance/DST tests, MCP protocol/config/snapshot tests, 200-request flood, and large-history benchmark pass. Timeout-retry, prune-write, and shutdown failure injection remain unverified; C10 remains optional. Full firmware suite timed out after 35/88 suites and was aborted.
**Build:** use `/home/rkansy/.local/share/pi-tools/platformio-env/bin/platformio run -e native`; PlatformIO core is isolated under `~/.local/share/pi-tools/platformio-core`. Original panel findings and acceptance criteria are retained below; headings and verification record show current status.
Key references (behavior, not to be modified lightly):

- `src/concurrency/OSThread.{h,cpp}` + `.pio/libdeps/native/ArduinoThread/Thread.{h,cpp}` - threads are `enabled=true` at construction, registered with `mainController`; `runOnce()` return = ms until next run (0 = again, 50 = idle).
- MCP spec revision **2024-11-05** ("Streamable HTTP"): single `/mcp` endpoint; POST JSON-RPC; notifications (no `id`) → **202 + empty body**; GET optional, else **405**; `Mcp-Session-Id` optional (stateless OK); tool errors → result with `isError: true`; `MCP-Protocol-Version` response header is spec-defined.
- jansson facts (verified on 2.14): `json_dumps` **omits non-finite reals** (key disappears); `json_object_foreach(NULL)` iterates zero times (safe, undocumented); `json_object_set_new`/`json_array_append_new` steal the reference.
- ulfius facts (verified on 2.7.15): unregistered method/path → **404** "Resource not found"; `ulfius_stop_framework` blocks until in-flight callbacks return.

---

## A. Release blockers (must fix)

### A1 = H1 - FIXED + TESTED - Clamp haversine `a`; NaN previously caused silent key loss + sort UB

**Where:** `McpServer.cpp:425-433` (`distanceKm`), consumers at `:543-556` (`get_nearest` sort).
**Why:** rounding pushes `a` to 1.0000000000000002 for near-antipodal coords (measured: 488/10⁶ grid points, max 1.0000000000000002). Then `asin(sqrt(a))` = NaN → (1) jansson drops the `distanceKm` key from the response entirely (verified: `json_dumps` of `{"distanceKm": NaN}` → `{}`), (2) the `std::sort` comparator at `:550-556` stops being a strict weak ordering → UB. A node DB spans the globe, so antipodal lookups are realistic.
**Fix:**

```cpp
a = std::clamp(a, 0.0, 1.0);   // before asin
```

(and/or filter non-finite distances before sorting: `if (!std::isfinite(d)) { drop row; }`).
**Acceptance:** unit test with antipodal coordinates (e.g. 0,0 vs 0,180 and 51.5,0 vs -41.3,174.8) → finite distance in response, sort stable; fuzz lat/lon pairs incl. ±90/±180 corners → no NaN keys in `json_dumps` output.

### A2 = H2 - FIXED + BENCHMARKED - Stream the snapshot JSONL; bound memory on the main loop

**Where:** `McpServer.cpp:329-348` (`readSnapshots` - retains all parsed records), consumers: `get_activity_summary` (`:601-637`), `compare_days` (`:653-726`, reads the file **twice**).
**Why:** `get_activity_summary(hours=720)` / `compare_days` parse the whole 30-day file into RAM inline in the firmware main loop. Measured: 2000 nodes/line (33 MB) → **182 MB peak RSS, ~416 ms**; 5000 nodes (83 MB) → **491 MB, ~1.15 s**; streaming peaks at ~37 MB. All mesh work (RX/telemetry/tx) stalls for the duration on the main loop; on slower boards this approaches the 10 s dispatch timeout and amplifies via A4.
**Fix:** process line-by-line and free each record immediately:

- `get_activity_summary`: one pass, build `byHour` buckets directly (no retained vector; `oldest`/`newest` = first/last seen ts in the pass, not `front()/back()` of a vector).
- `compare_days`: one pass collecting two `std::set<std::string>` (day A / day B) instead of two full-file reads.
- Optionally store node ids as integers in the JSONL (shrinks lines ~2×) - keep the `!xxxxxxxx` string format if you want backward compatibility with existing files.
  **Acceptance:** same 720-line fixtures as the review (2000 and 5000 nodes) → peak RSS ≤ ~50 MB, wall time ≤ ~100 ms on this host; results identical to the current implementation on a fixture where you know the answer.

### A3 = H3 - FIXED + CHECKED - MCPPort: parse only when the key is present; validate the type

**Where:** `PortduinoGlue.cpp:1534` (`portduino_config.mcp_port = (yamlConfig["General"]["MCPPort"]).as<int>(8767);`), `ConfigCheck.cpp:79` (schema) + `valueSpecs()` table (~`:602-606` has `General.APIPort` - no `MCPPort` entry), `ConfigCheck.cpp:1304-1308` (range check).
**Why (two bugs, both reproduced by running the built binary in review):**

1. The line runs for **every** config file that contains a `General` section - the main config _and every config.d file_ (merge at PortduinoGlue.cpp:263-272). A profile with any unrelated `General` key and no `MCPPort` resets `mcp_port` to the fallback 8767 → an explicit `MCPPort: 0` (disable) or custom port in the main config is **silently re-enabled/overridden** (reproduced: main `MCPPort: 1` → 8767 after a profile with `General: MaxNodes: 200`). This is exactly the layout the rpi5 packaging branch ships.
2. Malformed values fall back silently: `MCPPort: abc` → 8767, and `--check` reports **0 errors** because `General.MCPPort` is missing from `valueSpecs()` (the `checkValueType` warning at ConfigCheck.cpp:752-754 literally says "silently replaced by the default").
   **Fix:**

```cpp
// PortduinoGlue.cpp:1534
if (yamlConfig["General"]["MCPPort"].IsDefined())
    portduino_config.mcp_port = yamlConfig["General"]["MCPPort"].as<int>(portduino_config.mcp_port);
```

(keep the struct initializer `PortduinoGlue.h:328` as the single default), **and** add to `valueSpecs()`:

```cpp
{"General.MCPPort", {kInt, false}},
```

**Acceptance:** (a) main `MCPPort: 0` + config.d profile with `General: MaxNodes: 512` → `--output-yaml`/runtime shows MCP disabled; (b) `MCPPort: abc` → `--check` emits a finding (warn or error per the kInt spec) instead of passing.

---

## B. Should fix in the same commit

### B1 = M1 - FIXED - MCP is opt-in; stock deployments do not open the listener

**Where:** `PortduinoGlue.h:328` + `PortduinoGlue.cpp:1534` (default 8767), `main.cpp:1238-1245` (listener created whenever `mcp_port > 0`), `bin/config-dist.yaml:294`.
**Why:** ulfius is a hard dep of every native build, so **every** existing native deployment gains an unauthenticated loopback JSON endpoint (node IDs, names, positions, telemetry) plus hourly VFS writes + daily rewrite with no config present. Loopback ≠ local-user isolation.
**Fix (choose one, document whichever):** default `0` (opt-in - recommended) **or** keep default-ON and state the local-trust boundary + storage growth prominently in `MCP_NATIVE.md` and the config-dist comment.
**Acceptance:** `grep`-able statement in the docs of the chosen policy; if opt-in, a stock config starts the daemon with no MCP listener (verify with `ss -ltnp`).

### B2 = M2 - FIXED + FLOOD-TESTED - Bound the job queue and request body

**Where:** `McpServer.cpp:855-874` (`dispatchRequest`, unbounded `jobs` deque), ctor `:842` (`max_post_body_size`).
**Why:** each in-flight request deep-copies a body up to 1 MiB into a Job (McpServer.h:26); 600 concurrent callbacks observed with no plateau (thread-per-connection, no MHD limit configured). A local process can pin `connections × 1 MiB`.
**Fix:** in `dispatchRequest`, before enqueueing: `if (jobs.size() >= 32) return errorResponse(id, -32000, "MCP server busy");` (pick the cap with the author); optionally pass an MHD connection/thread limit through ulfius options if available in the installed version.
**Acceptance:** sustained-flood script (e.g. 200 concurrent POSTs) → bounded `jobs.size()`, HTTP -32000 errors beyond the cap, RSS bounded.

### B3 = M3 - FIXED - Mark timed-out jobs abandoned and skip queued work

**Where:** `McpServer.cpp:864-870` (timeout path), `:876-905` (`runOnce`).
**Why:** a job popped before a client's 10 s timeout still executes (wasted main-loop work, see A2), and MCP clients commonly retry at 5-10 s → the tool runs twice.
**Fix:** add `bool abandoned = false;` to `Job`; set it in the timeout path (under `jobsMutex`); in `runOnce()`, if `job->abandoned` skip `handleRpc` (set done, notify, drop).
**Acceptance:** force a slow tool (e.g. sleep in a test build) past 10 s, have the client retry → tool executes once per _successful_ dispatch, not once per attempt.

### B4 = M4 - FIXED - Verify prune writes before replacing history

**Where:** `McpServer.cpp:297-327` (`pruneSnapshots`).
**Why:** `File::print()` return values are unchecked; a full disk / VFS error leaves a truncated `lastheard.tmp` which `FSCom.rename` then moves over the good file → total history loss.
**Fix:** after the write loop, check `output.size()` (or compare written bytes vs `keep` total) before `rename`; on mismatch, remove the tmp and keep the old file. Also log when prune removed 0 lines.
**Acceptance:** simulate a failing write (read-only tmp target or tiny tmpfs) → old file intact, no rename, error logged.

### B5 = M5 - FIXED + RESTART-TESTED - Dedupe snapshots within an hour

**Where:** `McpServer.cpp:875-883` (`runOnce` snapshot block), `:601-637` (`get_activity_summary`).
**Why:** first `runOnce()` always writes; restart within the same hour appends a second line → two `byHour` entries for one hour, activity double-counted.
**Fix:** before appending, read the last line's `ts` (or track `hourBucket(lastTs) == hourBucket(now)` → skip); alternatively dedupe by hour in `get_activity_summary` (sum/merge same-hour records).
**Acceptance:** start, stop, restart the daemon within one hour → exactly one line per hour in the JSONL; `get_activity_summary` shows one entry for that hour.

### B6 = M6 - FIXED + ROUND-TRIP-TESTED - Serialize MCPPort in `--output-yaml`

**Where:** `PortduinoGlue.h:~769-783` (the `General` serializer block - emits `APIPort`/`TCPPort`, `ConfigDirectory`, … but never `mcp_port`).
**Why:** export→reload drops an explicit `MCPPort: 0` (or custom port) and restores the default → listener re-enabled after a round-trip (tested in review).
**Fix:** emit `mcp_port` unconditionally (or when `!= 8767`), mirroring how `APIPort` is emitted.
**Acceptance:** `--check`/`--output-yaml` with `MCPPort: 0` → output contains `MCPPort: 0`; reloading that output keeps MCP disabled.

### B7 = M7 - FIXED + BUILT - Guard Jansson and probe its compiler/linker flags

**Where:** `McpServer.h:4`, `McpServer.cpp:1` (guard `__has_include(<ulfius.h>)` only), `main.cpp:111-118,1235-1250` (same guard), `variants/native/portduino/platformio.ini:16` (`--libs` only).
**Why:** `libulfius-dev` Depends: `libjansson-dev` on Debian (verified), but on other hosts ulfius-without-jansson → **hard compile error** instead of the documented "logs that MCP is unavailable"; `--libs`-only is wrong on non-standard include prefixes.
**Fix:** extend every guard with `&& __has_include(<jansson.h>)` (add the matching `#elif`/`LOG_WARN` branch in main.cpp so the "unavailable" path is real), and change the platformio line to `!pkg-config --cflags --libs jansson --silence-errors || :`.
**Acceptance:** build on a host with ulfius but jansson headers removed (or simulate with a broken include path) → build succeeds, MCP compiled out, warning logged at startup; normal Debian build unchanged.

### B8 = M8 - IMPLEMENTED - Explicit shutdown wakes waiting callbacks on normal exit

**Where:** `main.cpp:1240-1244` (`std::atexit`), `McpServer.cpp:907-912` (dtor → `ulfius_stop_framework`).
**Why:** `ulfius_stop_framework` blocks until in-flight callbacks return (measured: 8 s callback → stop at +8 s); a callback waits on `runOnce()`, which no longer runs after `main()` returns → each in-flight request holds exit for its full 10 s timeout. `atexit` doesn't run on SIGTERM/SIGINT (systemd default).
**Fix:** add `MeshtasticMcpServer::shutdown()` that (1) marks all queued jobs abandoned (B3), (2) signals waiters so they unblock immediately, (3) then `ulfius_stop_framework` + `ulfius_clean_instance`; call it from the normal exit path (and keep the atexit as a backstop). Signal path: process death is acceptable, but document it.
**Acceptance:** kill with an in-flight request → exit completes promptly (no ~10 s hang per request) on the `exit()` path.

### B9 = M9 - FIXED + DST-TESTED - Use DST-correct local-day bounds

**Where:** `McpServer.cpp:351-372` (`localDayBounds`, `end = start + 86400` at `:370`), `:659` (`snapshotTime <= end + 7200` heuristic).
**Why:** 23/25-hour local days → boundary off by an hour (counts an hour of the next day / misses the last hour); the `+7200` window attributes the first two hours of the next day to `day_a`.
**Fix:** compute `end` as the _next local midnight_: copy `local`, `tm_mday++`, re-`mktime()` (after `localtime_r` normalization). Tighten or document the snapshot-acceptance window (e.g. `snapshotTime < end + 3600`, and note the heuristic in `MCP_NATIVE.md`).
**Acceptance:** with `TZ=Europe/Berlin` (or America/New_York), compare the spring-forward and fall-back day pairs → hour counts match a hand-computed expectation (e.g. 23 active hours vs 25).

---

## C. Optional (low severity / hygiene)

| #                 | Where                                | Change                                                                                                                                                                                                                                                                      |
| ----------------- | ------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| C1 (L1, fixed)    | `McpServer.cpp:843`                  | Check `ulfius_add_endpoint_by_val()` return; on failure log, `ulfius_clean_instance`, `disable()`, return (currently the framework starts "listening" with no `/mcp` handler → 404, verified).                                                                              |
| C2 (L2, fixed)    | `McpServer.cpp:843`                  | Register a GET endpoint on `/mcp` returning **405** (MCP spec: 405 when GET unsupported; ulfius currently 404s - verified empirically), or document the deviation.                                                                                                          |
| C3 (L3, fixed)    | `McpServer.cpp:510-523` (`get_node`) | Fix match precedence: exact id → id prefix (allow without `!`) → name/shortName substring; currently first DB-order row wins and `node_id: "1234"` never matches `!1234abcd`; nodes without a user profile are invisible to exact-id lookup.                                |
| C4 (L4, fixed)    | `McpServer.cpp:473` vs `:514`        | Unify the search haystack (include `shortName` in `get_node_db` or state the difference); align tool descriptions.                                                                                                                                                          |
| C5 (L5, fixed)    | `McpServer.cpp:262-295, 613-627`     | Snapshot `ts` uses wall clock: guard `oldest`/`newest` against out-of-order lines (track min/max instead of `front()/back()`), and `ts - heard < 3600` counts future-dated `last_heard` as active (use `heard > ts - 3600 && heard <= ts`).                                 |
| C6 (L6, fixed)    | `McpServer.cpp:622, 663`             | Add `json_is_object(lastHeard)` guards around `json_object_foreach` (empirically safe on jansson 2.14 - not a documented guarantee).                                                                                                                                        |
| C7 (L7, fixed)    | `McpServer.cpp:753, 11`              | Remove the unreachable `"notifications/initialized"` branch (id-less requests already 202 at `:812-815`) and the unused `#include <yder.h>`.                                                                                                                                |
| C8 (L8, fixed)    | `McpServer.cpp:74-111` (schemas)     | Tighten/don't-document: add range constraints to `limit` (e.g. `maximum: 2000`), and either type-check arguments or document the silent fallback behavior (wrong type → default).                                                                                           |
| C9 (L9, fixed)    | `MCP_NATIVE.md`                      | Doc wording: state that channel **names** are returned (only PSKs/keys/MACs are omitted); document `day_b` default (previous day); document "single JSON-RPC requests only (no batch)"; mention the `instructions` initialize field.                                        |
| C10 (info)        | `McpServer.cpp` tool paths           | No RAII for JSON objects: all _intentional_ throws precede allocation (verified - no leaks), but a `bad_alloc` mid-tool leaks and escapes into the main loop → `std::terminate`. Consider `std::vector<std::unique_ptr<Json, decltype(&json_decref)>>`. OOM-only; optional. |
| C11 (info, fixed) | `McpServer.cpp`                      | Renamed the status uptime field to `daemonUptimeSeconds` so it is not mistaken for a client connection duration.                                                                                                                                                            |

---

## Verification record

- **PASS:** `pio run -e native`; MCP math/DST tests (4/4); `--check` type/range validation; `--output-yaml` disable/profile round-trip.
- **PASS:** Simulated MCP protocol and tool smoke tests, GET 405, legacy-float snapshots, same-hour restart dedupe, config merge/type/range/round-trip, and 200 concurrent POSTs (32 served, 168 busy, no transport failures).
- **PASS:** 720 × 2000-node legacy snapshot fixture (~32 MiB): activity ~0.07 s, compare ~0.12 s, daemon peak RSS ~21 MiB. Exposure review: no PSKs, public keys, or MACs in tool outputs.
- **PENDING:** timeout/retry race test, injected prune write failure, and in-flight shutdown test.
- **INCOMPLETE:** full firmware suite timed out after 35/88 suites; completed suites passed.

## Out of scope for the fix commit

- The rpi5-rak6421 packaging fixes (round-1 review, branch `multiclient-api-packaging` - separate checklist: `/home/rkansy/rpi5-rak6421-FIXES.md`, still unaddressed on that branch).
- Firmware behavior changes to the node DB / VFS; MCP protocol version upgrades beyond 2024-11-05 conformance.
- Full firmware-suite completion remains separate: the earlier run timed out at 35/88 suites; the new `test_mcp_math` suite passes independently.
