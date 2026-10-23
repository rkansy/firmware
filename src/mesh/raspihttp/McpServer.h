#pragma once

#if defined(ARCH_PORTDUINO) && defined(PORTDUINO_LINUX_HARDWARE) && __has_include(<ulfius.h>) && __has_include(<jansson.h>)
#include "concurrency/OSThread.h"
#include <condition_variable>
#include <deque>
#include <jansson.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <ulfius.h>

class MeshtasticMcpServer : private concurrency::OSThread
{
  public:
    explicit MeshtasticMcpServer(int port);
    ~MeshtasticMcpServer();

    MeshtasticMcpServer(const MeshtasticMcpServer &) = delete;
    MeshtasticMcpServer &operator=(const MeshtasticMcpServer &) = delete;

    json_t *dispatchRequest(json_t *request);
    void shutdown();

  private:
    struct Job {
        json_t *request = nullptr;
        json_t *response = nullptr;
        bool done = false;
        bool abandoned = false;
        std::condition_variable completed;
        ~Job()
        {
            json_decref(request);
            json_decref(response);
        }
    };

    int32_t runOnce() override;
    std::mutex jobsMutex;
    std::deque<std::shared_ptr<Job>> jobs;
    std::shared_ptr<Job> activeJob;
    bool shuttingDown = false;
    uint32_t lastSnapshotMillis = 0;
    json_int_t lastSnapshotHour = -1;
    uint8_t snapshotsSincePrune = 0;
    bool snapshotTimerStarted = false;
    struct _u_instance instance{};
    struct sockaddr_in bindAddress{};
    bool initialized = false;
};
#endif
