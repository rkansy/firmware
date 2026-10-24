#include "MultiClientApiServerPort.h"
#include "configuration.h"

namespace
{
/// Admission cap for concurrent API sessions (MULT-184 F1).
///
/// Every accepted session is a WiFiServerAPI, whose ServerAPI base is an
/// OSThread registered in the cooperative thread registry — a fixed
/// Thread* thread[MAX_THREADS] with MAX_THREADS=32
/// (ArduinoThread/ThreadController.h). The daemon already runs ~10
/// cooperative firmware threads (main loop, radio, GPS, sensors, webserver,
/// ...), so an unbounded accept loop hit the registry cap on the ~22nd
/// client and the OSThread constructor's `assert(added)` aborted the
/// process — a remote DoS from any host that can reach port 4403. 8
/// concurrent sessions leaves comfortable headroom in the registry and is
/// far above the number of real simultaneous clients.
constexpr size_t kMaxApiClients = 8;
} // namespace

int32_t MultiClientApiServerPort::runOnce()
{
    // Reap connections whose clients already disconnected. Their ServerAPI has
    // already called PhoneAPI::close() (unregistering from the hub and
    // releasing the per-client queue contents); destroying the instance closes
    // the socket. One client going away never touches the others' sessions.
    for (auto it = apis.begin(); it != apis.end();) {
        if (!(*it)->checkIsConnected()) {
            LOG_INFO("Client dropped TCP connection, closing it");
            it = apis.erase(it);
        } else {
            ++it;
        }
    }

    // Accept pending connections up to the admission cap; each accepted
    // client keeps its own session. Accepting happens on the cooperative main
    // loop, so the size check is race-free. Connections beyond the cap are
    // rejected (closed immediately), never queued for later.
    int rejected = 0;
    for (;;) {
        auto client = WiFiServer::available(); // non-blocking accept()
        if (!client)
            break;
        if (apis.size() >= kMaxApiClients) {
            client.stop();
            rejected++;
            continue;
        }
        LOG_INFO("Accepted API client, %d connected", (int)apis.size() + 1);
        apis.push_back(std::make_unique<WiFiServerAPI>(client));
    }
    if (rejected)
        LOG_WARN("MultiClient: API admission cap (%d) reached, rejected %d connection(s)", (int)kMaxApiClients, rejected);
    return 100; // only check occasionally for incoming connections
}
