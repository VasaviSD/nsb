#pragma once

#include <string>
#include <memory>
#include "RabbitMQInterface.hpp"
#include "nsb.pb.h"

namespace nsb {

struct Config {
    enum class SystemMode {
        PULL = 0,
        PUSH = 1
    };
    enum class SimulatorMode {
        SYSTEM_WIDE = 0,
        PER_NODE    = 1
    };
    SystemMode    sys_mode{SystemMode::PULL};
    SimulatorMode sim_mode{SimulatorMode::SYSTEM_WIDE};
    bool          use_db{false};
    std::string   db_address;
    int           db_port{0};
    int           db_num{0};
};

struct MessageEntry {
    std::string src_id;
    std::string dest_id;
    std::string payload;
};

class NSBClientRMQ {
  public:
    enum class Originator { APP_CLIENT,
                            SIM_CLIENT };

    NSBClientRMQ(std::string id, std::string host, int port, Originator og);
    virtual ~NSBClientRMQ();

    void initialize(int timeout_sec = 10);
    void ping();
    void exit();

    const Config&      config() const { return cfg_; }
    const std::string& id() const { return id_; }

  protected:
    std::string                        id_;
    std::string                        host_;
    int                                port_{};
    Originator                         og_;
    Config                             cfg_;
    std::unique_ptr<RabbitMQInterface> comms_;

    // helpers
    nsb::nsbm buildBaseMsg(nsb::nsbm::Manifest::Operation op) const;
};
}
