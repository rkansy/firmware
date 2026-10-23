#include "configuration.h"
#include <Arduino.h>

#if HAS_WIFI
#include "WiFiServerAPI.h"
#if defined(ARCH_PORTDUINO)
#include "platform/portduino/MultiClientApiServerPort.h"
#endif

static WiFiServerPort *apiPort;

void initApiServer(int port)
{
    // Start API server on port 4403
    if (!apiPort) {
#if defined(ARCH_PORTDUINO)
        // Our daemon builds: multiple concurrent API clients (stock is single-connection)
        apiPort = new MultiClientApiServerPort(port);
#else
        apiPort = new WiFiServerPort(port);
#endif
        LOG_INFO("API server listen on TCP port %d", port);
        apiPort->init();
    }
}
void deInitApiServer()
{
    if (apiPort) {
        delete apiPort;
        apiPort = nullptr;
    }
}

WiFiServerAPI::WiFiServerAPI(WiFiClient &_client) : ServerAPI(_client)
{
    api_type = TYPE_WIFI;
    LOG_INFO("Incoming wifi connection");
#if defined(ARCH_PORTDUINO)
    // Multi-client delivery: this connection gets its own config handshake and
    // its own per-client FromRadio queues (see MultiClientHub).
    enableMultiClientDelivery();
#endif
}

WiFiServerPort::WiFiServerPort(int port) : APIServerPort(port) {}
#endif