#if defined(ARCH_PORTDUINO) && defined(PORTDUINO_LINUX_HARDWARE) && __has_include(<ulfius.h>) && __has_include(<jansson.h>)
#include "configuration.h"

#include "FSCommon.h"
#include "McpMath.h"
#include "McpServer.h"
#include "NodeDB.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "platform/portduino/PortduinoGlue.h"
#include <jansson.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
constexpr const char *MCP_PATH = "/mcp";
constexpr const char *MCP_PROTOCOL_VERSION = "2024-11-05";
constexpr const char *SNAPSHOT_FILE = "/mcp/lastheard.jsonl";
constexpr const char *SNAPSHOT_TEMP_FILE = "/mcp/lastheard.tmp";
constexpr uint32_t SNAPSHOT_INTERVAL_MS = 60 * 60 * 1000UL;
constexpr uint32_t SNAPSHOT_RETENTION_DAYS = 30;
constexpr size_t MCP_MAX_REQUEST_BYTES = 64 * 1024;
constexpr size_t MCP_MAX_PENDING_JOBS = 32;
using Json = json_t;

Json *object()
{
    return json_object();
}
Json *array()
{
    return json_array();
}
Json *string(const std::string &value)
{
    return json_string(value.c_str());
}
Json *number(double value)
{
    return json_real(value);
}
Json *integer(json_int_t value)
{
    return json_integer(value);
}
Json *boolean(bool value)
{
    return json_boolean(value);
}

void put(Json *obj, const char *key, Json *value)
{
    json_object_set_new(obj, key, value ? value : json_null());
}
void putString(Json *obj, const char *key, const std::string &value)
{
    put(obj, key, string(value));
}
void putInt(Json *obj, const char *key, json_int_t value)
{
    put(obj, key, integer(value));
}
void putDouble(Json *obj, const char *key, double value)
{
    put(obj, key, number(value));
}
void putBool(Json *obj, const char *key, bool value)
{
    put(obj, key, boolean(value));
}
void putNull(Json *obj, const char *key)
{
    put(obj, key, json_null());
}
void push(Json *list, Json *value)
{
    json_array_append_new(list, value ? value : json_null());
}

std::string dump(Json *value, bool pretty = false)
{
    char *text = json_dumps(value, JSON_ENSURE_ASCII | (pretty ? JSON_INDENT(2) : JSON_COMPACT));
    if (!text)
        return "null";
    std::string out(text);
    free(text);
    return out;
}

Json *property(const char *type)
{
    Json *p = object();
    putString(p, "type", type);
    return p;
}

Json *boundedProperty(const char *type, int minimum, int maximum)
{
    Json *p = property(type);
    putInt(p, "minimum", minimum);
    putInt(p, "maximum", maximum);
    return p;
}

Json *tool(const char *name, const char *description, Json *props = nullptr, Json *required = nullptr)
{
    Json *item = object();
    putString(item, "name", name);
    putString(item, "description", description);
    Json *schema = object();
    putString(schema, "type", "object");
    put(schema, "properties", props ? props : object());
    put(schema, "required", required ? required : array());
    put(item, "inputSchema", schema);
    return item;
}

Json *tools()
{
    Json *list = array();
    push(list, tool("get_status", "Return local node identity, position, metrics, and node counts."));

    Json *props = object();
    Json *recentHours = property("number");
    putInt(recentHours, "minimum", 0);
    put(props, "only_recent_hours", recentHours);
    put(props, "search", property("string"));
    put(props, "limit", boundedProperty("integer", 1, 2000));
    put(props, "include_detail", property("boolean"));
    push(list, tool("get_node_db", "Return known nodes, optionally filtered by recency or name/ID.", props));

    props = object();
    put(props, "node_id", property("string"));
    Json *required = array();
    push(required, string("node_id"));
    push(list, tool("get_node", "Return one node by ID or name substring.", props, required));

    props = object();
    put(props, "lat", boundedProperty("number", -90, 90));
    put(props, "lon", boundedProperty("number", -180, 180));
    put(props, "limit", boundedProperty("integer", 1, 200));
    required = array();
    push(required, string("lat"));
    push(required, string("lon"));
    push(list, tool("get_nearest", "Return positioned nodes sorted by distance from a coordinate.", json_deep_copy(props),
                    json_deep_copy(required)));
    Json *metricsProps = json_deep_copy(props);
    put(metricsProps, "limit", boundedProperty("integer", 1, 100));
    push(list, tool("get_nearest_with_metrics", "Return nearby nodes with battery and TX utilization.", metricsProps, required));

    push(list, tool("get_channels", "Return configured channels without exposing PSKs."));

    props = object();
    put(props, "hours", boundedProperty("integer", 1, (int)SNAPSHOT_RETENTION_DAYS * 24));
    push(list, tool("get_activity_summary", "Summarize hourly node activity from locally stored last-heard snapshots.", props));

    props = object();
    put(props, "day_a", property("string"));
    put(props, "day_b", property("string"));
    put(props, "limit", boundedProperty("integer", 1, 1000));
    required = array();
    push(required, string("day_a"));
    push(list, tool("compare_days", "Compare nodes heard on two local calendar days using stored snapshots.", props, required));
    return list;
}

Json *get(Json *obj, const char *key)
{
    return obj && json_is_object(obj) ? json_object_get(obj, key) : nullptr;
}

std::string asString(Json *value, const std::string &fallback = "")
{
    if (!value)
        return fallback;
    const char *text = json_string_value(value);
    if (!text)
        throw std::invalid_argument("argument must be a string");
    return text;
}

double asDouble(Json *value, double fallback)
{
    if (!value)
        return fallback;
    if (!json_is_number(value))
        throw std::invalid_argument("argument must be a number");
    return json_number_value(value);
}

int asInt(Json *value, int fallback)
{
    if (!value)
        return fallback;
    if (!json_is_integer(value))
        throw std::invalid_argument("argument must be an integer");
    const json_int_t number = json_integer_value(value);
    if (number < INT32_MIN || number > INT32_MAX)
        throw std::invalid_argument("integer argument is out of range");
    return (int)number;
}

bool asBool(Json *value, bool fallback)
{
    if (!value)
        return fallback;
    if (!json_is_boolean(value))
        throw std::invalid_argument("argument must be a boolean");
    return json_is_true(value);
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

Json *nodeView(const meshtastic_NodeInfoLite &node, time_t now, bool detail)
{
    Json *view = object();
    char id[10];
    snprintf(id, sizeof(id), "!%08x", node.num);
    putString(view, "id", id);
    putInt(view, "num", node.num);
    const bool hasUser = nodeInfoLiteHasUser(&node);
    putString(view, "name", hasUser ? (node.long_name[0] ? node.long_name : node.short_name) : "");
    putString(view, "shortName", hasUser ? node.short_name : "");
    putInt(view, "hwModel", hasUser ? (int)node.hw_model : 0);
    putInt(view, "role", hasUser ? (int)node.role : 0);
    putInt(view, "lastHeard", node.last_heard);
    if (node.last_heard)
        putInt(view, "ageSeconds", std::max<time_t>(0, now - node.last_heard));
    else
        putNull(view, "ageSeconds");
    putBool(view, "isFavorite", nodeInfoLiteIsFavorite(&node));
    putDouble(view, "snr", node.snr);

    Json *position = object();
    meshtastic_PositionLite pos = meshtastic_PositionLite_init_zero;
    if (nodeDB->copyNodePosition(node.num, pos)) {
        if (pos.latitude_i)
            putDouble(view, "latitude", pos.latitude_i * 1e-7);
        else
            putNull(view, "latitude");
        if (pos.longitude_i)
            putDouble(view, "longitude", pos.longitude_i * 1e-7);
        else
            putNull(view, "longitude");
        putInt(view, "altitude", pos.altitude);
        putInt(view, "positionTime", pos.time);
        putInt(position, "latitudeI", pos.latitude_i);
        putInt(position, "longitudeI", pos.longitude_i);
        putDouble(position, "latitude", pos.latitude_i * 1e-7);
        putDouble(position, "longitude", pos.longitude_i * 1e-7);
        putInt(position, "altitude", pos.altitude);
        putInt(position, "time", pos.time);
    }

    Json *metrics = object();
    meshtastic_DeviceMetrics dm = meshtastic_DeviceMetrics_init_zero;
    if (nodeDB->copyNodeTelemetry(node.num, dm)) {
        if (dm.has_battery_level) {
            putInt(view, "battery", dm.battery_level);
            putInt(metrics, "batteryLevel", dm.battery_level);
        }
        if (dm.has_voltage)
            putDouble(metrics, "voltage", dm.voltage);
        if (dm.has_air_util_tx) {
            putDouble(view, "airUtilTx", dm.air_util_tx);
            putDouble(metrics, "airUtilTx", dm.air_util_tx);
        }
        if (dm.has_channel_utilization) {
            putDouble(view, "channelUtilization", dm.channel_utilization);
            putDouble(metrics, "channelUtilization", dm.channel_utilization);
        }
        if (dm.has_uptime_seconds) {
            putInt(view, "uptimeSeconds", dm.uptime_seconds);
            putInt(metrics, "uptimeSeconds", dm.uptime_seconds);
        }
    }

    if (detail) {
        Json *user = object();
        putString(user, "id", id);
        putString(user, "longName", hasUser ? node.long_name : "");
        putString(user, "shortName", hasUser ? node.short_name : "");
        putInt(user, "hwModel", hasUser ? (int)node.hw_model : 0);
        putInt(user, "role", hasUser ? (int)node.role : 0);
        put(view, "user", user);
        put(view, "position", position);
        put(view, "deviceMetrics", metrics);
    } else {
        json_decref(position);
        json_decref(metrics);
    }
    return view;
}

std::vector<Json *> allNodes()
{
    std::vector<Json *> result;
    if (!nodeDB)
        return result;
    uint32_t index = 0;
    const time_t now = time(nullptr);
    const meshtastic_NodeInfoLite *node = nullptr;
    while ((node = nodeDB->readNextMeshNode(index)) != nullptr) {
        if (nodeInfoLiteHasUser(node))
            result.push_back(nodeView(*node, now, false));
    }
    return result;
}

bool writeSnapshot()
{
    if (!nodeDB)
        return false;
    if (!FSCom.exists("/mcp") && !FSCom.mkdir("/mcp"))
        return false;

    Json *record = object(), *lastHeard = object();
    const json_int_t now = (json_int_t)time(nullptr);
    uint32_t index = 0;
    const meshtastic_NodeInfoLite *node = nullptr;
    json_int_t count = 0;
    while ((node = nodeDB->readNextMeshNode(index)) != nullptr) {
        if (!nodeInfoLiteHasUser(node))
            continue;
        char id[10];
        snprintf(id, sizeof(id), "!%08x", node->num);
        putInt(lastHeard, id, node->last_heard);
        count++;
    }
    putInt(record, "ts", now);
    putInt(record, "count", count);
    put(record, "lastHeard", lastHeard);
    const std::string line = dump(record);
    json_decref(record);

    File file = FSCom.open(SNAPSHOT_FILE, "a");
    if (!file)
        return false;
    file.print(line.c_str());
    file.print('\n');
    file.close();
    return true;
}

class SnapshotLineReader
{
  public:
    explicit SnapshotLineReader(File &file) : file(file) {}

    bool next(std::string &line)
    {
        line.clear();
        while (true) {
            if (offset == available) {
                available = file.read(buffer.data(), buffer.size());
                offset = 0;
                if (available == 0)
                    return !line.empty();
            }
            const char *begin = reinterpret_cast<const char *>(buffer.data() + offset);
            const size_t remaining = available - offset;
            const char *newline = static_cast<const char *>(memchr(begin, '\n', remaining));
            if (newline) {
                const size_t count = (size_t)(newline - begin);
                line.append(begin, count);
                offset += count + 1;
                return true;
            }
            line.append(begin, remaining);
            offset = available;
        }
    }

  private:
    File &file;
    std::array<uint8_t, 64 * 1024> buffer{};
    size_t offset = 0;
    size_t available = 0;
};

bool parseNumber(const std::string &line, size_t &position, double &value)
{
    while (position < line.size() && std::isspace((unsigned char)line[position]))
        position++;
    if (position >= line.size() || line[position] != ':')
        return false;
    position++;
    while (position < line.size() && std::isspace((unsigned char)line[position]))
        position++;
    const char *begin = line.data() + position;
    const char *limit = line.data() + line.size();
    int64_t integerValue = 0;
    const auto parsed = std::from_chars(begin, limit, integerValue);
    if (parsed.ec == std::errc{} && parsed.ptr != begin &&
        (parsed.ptr == limit || (*parsed.ptr != '.' && *parsed.ptr != 'e' && *parsed.ptr != 'E'))) {
        value = (double)integerValue;
        position = (size_t)(parsed.ptr - line.data());
        return true;
    }
    char *end = nullptr;
    value = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(value))
        return false;
    position = (size_t)(end - line.c_str());
    return true;
}

bool parseSnapshotHeader(const std::string &line, time_t &timestamp, uint32_t &count)
{
    const size_t tsKey = line.find("\"ts\"");
    const size_t countKey = line.find("\"count\"");
    if (tsKey == std::string::npos || countKey == std::string::npos)
        return false;
    size_t tsPosition = tsKey + 4, countPosition = countKey + 7;
    double ts = 0, countValue = 0;
    if (!parseNumber(line, tsPosition, ts) || !parseNumber(line, countPosition, countValue) || countValue < 0 ||
        countValue > UINT32_MAX)
        return false;
    timestamp = (time_t)ts;
    count = (uint32_t)countValue;
    return true;
}

template <typename Callback> void forEachSnapshotHeard(const std::string &line, Callback callback)
{
    const size_t keyPos = line.find("\"lastHeard\"");
    if (keyPos == std::string::npos)
        return;
    size_t position = keyPos + 11;
    while (position < line.size() && std::isspace((unsigned char)line[position]))
        position++;
    if (position >= line.size() || line[position++] != ':')
        return;
    while (position < line.size() && std::isspace((unsigned char)line[position]))
        position++;
    if (position >= line.size() || line[position++] != '{')
        return;

    while (position < line.size()) {
        while (position < line.size() && (std::isspace((unsigned char)line[position]) || line[position] == ','))
            position++;
        if (position >= line.size() || line[position] == '}')
            return;
        if (line[position++] != '"')
            return;
        const size_t idStart = position;
        while (position < line.size() && line[position] != '"') {
            if (line[position] == '\\' || (unsigned char)line[position] < 0x20)
                return;
            position++;
        }
        if (position >= line.size())
            return;
        const std::string id = line.substr(idStart, position - idStart);
        position++;
        double heardAt = 0;
        if (!parseNumber(line, position, heardAt))
            return;
        callback(id, (time_t)heardAt);
    }
}

template <typename Callback> void forEachSnapshotLine(time_t since, Callback callback)
{
    File input = FSCom.open(SNAPSHOT_FILE, FILE_O_READ);
    if (!input)
        return;
    SnapshotLineReader reader(input);
    std::string line;
    while (reader.next(line)) {
        time_t timestamp = 0;
        uint32_t count = 0;
        if (!parseSnapshotHeader(line, timestamp, count) || timestamp < since)
            continue;
        callback(timestamp, count, line);
    }
    input.close();
}

void pruneSnapshots()
{
    File input = FSCom.open(SNAPSHOT_FILE, FILE_O_READ);
    if (!input)
        return;
    File output = FSCom.open(SNAPSHOT_TEMP_FILE, "w");
    if (!output) {
        input.close();
        LOG_ERROR("MCP snapshot prune could not open temporary file");
        return;
    }

    const json_int_t cutoff = (json_int_t)time(nullptr) - SNAPSHOT_RETENTION_DAYS * 86400;
    size_t expectedBytes = 0, writtenBytes = 0, removed = 0;
    bool writeOk = true;
    SnapshotLineReader reader(input);
    std::string line;
    while (reader.next(line)) {
        if (line.empty())
            continue;
        time_t timestamp = 0;
        uint32_t count = 0;
        if (!parseSnapshotHeader(line, timestamp, count) || timestamp < cutoff) {
            removed++;
            continue;
        }

        expectedBytes += line.size() + 1;
        writtenBytes += output.print(line.c_str());
        writtenBytes += output.print('\n');
        if (writtenBytes != expectedBytes) {
            writeOk = false;
            break;
        }
    }
    input.close();
    output.flush();
    output.close();
    if (!writeOk || writtenBytes != expectedBytes) {
        FSCom.remove(SNAPSHOT_TEMP_FILE);
        LOG_ERROR("MCP snapshot prune write failed; keeping original history");
        return;
    }
    if (!FSCom.rename(SNAPSHOT_TEMP_FILE, SNAPSHOT_FILE)) {
        FSCom.remove(SNAPSHOT_TEMP_FILE);
        LOG_ERROR("MCP snapshot prune rename failed; keeping original history");
        return;
    }
    if (removed == 0)
        LOG_DEBUG("MCP snapshot prune retained all history records");
}

json_int_t latestSnapshotTimestamp()
{
    json_int_t latest = 0;
    forEachSnapshotLine(0, [&](time_t timestamp, uint32_t, const std::string &) { latest = (json_int_t)timestamp; });
    return latest;
}

std::string formatLocalTime(time_t timestamp)
{
    struct tm local;
    localtime_r(&timestamp, &local);
    char out[32];
    strftime(out, sizeof(out), "%Y-%m-%d %H:%M", &local);
    return out;
}

Json *status()
{
    Json *out = object();
    if (!nodeDB)
        return out;
    const NodeNum self = nodeDB->getNodeNum();
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(self);
    char id[10];
    snprintf(id, sizeof(id), "!%08x", self);
    putString(out, "id", id);
    putString(out, "name", owner.long_name);
    putString(out, "shortName", owner.short_name);
    if (node && nodeInfoLiteHasUser(node)) {
        putInt(out, "hwModel", (int)node->hw_model);
        putInt(out, "role", (int)node->role);
    }

    meshtastic_PositionLite pos = meshtastic_PositionLite_init_zero;
    if (nodeDB->copyNodePosition(self, pos)) {
        if (pos.latitude_i)
            putDouble(out, "latitude", pos.latitude_i * 1e-7);
        if (pos.longitude_i)
            putDouble(out, "longitude", pos.longitude_i * 1e-7);
        putInt(out, "altitude", pos.altitude);
        putInt(out, "positionTime", pos.time);
    }
    meshtastic_DeviceMetrics dm = meshtastic_DeviceMetrics_init_zero;
    if (nodeDB->copyNodeTelemetry(self, dm)) {
        if (dm.has_battery_level)
            putInt(out, "battery", dm.battery_level);
        if (dm.has_voltage)
            putDouble(out, "voltage", dm.voltage);
        if (dm.has_channel_utilization)
            putDouble(out, "channelUtilization", dm.channel_utilization);
        if (dm.has_air_util_tx)
            putDouble(out, "airUtilTx", dm.air_util_tx);
        if (dm.has_uptime_seconds)
            putInt(out, "uptimeSeconds", dm.uptime_seconds);
    }
    return out;
}

int boundedLimit(Json *args, int fallback, int ceiling)
{
    return std::max(1, std::min(asInt(get(args, "limit"), fallback), ceiling));
}

Json *callTool(Json *params)
{
    const std::string name = asString(get(params, "name"));
    Json *args = get(params, "arguments");

    if (name == "get_status") {
        Json *result = status();
        auto rows = allNodes();
        json_int_t recentCount = 0, positionedCount = 0;
        for (Json *row : rows) {
            Json *age = get(row, "ageSeconds");
            if (json_is_integer(age) && json_integer_value(age) < 86400)
                recentCount++;
            Json *lat = get(row, "latitude");
            if (lat && !json_is_null(lat))
                positionedCount++;
            json_decref(row);
        }
        putInt(result, "nodesKnown", (json_int_t)rows.size());
        putInt(result, "nodesLastHeard24h", recentCount);
        putInt(result, "nodesWithPosition", positionedCount);
        putInt(result, "daemonUptimeSeconds", Time::getUptimeSecs());
        return result;
    }

    if (name == "get_node_db") {
        const double hours = asDouble(get(args, "only_recent_hours"), 24.0);
        if (hours < 0)
            throw std::invalid_argument("only_recent_hours must be nonnegative");
        const std::string search = lower(asString(get(args, "search")));
        const int limit = boundedLimit(args, 500, 2000);
        const bool detail = asBool(get(args, "include_detail"), false);
        auto rows = allNodes();
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](Json *row) {
                                      Json *age = get(row, "ageSeconds");
                                      const std::string haystack =
                                          lower(asString(get(row, "id")) + " " + asString(get(row, "name")) + " " +
                                                asString(get(row, "shortName")));
                                      const bool old =
                                          hours > 0 && (!json_is_integer(age) || json_integer_value(age) >= hours * 3600);
                                      const bool noMatch = !search.empty() && haystack.find(search) == std::string::npos;
                                      if (old || noMatch) {
                                          json_decref(row);
                                          return true;
                                      }
                                      return false;
                                  }),
                   rows.end());
        std::sort(rows.begin(), rows.end(), [](Json *a, Json *b) {
            return json_integer_value(get(a, "lastHeard")) > json_integer_value(get(b, "lastHeard"));
        });
        Json *result = object();
        putInt(result, "totalMatched", (json_int_t)rows.size());
        Json *out = array();
        const size_t count = std::min((size_t)limit, rows.size());
        for (size_t i = 0; i < count; i++) {
            if (detail) {
                const auto *node = nodeDB->getMeshNode((NodeNum)json_integer_value(get(rows[i], "num")));
                if (node)
                    push(out, nodeView(*node, time(nullptr), true));
            } else {
                push(out, json_deep_copy(rows[i]));
            }
        }
        for (Json *row : rows)
            json_decref(row);
        putInt(result, "returned", (json_int_t)json_array_size(out));
        put(result, "nodes", out);
        return result;
    }

    if (name == "get_node") {
        const std::string needle = lower(asString(get(args, "node_id")));
        if (needle.empty())
            throw std::invalid_argument("node_id must not be empty");
        const std::string suffix = needle[0] == '!' ? needle.substr(1) : needle;
        if (suffix.empty() || !nodeDB)
            throw std::invalid_argument("node_id must identify a node");
        const meshtastic_NodeInfoLite *exact = nullptr, *prefix = nullptr, *named = nullptr;
        uint32_t index = 0;
        const time_t now = time(nullptr);
        const meshtastic_NodeInfoLite *node = nullptr;
        while ((node = nodeDB->readNextMeshNode(index)) != nullptr) {
            char id[10];
            snprintf(id, sizeof(id), "!%08x", node->num);
            const std::string fullId = id;
            const std::string idSuffix = fullId.substr(1);
            if (fullId == needle || idSuffix == suffix)
                exact = node;
            else if (!prefix && idSuffix.rfind(suffix, 0) == 0)
                prefix = node;
            if (!named && needle[0] != '!' && nodeInfoLiteHasUser(node)) {
                const std::string names = lower(std::string(node->long_name) + " " + node->short_name);
                if (names.find(needle) != std::string::npos)
                    named = node;
            }
        }
        const auto *match = exact ? exact : prefix ? prefix : named;
        if (match)
            return nodeView(*match, now, true);
        throw std::invalid_argument("node not found in node database");
    }

    if (name == "get_nearest" || name == "get_nearest_with_metrics") {
        Json *latArg = get(args, "lat"), *lonArg = get(args, "lon");
        if (!json_is_number(latArg) || !json_is_number(lonArg))
            throw std::invalid_argument("lat and lon are required numbers");
        const double lat = json_number_value(latArg), lon = json_number_value(lonArg);
        if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180)
            throw std::invalid_argument("lat/lon must be valid geographic coordinates");
        const bool compact = name == "get_nearest_with_metrics";
        const int limit = boundedLimit(args, compact ? 12 : 20, compact ? 100 : 200);
        auto rows = allNodes();
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](Json *row) {
                                      Json *nodeLat = get(row, "latitude"), *nodeLon = get(row, "longitude");
                                      if (!json_is_number(nodeLat) || !json_is_number(nodeLon)) {
                                          json_decref(row);
                                          return true;
                                      }
                                      putDouble(row, "distanceKm",
                                                std::round(meshtastic_mcp::distanceKm(lat, lon, json_number_value(nodeLat),
                                                                                      json_number_value(nodeLon)) *
                                                           100) /
                                                    100);
                                      return false;
                                  }),
                   rows.end());
        std::sort(rows.begin(), rows.end(), [](Json *a, Json *b) {
            return json_number_value(get(a, "distanceKm")) < json_number_value(get(b, "distanceKm"));
        });
        Json *result = object(), *origin = array(), *nodes = array();
        push(origin, number(lat));
        push(origin, number(lon));
        const size_t count = std::min((size_t)limit, rows.size());
        const char *compactKeys[] = {"id",      "name",      "shortName", "distanceKm", "latitude",   "longitude",
                                     "battery", "airUtilTx", "snr",       "lastHeard",  "ageSeconds", "hwModel"};
        for (size_t i = 0; i < count; i++) {
            if (!compact) {
                push(nodes, json_deep_copy(rows[i]));
            } else {
                Json *item = object();
                for (const char *key : compactKeys) {
                    Json *value = get(rows[i], key);
                    if (value)
                        put(item, key, json_deep_copy(value));
                }
                push(nodes, item);
            }
        }
        for (Json *row : rows)
            json_decref(row);
        put(result, "origin", origin);
        put(result, "nodes", nodes);
        return result;
    }

    if (name == "get_channels") {
        Json *result = object(), *channels = array();
        const size_t capacity = sizeof(channelFile.channels) / sizeof(channelFile.channels[0]);
        const int count = std::max(0, std::min<int>(channelFile.channels_count, (int)capacity));
        for (int i = 0; i < count; i++) {
            const meshtastic_Channel &channel = channelFile.channels[i];
            Json *item = object();
            putInt(item, "index", channel.index);
            putInt(item, "role", (int)channel.role);
            if (channel.has_settings) {
                if (channel.settings.name[0])
                    putString(item, "name", channel.settings.name);
                putBool(item, "uplinkEnabled", channel.settings.uplink_enabled);
                putBool(item, "downlinkEnabled", channel.settings.downlink_enabled);
                if (channel.settings.has_module_settings)
                    putInt(item, "positionPrecision", channel.settings.module_settings.position_precision);
            }
            push(channels, item);
        }
        put(result, "channels", channels);
        return result;
    }
    if (name == "get_activity_summary") {
        const int hours = std::max(1, std::min(asInt(get(args, "hours"), 24), (int)SNAPSHOT_RETENTION_DAYS * 24));
        const time_t now = time(nullptr);
        Json *result = object(), *byHour = array();
        json_int_t snapshotCount = 0;
        time_t oldest = std::numeric_limits<time_t>::max(), newest = 0;
        putInt(result, "intervalSeconds", 3600);
        putInt(result, "retentionDays", SNAPSHOT_RETENTION_DAYS);
        forEachSnapshotLine(now - hours * 3600, [&](time_t ts, uint32_t nodesKnown, const std::string &line) {
            oldest = std::min(oldest, ts);
            newest = std::max(newest, ts);
            snapshotCount++;
            json_int_t active = 0;
            forEachSnapshotHeard(line, [&](const std::string &, time_t heardAt) {
                if (heardAt > 0 && heardAt <= ts && heardAt > ts - 3600)
                    active++;
            });
            Json *item = object();
            putString(item, "time", formatLocalTime(ts));
            putInt(item, "ts", ts);
            putInt(item, "activeLastHour", active);
            putInt(item, "nodesKnown", nodesKnown);
            push(byHour, item);
        });
        putInt(result, "snapshotCount", snapshotCount);
        if (snapshotCount) {
            putString(result, "oldest", formatLocalTime(oldest));
            putString(result, "newest", formatLocalTime(newest));
        } else {
            putNull(result, "oldest");
            putNull(result, "newest");
        }
        put(result, "byHour", byHour);
        return result;
    }

    if (name == "compare_days") {
        const std::string dayA = asString(get(args, "day_a"));
        std::string dayB = asString(get(args, "day_b"));
        time_t startA, endA, startB, endB;
        if (!meshtastic_mcp::localDayBounds(dayA, startA, endA))
            throw std::invalid_argument("day_a must be a valid YYYY-MM-DD date");
        if (dayB.empty()) {
            struct tm previous;
            localtime_r(&startA, &previous);
            previous.tm_mday--;
            previous.tm_isdst = -1;
            if (mktime(&previous) == (time_t)-1)
                throw std::invalid_argument("could not determine the previous local day");
            char text[16];
            strftime(text, sizeof(text), "%Y-%m-%d", &previous);
            dayB = text;
        }
        if (!meshtastic_mcp::localDayBounds(dayB, startB, endB))
            throw std::invalid_argument("day_b must be a valid YYYY-MM-DD date");
        const int limit = boundedLimit(args, 200, 1000);
        std::set<std::string> setA, setB;
        forEachSnapshotLine(std::min(startA, startB), [&](time_t snapshotTime, uint32_t, const std::string &line) {
            const bool collectA = snapshotTime >= startA && snapshotTime < endA + 3600;
            const bool collectB = snapshotTime >= startB && snapshotTime < endB + 3600;
            if (!collectA && !collectB)
                return;
            forEachSnapshotHeard(line, [&](const std::string &id, time_t heardAt) {
                if (collectA && heardAt >= startA && heardAt < endA)
                    setA.insert(id);
                if (collectB && heardAt >= startB && heardAt < endB)
                    setB.insert(id);
            });
        });
        std::map<std::string, std::string> names;
        for (Json *row : allNodes()) {
            names[asString(get(row, "id"))] = asString(get(row, "name"), "?");
            json_decref(row);
        }
        std::vector<std::string> onlyA, onlyB;
        size_t inBoth = 0;
        for (const auto &id : setA) {
            if (setB.count(id))
                inBoth++;
            else
                onlyA.push_back(id);
        }
        for (const auto &id : setB)
            if (!setA.count(id))
                onlyB.push_back(id);
        auto nodeList = [&](const std::vector<std::string> &ids) {
            Json *list = array();
            for (size_t i = 0; i < std::min((size_t)limit, ids.size()); i++) {
                Json *item = object();
                putString(item, "id", ids[i]);
                putString(item, "name", names.count(ids[i]) ? names[ids[i]] : "?");
                push(list, item);
            }
            return list;
        };
        Json *result = object();
        putString(result, "day_a", dayA);
        putString(result, "day_b", dayB);
        putInt(result, "count_a", (json_int_t)setA.size());
        putInt(result, "count_b", (json_int_t)setB.size());
        putInt(result, "inBoth", (json_int_t)inBoth);
        putInt(result, "onlyInA", (json_int_t)onlyA.size());
        putInt(result, "onlyInB", (json_int_t)onlyB.size());
        put(result, "onlyInANodes", nodeList(onlyA));
        put(result, "onlyInBNodes", nodeList(onlyB));
        putBool(result, "truncated", onlyA.size() > (size_t)limit || onlyB.size() > (size_t)limit);
        return result;
    }
    throw std::invalid_argument("unknown tool");
}

Json *errorResponse(Json *id, int code, const char *message)
{
    Json *response = object(), *error = object();
    putString(response, "jsonrpc", "2.0");
    put(response, "id", id ? json_deep_copy(id) : json_null());
    putInt(error, "code", code);
    putString(error, "message", message);
    put(response, "error", error);
    return response;
}

Json *handleRpc(Json *request)
{
    Json *id = get(request, "id");
    Json *methodValue = get(request, "method");
    if (!json_is_object(request) || !json_is_string(get(request, "jsonrpc")) || asString(get(request, "jsonrpc")) != "2.0" ||
        !json_is_string(methodValue))
        return errorResponse(id, -32600, "Invalid Request");
    const std::string method = asString(methodValue);
    Json *params = get(request, "params");
    Json *response = object(), *result = object();
    putString(response, "jsonrpc", "2.0");
    put(response, "id", id ? json_deep_copy(id) : json_null());

    if (method == "initialize") {
        putString(result, "protocolVersion", MCP_PROTOCOL_VERSION);
        Json *capabilities = object();
        put(capabilities, "tools", object());
        put(result, "capabilities", capabilities);
        Json *serverInfo = object();
        putString(serverInfo, "name", "Meshtastic Node");
        putString(serverInfo, "version", "1.0.0");
        put(result, "serverInfo", serverInfo);
        putString(result, "instructions", "Read-only access to the local Meshtastic node and its mesh database.");
        put(response, "result", result);
        return response;
    }
    if (method == "ping") {
        put(response, "result", result);
        return response;
    }
    if (method == "tools/list") {
        put(result, "tools", tools());
        put(response, "result", result);
        return response;
    }
    if (method == "tools/call") {
        json_decref(result);
        try {
            Json *value = callTool(params);
            char *text = json_dumps(value, JSON_ENSURE_ASCII | JSON_INDENT(2));
            json_decref(value);
            Json *content = object(), *contents = array(), *toolResult = object();
            putString(content, "type", "text");
            putString(content, "text", text ? text : "null");
            free(text);
            push(contents, content);
            put(toolResult, "content", contents);
            putBool(toolResult, "isError", false);
            put(response, "result", toolResult);
        } catch (const std::exception &e) {
            Json *content = object(), *contents = array(), *toolResult = object();
            putString(content, "type", "text");
            putString(content, "text", e.what());
            push(contents, content);
            put(toolResult, "content", contents);
            putBool(toolResult, "isError", true);
            put(response, "result", toolResult);
        }
        return response;
    }
    json_decref(result);
    json_decref(response);
    return errorResponse(id, -32601, "Method not found");
}

int mcpGetUnsupported(const struct _u_request *, struct _u_response *response, void *)
{
    ulfius_add_header_to_response(response, "Allow", "POST");
    ulfius_set_string_body_response(response, 405, "GET is not supported");
    return U_CALLBACK_COMPLETE;
}

int mcpPost(const struct _u_request *request, struct _u_response *response, void *userData)
{
    ulfius_add_header_to_response(response, "Content-Type", "application/json");
    ulfius_add_header_to_response(response, "Cache-Control", "no-store");
    ulfius_add_header_to_response(response, "MCP-Protocol-Version", MCP_PROTOCOL_VERSION);
    if (!request->binary_body || request->binary_body_length == 0 || request->binary_body_length > MCP_MAX_REQUEST_BYTES) {
        ulfius_set_string_body_response(response, 400, "Invalid MCP request body");
        return U_CALLBACK_COMPLETE;
    }

    json_error_t error;
    Json *rpc = json_loadb((const char *)request->binary_body, request->binary_body_length, JSON_REJECT_DUPLICATES, &error);
    if (!rpc || !json_is_object(rpc)) {
        json_decref(rpc);
        Json *parseError = errorResponse(nullptr, -32700, "Parse error");
        const std::string serialized = dump(parseError);
        json_decref(parseError);
        ulfius_set_string_body_response(response, 400, serialized.c_str());
        return U_CALLBACK_COMPLETE;
    }
    if (!get(rpc, "id")) {
        json_decref(rpc);
        ulfius_set_response_properties(response, U_OPT_STATUS, 202, U_OPT_NONE);
        return U_CALLBACK_COMPLETE;
    }
    auto *server = static_cast<MeshtasticMcpServer *>(userData);
    Json *reply = server ? server->dispatchRequest(rpc) : errorResponse(get(rpc, "id"), -32603, "MCP server unavailable");
    json_decref(rpc);
    const std::string serialized = dump(reply);
    json_decref(reply);
    ulfius_set_string_body_response(response, 200, serialized.c_str());
    return U_CALLBACK_COMPLETE;
}
} // namespace

MeshtasticMcpServer::MeshtasticMcpServer(int port) : concurrency::OSThread("McpServer")
{
    if (port <= 0) {
        disable();
        return;
    }
    bindAddress.sin_family = AF_INET;
    bindAddress.sin_port = htons((uint16_t)port);
    bindAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ulfius_init_instance(&instance, (unsigned int)port, &bindAddress, "meshtastic-mcp") != U_OK) {
        LOG_ERROR("MCP server failed to initialize on port %d", port);
        disable();
        return;
    }
    initialized = true;
    instance.max_post_body_size = MCP_MAX_REQUEST_BYTES;
    const int postEndpointResult = ulfius_add_endpoint_by_val(&instance, "POST", nullptr, MCP_PATH, 0, &mcpPost, this);
    const int getEndpointResult = ulfius_add_endpoint_by_val(&instance, "GET", nullptr, MCP_PATH, 0, &mcpGetUnsupported, this);
    if (postEndpointResult != U_OK || getEndpointResult != U_OK) {
        LOG_ERROR("MCP endpoint registration failed (POST %d, GET %d)", postEndpointResult, getEndpointResult);
        ulfius_clean_instance(&instance);
        initialized = false;
        disable();
        return;
    }
    const int startResult = ulfius_start_framework(&instance);
    if (startResult != U_OK) {
        LOG_ERROR("MCP server failed to start on 127.0.0.1:%d (Ulfius error %d)", port, startResult);
        ulfius_clean_instance(&instance);
        initialized = false;
        disable();
        return;
    }
    LOG_INFO("MCP endpoint listening at http://127.0.0.1:%d%s", port, MCP_PATH);
}

json_t *MeshtasticMcpServer::dispatchRequest(json_t *request)
{
    std::unique_lock<std::mutex> guard(jobsMutex);
    if (shuttingDown)
        return errorResponse(get(request, "id"), -32000, "MCP server is shutting down");
    if (jobs.size() >= MCP_MAX_PENDING_JOBS)
        return errorResponse(get(request, "id"), -32000, "MCP server busy");

    auto job = std::make_shared<Job>();
    job->request = json_deep_copy(request);
    if (!job->request)
        return errorResponse(get(request, "id"), -32603, "Unable to queue MCP request");
    jobs.push_back(job);
    if (!job->completed.wait_for(guard, std::chrono::seconds(10), [&] { return job->done; })) {
        job->abandoned = true;
        auto it = std::find(jobs.begin(), jobs.end(), job);
        if (it != jobs.end())
            jobs.erase(it);
        return errorResponse(get(request, "id"), -32000, "Meshtastic main loop did not process MCP request in time");
    }
    json_t *response = job->response;
    job->response = nullptr;
    return response;
}

int32_t MeshtasticMcpServer::runOnce()
{
    const uint32_t now = Time::getMillis();
    const bool firstSnapshotCheck = !snapshotTimerStarted;
    if (firstSnapshotCheck || Throttle::hasElapsed(lastSnapshotMillis, SNAPSHOT_INTERVAL_MS)) {
        snapshotTimerStarted = true;
        lastSnapshotMillis = now;
        if (firstSnapshotCheck) {
            const json_int_t latest = latestSnapshotTimestamp();
            if (latest > 0)
                lastSnapshotHour = latest / 3600;
        }
        const json_int_t currentHour = (json_int_t)time(nullptr) / 3600;
        if (currentHour != lastSnapshotHour) {
            if (writeSnapshot()) {
                lastSnapshotHour = currentHour;
                if (++snapshotsSincePrune >= 24) {
                    pruneSnapshots();
                    snapshotsSincePrune = 0;
                }
            } else {
                LOG_WARN("MCP last-heard snapshot could not be written");
            }
        }
    }

    std::shared_ptr<Job> job;
    bool abandoned = false;
    {
        std::lock_guard<std::mutex> guard(jobsMutex);
        if (shuttingDown || jobs.empty())
            return 50;
        job = jobs.front();
        jobs.pop_front();
        abandoned = job->abandoned;
        if (!abandoned)
            activeJob = job;
    }
    if (!abandoned) {
        std::lock_guard<std::mutex> guard(jobsMutex);
        abandoned = job->abandoned || shuttingDown;
    }
    if (abandoned) {
        json_decref(job->request);
        job->request = nullptr;
        {
            std::lock_guard<std::mutex> guard(jobsMutex);
            job->done = true;
            if (activeJob == job)
                activeJob.reset();
        }
        job->completed.notify_one();
        return 0;
    }

    json_t *response = handleRpc(job->request);
    json_decref(job->request);
    job->request = nullptr;
    {
        std::lock_guard<std::mutex> guard(jobsMutex);
        if (job->abandoned) {
            json_decref(response);
        } else {
            job->response = response;
            job->done = true;
        }
        if (activeJob == job)
            activeJob.reset();
    }
    job->completed.notify_one();
    return 0;
}

void MeshtasticMcpServer::shutdown()
{
    {
        std::lock_guard<std::mutex> guard(jobsMutex);
        if (shuttingDown)
            return;
        shuttingDown = true;
        for (const auto &job : jobs) {
            job->abandoned = true;
            job->response = errorResponse(get(job->request, "id"), -32000, "MCP server is shutting down");
            job->done = true;
            job->completed.notify_one();
        }
        jobs.clear();
        if (activeJob && !activeJob->done) {
            activeJob->abandoned = true;
            activeJob->response = errorResponse(get(activeJob->request, "id"), -32000, "MCP server is shutting down");
            activeJob->done = true;
            activeJob->completed.notify_one();
        }
    }
    disable();
    if (initialized) {
        ulfius_stop_framework(&instance);
        ulfius_clean_instance(&instance);
        initialized = false;
    }
}

MeshtasticMcpServer::~MeshtasticMcpServer()
{
    shutdown();
}
#endif
