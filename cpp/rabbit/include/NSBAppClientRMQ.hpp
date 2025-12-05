#pragma once

#include "NSBClientRMQ.hpp"

namespace nsb {

class NSBAppClientRMQ : public NSBClientRMQ {
  public:
    NSBAppClientRMQ(const std::string& id, const std::string& host, int port)
        : NSBClientRMQ(id, host, port, Originator::APP_CLIENT) {}

    void send(const std::string& dest_id, const std::string& payload);
    bool receive(MessageEntry& out, int timeout_seconds = 0);
};

}
