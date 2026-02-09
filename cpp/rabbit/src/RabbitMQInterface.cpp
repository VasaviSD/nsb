#include "RabbitMQInterface.hpp"
#include <stdexcept>
#include <thread>
#include <iostream>

namespace nsb {

static const std::string kExchange = "nsb";

RabbitMQInterface::RabbitMQInterface(const std::string& host, int port, const std::string& client_id)
    : host_(host), port_(port), id_(client_id) {}

RabbitMQInterface::~RabbitMQInterface() {
    try {
        close();
    } catch (...) {
    }
}

void RabbitMQInterface::connect(int timeout_seconds) {
    // SimpleAmqpClient connects via URI like amqp://user:pass@host:port
    std::string uri   = "amqp://guest:guest@" + host_ + ":" + std::to_string(port_);
    auto        start = std::chrono::steady_clock::now();
    while (true) {
        try {
            chan_ = AmqpClient::Channel::CreateFromUri(uri);
            declare_exchange_and_queues();
            return;
        } catch (const std::exception& e) {
            if (timeout_seconds >= 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
                if (elapsed >= timeout_seconds) {
                    throw std::runtime_error(std::string("RabbitMQ connect timeout: ") + e.what());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

void RabbitMQInterface::declare_exchange_and_queues() {
    chan_->DeclareExchange(kExchange, AmqpClient::Channel::EXCHANGE_TYPE_DIRECT, false, true);

    // CTRL listens on nsb.config.response.{id}, publishes to nsb.config.request
    q_ctrl_ = "nsb.config.response." + id_;
    chan_->DeclareQueue(q_ctrl_, false, true, false, false);
    chan_->BindQueue(q_ctrl_, kExchange, q_ctrl_);

    // SEND/RECV per-client queues
    q_send_ = "nsb.send." + id_;
    chan_->DeclareQueue(q_send_, false, true, false, false);
    chan_->BindQueue(q_send_, kExchange, q_send_);

    q_recv_ = "nsb.recv." + id_;
    chan_->DeclareQueue(q_recv_, false, true, false, false);
    chan_->BindQueue(q_recv_, kExchange, q_recv_);

    // Ensure config.request exists/bound
    chan_->DeclareQueue("nsb.config.request", false, true, false, false);
    chan_->BindQueue("nsb.config.request", kExchange, "nsb.config.request");
}

void RabbitMQInterface::close() {
    if (chan_) {
        chan_.reset();
    }
}

std::string RabbitMQInterface::queue_name(ChannelKind ch) const {
    switch (ch) {
    case ChannelKind::CTRL:
        return q_ctrl_;
    case ChannelKind::SEND:
        return q_send_;
    case ChannelKind::RECV:
        return q_recv_;
    }
    return {};
}

void RabbitMQInterface::send(ChannelKind ch, const std::string& bytes, const std::string& dest_id) {
    if (!chan_)
        throw std::runtime_error("RabbitMQ not connected");
    std::string routing;
    if (ch == ChannelKind::CTRL) {
        routing = "nsb.config.request";
    } else if (ch == ChannelKind::SEND && !dest_id.empty()) {
        routing = "nsb.recv." + dest_id;
    } else {
        routing = queue_name(ch);
    }
    auto msg = AmqpClient::BasicMessage::Create(bytes);
    msg->DeliveryMode(AmqpClient::BasicMessage::dm_nonpersistent);
    chan_->BasicPublish(kExchange, routing, msg);
}

bool RabbitMQInterface::recv(ChannelKind ch, std::string& out, int timeout_seconds) {
    if (!chan_)
        throw std::runtime_error("RabbitMQ not connected");

    // Use a consumer for timeout support
    const std::string q        = queue_name(ch);
    const std::string tag      = chan_->BasicConsume(q, "", true, true, false);
    auto              deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds < 0 ? 0 : timeout_seconds);

    while (true) {
        AmqpClient::Envelope::ptr_t env;
        bool                        ok = false;
        if (timeout_seconds < 0) {
            env = chan_->BasicConsumeMessage(tag);
            ok  = static_cast<bool>(env);
        } else {
            int ms = 0;
            if (timeout_seconds == 0) {
                ms = 0; // poll
            } else {
                auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    chan_->BasicCancel(tag);
                    return false;
                }
                ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                if (ms < 0)
                    ms = 0;
            }
            ok = chan_->BasicConsumeMessage(tag, env, ms);
        }
        if (ok && env) {
            out = env->Message()->Body();
            chan_->BasicCancel(tag);
            return true;
        }
        if (timeout_seconds == 0) {
            chan_->BasicCancel(tag);
            return false;
        }
        // loop until deadline
    }
}

void RabbitMQInterface::async_listen(ChannelKind ch, MessageCallback callback) {
    if (listening_.load()) {
        throw std::runtime_error("Already listening");
    }
    if (!chan_) {
        throw std::runtime_error("RabbitMQ not connected");
    }
    listening_ = true;
    listen_thread_ = std::thread(&RabbitMQInterface::listen_loop, this, ch, callback);
}

void RabbitMQInterface::stop_listen() {
    if (!listening_.load()) return;
    listening_ = false;
    // The listen loop polls with timeout, so it will exit on next iteration
    if (listen_thread_.joinable()) {
        listen_thread_.join();
    }
}

void RabbitMQInterface::listen_loop(ChannelKind ch, MessageCallback callback) {
    const std::string q = queue_name(ch);
    // no_local=true, no_ack=true, exclusive=false
    listen_consumer_tag_ = chan_->BasicConsume(q, "", true, true, false);

    while (listening_.load()) {
        AmqpClient::Envelope::ptr_t env;
        // Poll with 500ms timeout to allow checking listening_ flag
        bool ok = chan_->BasicConsumeMessage(listen_consumer_tag_, env, 500);
        if (ok && env) {
            try {
                callback(env->Message()->Body());
            } catch (const std::exception& e) {
                // Log but don't crash the listen loop
                std::cerr << "Callback error: " << e.what() << std::endl;
            }
        }
    }

    try {
        chan_->BasicCancel(listen_consumer_tag_);
    } catch (...) {
        // Ignore cancel errors on shutdown
    }
    listen_consumer_tag_.clear();
}

}
