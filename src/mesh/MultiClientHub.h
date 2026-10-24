#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

// Note: deliberately does NOT include configuration.h / MeshTypes.h — this header
// is reachable from within configuration.h itself (DebugConfiguration.h ->
// SerialConsole.h -> StreamAPI.h -> PhoneAPI.h) where logging macros and the
// pool globals are not yet available. The implementation includes them.
struct _meshtastic_MeshPacket;
struct _meshtastic_QueueStatus;
struct _meshtastic_MqttClientProxyMessage;
struct _meshtastic_ClientNotification;
typedef struct _meshtastic_MeshPacket meshtastic_MeshPacket;
typedef struct _meshtastic_QueueStatus meshtastic_QueueStatus;
typedef struct _meshtastic_MqttClientProxyMessage meshtastic_MqttClientProxyMessage;
typedef struct _meshtastic_ClientNotification meshtastic_ClientNotification;

/**
 * MultiClientHub — server-side hub for serving multiple concurrent API clients
 * (our fork feature; stock firmware only allows a single TCP API client and
 * force-closes the previous connection when a new one appears).
 *
 * Wire protocol framing is unchanged (0x94 0xC3 + 2-byte BE length). The hub
 * provides the semantics that external proxies (e.g. meshtastic-multiplexer)
 * had to implement around the stock daemon:
 *
 * - Live upstream FromRadio traffic (received packets, positions, telemetry,
 *   node-info updates) is fanned out to every connected client.
 * - Each client's outgoing MeshPacket id is remapped to a server-owned id so
 *   concurrent clients cannot collide; matching responses (routing ACK/NAK,
 *   queue status, client notifications) are delivered only to the originating
 *   client with its original id restored.
 *
 * Everything runs on the single cooperative firmware loop, so no locks are
 * needed.
 */

/// A client endpoint that accepts per-client FromRadio delivery.
/// Implemented by PhoneAPI (one instance per TCP connection in our build).
class MCDeliveryTarget
{
  public:
    virtual ~MCDeliveryTarget() {}

    /// Take ownership of p (allocated from packetPool)
    virtual void mcEnqueuePacket(meshtastic_MeshPacket *p) = 0;
    /// Take ownership of q (allocated from queueStatusPool)
    virtual void mcEnqueueQueueStatus(meshtastic_QueueStatus *q) = 0;
    /// Take ownership of m (allocated from mqttClientProxyMessagePool)
    virtual void mcEnqueueMqttClientProxyMessage(meshtastic_MqttClientProxyMessage *m) = 0;
    /// Take ownership of n (allocated from clientNotificationPool)
    virtual void mcEnqueueClientNotification(meshtastic_ClientNotification *n) = 0;
};

class MultiClientHub
{
  public:
    MultiClientHub();

    /// True when at least one multi-client endpoint is registered; in that
    /// case MeshService routes all phone-bound traffic through this hub.
    bool active() const { return !clients.empty(); }
    size_t clientCount() const { return clients.size(); }

    void registerClient(MCDeliveryTarget *client);
    /// Idempotent. Also drops this client's packet-id remap entries.
    void unregisterClient(MCDeliveryTarget *client);

    /// Remap a client-originated packet id to a fresh, server-unique id so
    /// concurrent clients cannot produce colliding mesh packet ids.
    /// Returns the server id (0 if clientPacketId is 0).
    uint32_t registerPacket(MCDeliveryTarget *owner, uint32_t clientPacketId);

    /// If serverPacketId is a known remapped id: write the client's original
    /// id into *clientPacketId and return the owning client. Returns nullptr
    /// for unknown ids (and leaves *clientPacketId untouched).
    MCDeliveryTarget *resolvePacket(uint32_t serverPacketId, uint32_t *clientPacketId) const;

    /// Deliver a packet to the right client(s). Takes ownership of p.
    /// - p->decoded.request_id referencing a remapped id  -> owner only, id restored
    /// - p->id itself being a remapped id (our own echo)  -> all clients, id restored
    /// - otherwise (live mesh traffic)                    -> all clients, unchanged
    void deliverPacket(meshtastic_MeshPacket *p);

    /// Fan out a queue status. mesh_packet_id of 0 (generic) fans out to all
    /// clients; a remapped id is owner-routed with the client id restored.
    /// Takes ownership of q.
    void deliverQueueStatus(meshtastic_QueueStatus *q);

    /// Fan out to all clients. Takes ownership of m.
    void deliverMqttClientProxyMessage(meshtastic_MqttClientProxyMessage *m);

    /// reply_id of 0/false (generic) fans out to all clients; a remapped
    /// reply_id is owner-routed with the client id restored. Takes ownership of n.
    void deliverClientNotification(meshtastic_ClientNotification *n);

  private:
    struct Entry {
        MCDeliveryTarget *owner;
        uint32_t clientPacketId;
    };

    std::vector<MCDeliveryTarget *> clients;
    std::unordered_map<uint32_t, Entry> remap;
    // Per-client FIFO of live server ids (MULT-184 F5/F6, replacing a single
    // global FIFO). Per-client eviction means a high-volume client can only
    // evict its own oldest mapping, never another client's live mappings;
    // and dropping a client removes its whole FIFO at once, so the bookkeeping
    // stays bounded across connect/disconnect churn.
    std::unordered_map<MCDeliveryTarget *, std::deque<uint32_t>> clientOrder;
    uint32_t nextServerId = 1; // id 0 is reserved ("unset")

    static constexpr size_t kMaxRemapEntriesPerClient = 2048;
};

extern MultiClientHub multiClientHub;
