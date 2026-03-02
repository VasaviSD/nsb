#pragma once

#include <memory>
#include <string>
#include <chrono>
#include <functional>
#include <thread>
#include <atomic>
#include <SimpleAmqpClient/SimpleAmqpClient.h>

namespace nsb {

enum class ChannelKind { CTRL,
                         SEND,
                         RECV };

class RabbitMQInterface {
  public:
    using MessageCallback = std::function<void(const std::string&)>;

    RabbitMQInterface(const std::string& host, int port, const std::string& client_id);
    ~RabbitMQInterface();

    void connect(int timeout_seconds = 10);
    void close();

    // Publish bytes on channel. For SEND, dest_id routes to nsb.recv.{dest_id}
    void send(ChannelKind ch, const std::string& bytes, const std::string& dest_id = "");

    // Receive one message body; timeout_seconds <0 => wait forever, 0 => poll once
    bool recv(ChannelKind ch, std::string& out, int timeout_seconds = 0);

    // Async listen: spawns background thread, invokes callback on each message
    void async_listen(ChannelKind ch, MessageCallback callback);
    void stop_listen();
    bool is_listening() const { return listening_.load(); }

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

    // Async listen state
    std::atomic<bool> listening_{false};
    std::thread       listen_thread_;
    std::string       listen_consumer_tag_;

    void declare_exchange_and_queues();
    void listen_loop(ChannelKind ch, MessageCallback callback);
};

}
