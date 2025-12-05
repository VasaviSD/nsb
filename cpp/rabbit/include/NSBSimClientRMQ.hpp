#pragma once

#include "NSBClientRMQ.hpp"

namespace nsb {

class NSBSimClientRMQ : public NSBClientRMQ {
  public:
    NSBSimClientRMQ(const std::string& id, const std::string& host, int port)
        : NSBClientRMQ(id, host, port, Originator::SIM_CLIENT) {}

    // Fetch a message
    bool fetch(MessageEntry& out, int timeout_seconds = 0);

    // Do some processing

    // Post a processed message
    void post(const std::string& src_id, const std::string& dest_id, const std::string& payload);
};

}
