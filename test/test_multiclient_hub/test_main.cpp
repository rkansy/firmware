#include "MultiClientHub.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "TestUtil.h"
#include <cstring>
#include <vector>

#include <unity.h>

// A minimal MCDeliveryTarget that records what it is handed, so the hub's
// routing decisions can be asserted without a live TCP connection.
class StubClient : public MCDeliveryTarget
{
  public:
    std::vector<meshtastic_MeshPacket *> packets;
    std::vector<meshtastic_QueueStatus *> qstatus;

    void mcEnqueuePacket(meshtastic_MeshPacket *p) override { packets.push_back(p); }
    void mcEnqueueQueueStatus(meshtastic_QueueStatus *q) override { qstatus.push_back(q); }
    void mcEnqueueMqttClientProxyMessage(meshtastic_MqttClientProxyMessage *) override {}
    void mcEnqueueClientNotification(meshtastic_ClientNotification *) override {}

    void releaseAll()
    {
        for (auto *p : packets)
            packetPool.release(p);
        for (auto *q : qstatus)
            queueStatusPool.release(q);
        packets.clear();
        qstatus.clear();
    }
};

static MultiClientHub hub;
static StubClient a, b;

// isFromUs() (used by the F7 origin check in deliverPacket) resolves the local
// node number via nodeDB->getNodeNum(); give the tests a real (empty) NodeDB
// and a known my_node_num.
class TestNodeDB : public NodeDB
{
  public:
    meshtastic_NodeInfoLite *getMeshNode(NodeNum n) override { return &emptyNode; }
    meshtastic_NodeInfoLite emptyNode = {};
};
static TestNodeDB *testNodeDB = nullptr; // heap-allocated in setup() after env init

void setUp(void)
{
    nodeDB = testNodeDB;
    myNodeInfo = meshtastic_MyNodeInfo{.my_node_num = 0x1234};
    // fresh remap table + client list for each test
    hub.unregisterClient(&a);
    hub.unregisterClient(&b);
    hub.registerClient(&a);
    hub.registerClient(&b);
}

void tearDown(void)
{
    a.releaseAll();
    b.releaseAll();
    hub.unregisterClient(&a);
    hub.unregisterClient(&b);
}

static meshtastic_MeshPacket *allocPacket()
{
    meshtastic_MeshPacket z = meshtastic_MeshPacket_init_zero;
    return packetPool.allocCopy(z);
}

// A client id is remapped to a non-zero, distinct server id.
static void test_registerPacket_remapsToNonZero()
{
    uint32_t s1 = hub.registerPacket(&a, 0x1111);
    uint32_t s2 = hub.registerPacket(&b, 0x1111); // same client id, other client
    TEST_ASSERT_NOT_EQUAL(0, s1);
    TEST_ASSERT_NOT_EQUAL(0, s2);
    TEST_ASSERT_NOT_EQUAL(s1, s2); // concurrent clients must not collide
}

// id 0 (unset) is passed through untouched.
static void test_registerPacket_zeroPassthrough()
{
    TEST_ASSERT_EQUAL(0, hub.registerPacket(&a, 0));
}

// resolvePacket returns the owner and restores the client's original id.
static void test_resolvePacket_roundtrip()
{
    uint32_t s = hub.registerPacket(&a, 0xABCD);
    uint32_t out = 0;
    MCDeliveryTarget *o = hub.resolvePacket(s, &out);
    TEST_ASSERT_EQUAL_PTR(&a, o);
    TEST_ASSERT_EQUAL(0xABCD, out);
}

// Unknown ids resolve to nullptr and leave the id untouched.
static void test_resolvePacket_unknown()
{
    uint32_t out = 0x1234;
    TEST_ASSERT_NULL(hub.resolvePacket(0xDEAD, &out));
    TEST_ASSERT_EQUAL(0x1234, out);
}

// A response whose request_id references a remapped id goes only to the owner,
// with the client id restored.
static void test_deliverPacket_ownerRouting()
{
    uint32_t s = hub.registerPacket(&a, 0x55);
    meshtastic_MeshPacket *p = allocPacket();
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    p->decoded.request_id = s;

    hub.deliverPacket(p); // hub takes ownership

    TEST_ASSERT_EQUAL(1, a.packets.size());
    TEST_ASSERT_EQUAL(0, b.packets.size());
    TEST_ASSERT_EQUAL(0x55, a.packets[0]->decoded.request_id); // restored
    a.releaseAll();
}

// Live mesh traffic (no matching ids) fans out to every client unchanged.
static void test_deliverPacket_fanout()
{
    meshtastic_MeshPacket *p = allocPacket();
    p->id = 0x9999; // not a remapped id
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_POSITION_APP;
    p->decoded.request_id = 0;

    hub.deliverPacket(p);

    TEST_ASSERT_EQUAL(1, a.packets.size());
    TEST_ASSERT_EQUAL(1, b.packets.size());
    TEST_ASSERT_EQUAL(0x9999, a.packets[0]->id);
    TEST_ASSERT_EQUAL(0x9999, b.packets[0]->id);
    a.releaseAll();
    b.releaseAll();
}

// The client's own packet echoed back (id is a remapped id) fans out to all
// clients with the client id restored.
static void test_deliverPacket_ownEcho()
{
    uint32_t s = hub.registerPacket(&a, 0x77);
    meshtastic_MeshPacket *p = allocPacket();
    p->id = s;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->decoded.request_id = 0;

    hub.deliverPacket(p);

    TEST_ASSERT_EQUAL(1, a.packets.size());
    TEST_ASSERT_EQUAL(1, b.packets.size());
    TEST_ASSERT_EQUAL(0x77, a.packets[0]->id);
    TEST_ASSERT_EQUAL(0x77, b.packets[0]->id);
    a.releaseAll();
    b.releaseAll();
}

// A queue status carrying a remapped mesh_packet_id goes only to the owner,
// with the client id restored.
static void test_deliverQueueStatus_ownerRouting()
{
    uint32_t s = hub.registerPacket(&b, 0x42);
    meshtastic_QueueStatus z = meshtastic_QueueStatus_init_zero;
    z.mesh_packet_id = s;
    meshtastic_QueueStatus *q = queueStatusPool.allocCopy(z);

    hub.deliverQueueStatus(q);

    TEST_ASSERT_EQUAL(0, a.qstatus.size());
    TEST_ASSERT_EQUAL(1, b.qstatus.size());
    TEST_ASSERT_EQUAL(0x42, b.qstatus[0]->mesh_packet_id);
    b.releaseAll();
}

// A generic queue status (mesh_packet_id == 0) fans out to all clients.
static void test_deliverQueueStatus_genericFanout()
{
    meshtastic_QueueStatus z = meshtastic_QueueStatus_init_zero;
    z.mesh_packet_id = 0;
    meshtastic_QueueStatus *q = queueStatusPool.allocCopy(z);

    hub.deliverQueueStatus(q);

    TEST_ASSERT_EQUAL(1, a.qstatus.size());
    TEST_ASSERT_EQUAL(1, b.qstatus.size());
    a.releaseAll();
    b.releaseAll();
}

// Unregistering a client drops its remap entries (no dangling owner pointers).
static void test_unregister_dropsRemapEntries()
{
    uint32_t s = hub.registerPacket(&a, 0x31);
    hub.unregisterClient(&a);
    uint32_t out = 0;
    TEST_ASSERT_NULL(hub.resolvePacket(s, &out));
}

// F5: connect/disconnect churn leaves no stale remap bookkeeping: after
// unregister + reconnect the client's new registrations resolve and its old
// server ids stay dead.
static void test_unregister_compactsOrder()
{
    myNodeInfo = meshtastic_MyNodeInfo{.my_node_num = 0x1234};
    uint32_t s1 = hub.registerPacket(&a, 0x101);
    uint32_t s2 = hub.registerPacket(&a, 0x102);
    hub.registerPacket(&b, 0x202);

    hub.unregisterClient(&a); // drops a's map entries and its per-client FIFO
    hub.registerClient(&a);   // reconnect on the same endpoint

    uint32_t out = 0;
    TEST_ASSERT_NULL(hub.resolvePacket(s1, &out));
    TEST_ASSERT_NULL(hub.resolvePacket(s2, &out));

    uint32_t s3 = hub.registerPacket(&a, 0x103); // fresh registration after reconnect
    MCDeliveryTarget *o = hub.resolvePacket(s3, &out);
    TEST_ASSERT_EQUAL_PTR(&a, o);
    TEST_ASSERT_EQUAL(0x103, out);
}

// F6: eviction is per-client. A high-volume client overflowing its own cap
// must never evict another client's live mappings.
static void test_perClientEviction()
{
    myNodeInfo = meshtastic_MyNodeInfo{.my_node_num = 0x1234};
    // kMaxRemapEntriesPerClient (see MultiClientHub.h) is 2048.
    const uint32_t cap = 2048;

    // Fill a's per-client FIFO to the cap.
    uint32_t aOldest = hub.registerPacket(&a, 0x00000001);
    for (uint32_t i = 2; i <= cap; i++)
        hub.registerPacket(&a, i);

    // b registers one live mapping while the global table is full.
    uint32_t bId = hub.registerPacket(&b, 0xB0B);

    // a keeps sending: with per-client eviction only a's own oldest
    // mappings go away, b's mapping must survive.
    for (uint32_t i = cap + 1; i <= 2 * cap; i++)
        hub.registerPacket(&a, 0x01000000 + i);

    uint32_t out = 0;
    MCDeliveryTarget *o = hub.resolvePacket(bId, &out);
    TEST_ASSERT_EQUAL_PTR(&b, o); // b's live mapping was not evicted
    TEST_ASSERT_EQUAL(0xB0B, out);

    TEST_ASSERT_NULL(hub.resolvePacket(aOldest, &out));      // a's oldest mapping was evicted
    o = hub.resolvePacket(hub.registerPacket(&a, 0xCAFE), &out);
    TEST_ASSERT_EQUAL_PTR(&a, o);                            // a's new mappings still work
    TEST_ASSERT_EQUAL(0xCAFE, out);
}

// F7: a local echo (from == my node) gets the client id restored and is fanned
// out; a remote packet carrying the same id must not be rewritten or
// owner-routed; remote responses matching on request_id still owner-route.
static void test_echoResolutionRequiresLocalOrigin()
{
    myNodeInfo = meshtastic_MyNodeInfo{.my_node_num = 0x1234};
    uint32_t s = hub.registerPacket(&a, 0x77);

    // Local echo: id restored, fanned out to everyone.
    {
        meshtastic_MeshPacket *p = allocPacket();
        p->id = s;
        p->from = myNodeInfo.my_node_num;
        p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        hub.deliverPacket(p);
        TEST_ASSERT_EQUAL(1, a.packets.size());
        TEST_ASSERT_EQUAL(1, b.packets.size());
        TEST_ASSERT_EQUAL(0x77, a.packets[0]->id);
        TEST_ASSERT_EQUAL(0x77, b.packets[0]->id);
        a.releaseAll();
        b.releaseAll();
    }

    // Remote packet with the same id: no rewrite, plain fan-out.
    {
        meshtastic_MeshPacket *p = allocPacket();
        p->id = s;
        p->from = 0xBEEF; // a remote node
        p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        p->decoded.portnum = meshtastic_PortNum_POSITION_APP;
        hub.deliverPacket(p);
        TEST_ASSERT_EQUAL(1, a.packets.size());
        TEST_ASSERT_EQUAL(1, b.packets.size());
        TEST_ASSERT_EQUAL(s, a.packets[0]->id); // unchanged server id
        TEST_ASSERT_EQUAL(s, b.packets[0]->id);
        a.releaseAll();
        b.releaseAll();
    }

    // Remote response referencing the remapped request_id: owner-routed.
    {
        meshtastic_MeshPacket *p = allocPacket();
        p->id = 0x999999; // an unrelated remote id
        p->from = 0xBEEF;
        p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        p->decoded.portnum = meshtastic_PortNum_ROUTING_APP;
        p->decoded.request_id = s;
        hub.deliverPacket(p);
        TEST_ASSERT_EQUAL(1, a.packets.size());
        TEST_ASSERT_EQUAL(0, b.packets.size());
        TEST_ASSERT_EQUAL(0x77, a.packets[0]->decoded.request_id); // restored
        a.releaseAll();
    }
}

void setup()
{
    initializeTestEnvironment();
    testNodeDB = new TestNodeDB(); // construct after the test env is ready

    UNITY_BEGIN();
    RUN_TEST(test_registerPacket_remapsToNonZero);
    RUN_TEST(test_registerPacket_zeroPassthrough);
    RUN_TEST(test_resolvePacket_roundtrip);
    RUN_TEST(test_resolvePacket_unknown);
    RUN_TEST(test_deliverPacket_ownerRouting);
    RUN_TEST(test_deliverPacket_fanout);
    RUN_TEST(test_deliverPacket_ownEcho);
    RUN_TEST(test_deliverQueueStatus_ownerRouting);
    RUN_TEST(test_deliverQueueStatus_genericFanout);
    RUN_TEST(test_unregister_dropsRemapEntries);
    RUN_TEST(test_unregister_compactsOrder);
    RUN_TEST(test_perClientEviction);
    RUN_TEST(test_echoResolutionRequiresLocalOrigin);
    exit(UNITY_END());
}

void loop() {}
