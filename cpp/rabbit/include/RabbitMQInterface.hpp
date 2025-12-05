#pragma once

#include <memory>
#include <string>
#include <chrono>
#include <SimpleAmqpClient/SimpleAmqpClient.h>

namespace nsb {

enum class ChannelKind { CTRL,
                         SEND,
                         RECV };

class RabbitMQInterface {
  public:
    RabbitMQInterface(const std::string& host, int port, const std::string& client_id);
    ~RabbitMQInterface();

    void connect(int timeout_seconds = 10);
    void close();

    // Publish bytes on channel. For SEND, dest_id routes to nsb.recv.{dest_id}
    void send(ChannelKind ch, const std::string& bytes, const std::string& dest_id = "");

    // Receive one message body; timeout_seconds <0 => wait forever, 0 => poll once
    bool recv(ChannelKind ch, std::string& out, int timeout_seconds = 0);

    // Queue names
    std::string queue_name(ChannelKind ch) const;

  private:
    std::string                host_;
    int                        port_{};
    std::string                id_;
    AmqpClient::Channel::ptr_t chan_;

    // Cached queue names
    std::string q_ctrl_;
    std::string q_send_;
    std::string q_recv_;

    void declare_exchange_and_queues();
};

}
