#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include "nsb.pb.h"

namespace nsb {

class NSBDaemonRMQ {
  public:
    explicit NSBDaemonRMQ(const std::string& host, int port);
    ~NSBDaemonRMQ();

    void start();
    void stop();

    // Configuration to return on INIT
    void set_config(int sys_mode, int sim_mode, bool use_db = false,
                    const std::string& db_address = "", int db_port = 0, int db_num = 0);

  private:
    std::string                host_;
    int                        port_{};
    AmqpClient::Channel::ptr_t chan_;
    std::atomic<bool>          running_{false};
    std::thread                worker_;

    // config cache
    nsb::nsbm::ConfigParams cfg_;

    void run_loop();
    void handle_message(const AmqpClient::Envelope::ptr_t& env);
    void reply_init(const nsb::nsbm& req);
    void reply_ping(const nsb::nsbm& req);
};

}
