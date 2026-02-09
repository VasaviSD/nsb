#pragma once

#include "NSBClientRMQ.hpp"
#include <functional>

namespace nsb {

class NSBAppClientRMQ : public NSBClientRMQ {
  public:
    using MessageCallback = std::function<void(const MessageEntry&)>;

    NSBAppClientRMQ(const std::string& id, const std::string& host, int port)
        : NSBClientRMQ(id, host, port, Originator::APP_CLIENT) {}

    void send(const std::string& dest_id, const std::string& payload);
    bool receive(MessageEntry& out, int timeout_seconds = 0);

    // Async listen for PUSH mode: spawns background thread, invokes callback on each message
    void listen(MessageCallback callback);
    void stop_listen();
    bool is_listening() const;
};

}
