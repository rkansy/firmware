#include "MultiClientApiServerPort.h"
#include "configuration.h"

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

    // Accept all pending connections; each keeps its own session
    for (;;) {
        auto client = WiFiServer::available(); // non-blocking accept()
        if (!client)
            break;
        LOG_INFO("Accepted API client, %d connected", (int)apis.size() + 1);
        apis.push_back(std::make_unique<WiFiServerAPI>(client));
    }
    return 100; // only check occasionally for incoming connections
}
