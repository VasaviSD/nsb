#pragma once

#include "NSBClientRMQ.hpp"
#include <functional>

namespace nsb {

class NSBSimClientRMQ : public NSBClientRMQ {
  public:
    using MessageCallback = std::function<void(const MessageEntry&)>;

    NSBSimClientRMQ(const std::string& id, const std::string& host, int port)
        : NSBClientRMQ(id, host, port, Originator::SIM_CLIENT) {}

    // Fetch a message
    bool fetch(MessageEntry& out, int timeout_seconds = 0);

    // Post a processed message
    void post(const std::string& src_id, const std::string& dest_id, const std::string& payload);

    // Async listen for PUSH mode: spawns background thread, invokes callback on each message
    void listen(MessageCallback callback);
    void stop_listen();
    bool is_listening() const;
};

}
