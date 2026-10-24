#pragma once

#include <memory>
#include <vector>

#include "mesh/api/WiFiServerAPI.h"

/**
 * MultiClientApiServerPort — TCP API server port for the portduino (Linux
 * daemon) builds that accepts any number of concurrent clients.
 *
 * The stock APIServerPort has single-connection semantics: a new connection
 * force-closes the previous one. Our build instead gives every connection its
 * own WiFiServerAPI (per-connection PhoneAPI state machine: independent config
 * handshake, config_complete_id, ...). Cross-client fan-out of live FromRadio
 * traffic and per-client MeshPacket id remapping are done by the
 * MultiClientHub (src/mesh/MultiClientHub.h).
 *
 * Wire framing is byte-identical to stock (0x94 0xC3 + 2-byte BE length), so
 * existing single-client clients work unmodified.
 *
 * Concurrent sessions are capped (kMaxApiClients, see the .cpp) so the
 * per-session OSThread registrations cannot overflow the cooperative thread
 * registry; connections beyond the cap are rejected.
 */
class MultiClientApiServerPort : public WiFiServerPort
{
  public:
    explicit MultiClientApiServerPort(int port) : WiFiServerPort(port) {}

    int clientCount() const { return (int)apis.size(); }

  protected:
    int32_t runOnce() override;

  private:
    std::vector<std::unique_ptr<WiFiServerAPI>> apis;
};
