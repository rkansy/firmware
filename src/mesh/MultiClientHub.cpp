#include "MultiClientHub.h"
#include "MeshService.h"
#include "configuration.h"
#include <algorithm>

MultiClientHub multiClientHub;

MultiClientHub::MultiClientHub() {}

void MultiClientHub::registerClient(MCDeliveryTarget *client)
{
    if (!client)
        return;
    for (auto *existing : clients)
        if (existing == client)
            return; // already registered
    clients.push_back(client);
    LOG_INFO("MultiClient: client registered, %u connected", (unsigned)clients.size());
}

void MultiClientHub::unregisterClient(MCDeliveryTarget *client)
{
    auto it = std::find(clients.begin(), clients.end(), client);
    if (it == clients.end())
        return;
    clients.erase(it);
    // Drop this client's remap entries: its pending responses are gone with it
    for (auto rit = remap.begin(); rit != remap.end();) {
        if (rit->second.owner == client)
            rit = remap.erase(rit);
        else
            ++rit;
    }
    // F5: also drop this client's per-client FIFO so its (now-erased) server
    // ids do not linger as stale bookkeeping across connect/disconnect churn.
    clientOrder.erase(client);
    LOG_INFO("MultiClient: client unregistered, %u connected", (unsigned)clients.size());
}

uint32_t MultiClientHub::registerPacket(MCDeliveryTarget *owner, uint32_t clientPacketId)
{
    if (clientPacketId == 0)
        return 0;

    uint32_t serverId = nextServerId++;
    if (serverId == 0) // 0 means "unset" in the mesh protocol
        serverId = nextServerId++;

    // F6: per-client cap. Evict only this client's own oldest mapping, so a
    // high-volume client can never evict another client's live mappings.
    auto &order = clientOrder[owner];
    while (order.size() >= kMaxRemapEntriesPerClient) {
        // Cap reached: the oldest outstanding send has long since timed out on
        // the client; dropping its mapping only means its (late) response is
        // fanned out with the server id instead of the client id.
        auto oldest = order.front();
        order.pop_front();
        remap.erase(oldest);
    }

    remap[serverId] = {owner, clientPacketId};
    order.push_back(serverId);
    LOG_DEBUG("MultiClient: remap client id 0x%x -> server id 0x%x", (unsigned)clientPacketId, (unsigned)serverId);
    return serverId;
}

MCDeliveryTarget *MultiClientHub::resolvePacket(uint32_t serverPacketId, uint32_t *clientPacketId) const
{
    auto it = remap.find(serverPacketId);
    if (it == remap.end())
        return nullptr;
    if (clientPacketId)
        *clientPacketId = it->second.clientPacketId;
    return it->second.owner;
}

void MultiClientHub::deliverPacket(meshtastic_MeshPacket *p)
{
    MCDeliveryTarget *owner = nullptr;

    // A response (routing ACK/NAK, route reply, nodeinfo reply, ...) that
    // references one of our remapped ids goes only to the client that sent
    // the original packet, with its id restored. Responses come from remote
    // nodes, so this branch must stay open to any origin.
    if (p->decoded.request_id != 0)
        owner = resolvePacket(p->decoded.request_id, &p->decoded.request_id);

    // The packet itself is a client-originated packet (our own broadcast,
    // sim-mode echo, ...): restore the client id, still fan out to everyone.
    // Only local echoes are eligible (MULT-184 F7): id and request_id are
    // peer-controllable protocol fields, so a remote packet whose id equals a
    // live (small, sequential) server id must not be rewritten to that
    // client's original id or owner-routed away from the other clients.
    if (!owner && p->id != 0 && isFromUs(p))
        resolvePacket(p->id, &p->id);

    if (owner) {
        owner->mcEnqueuePacket(p); // ownership moves to the client's queue
        return;
    }

    if (clients.empty()) {
        packetPool.release(p);
        return;
    }
    for (size_t i = 0; i < clients.size(); i++) {
        if (i + 1 == clients.size())
            clients[i]->mcEnqueuePacket(p); // last client takes ownership
        else
            clients[i]->mcEnqueuePacket(packetPool.allocCopy(*p));
    }
}

void MultiClientHub::deliverQueueStatus(meshtastic_QueueStatus *q)
{
    if (q->mesh_packet_id != 0) {
        if (auto *owner = resolvePacket(q->mesh_packet_id, &q->mesh_packet_id)) {
            owner->mcEnqueueQueueStatus(q);
            return;
        }
    }
    // Generic (mesh_packet_id == 0) or unknown: fan out to all clients
    if (clients.empty()) {
        queueStatusPool.release(q);
        return;
    }
    for (size_t i = 0; i < clients.size(); i++) {
        if (i + 1 == clients.size())
            clients[i]->mcEnqueueQueueStatus(q);
        else
            clients[i]->mcEnqueueQueueStatus(queueStatusPool.allocCopy(*q));
    }
}

void MultiClientHub::deliverMqttClientProxyMessage(meshtastic_MqttClientProxyMessage *m)
{
    if (clients.empty()) {
        mqttClientProxyMessagePool.release(m);
        return;
    }
    for (size_t i = 0; i < clients.size(); i++) {
        if (i + 1 == clients.size())
            clients[i]->mcEnqueueMqttClientProxyMessage(m);
        else
            clients[i]->mcEnqueueMqttClientProxyMessage(mqttClientProxyMessagePool.allocCopy(*m));
    }
}

void MultiClientHub::deliverClientNotification(meshtastic_ClientNotification *n)
{
    if (n->has_reply_id && n->reply_id != 0) {
        if (auto *owner = resolvePacket(n->reply_id, &n->reply_id)) {
            owner->mcEnqueueClientNotification(n);
            return;
        }
    }
    if (clients.empty()) {
        clientNotificationPool.release(n);
        return;
    }
    for (size_t i = 0; i < clients.size(); i++) {
        if (i + 1 == clients.size())
            clients[i]->mcEnqueueClientNotification(n);
        else
            clients[i]->mcEnqueueClientNotification(clientNotificationPool.allocCopy(*n));
    }
}
