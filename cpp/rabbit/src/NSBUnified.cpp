/**
 * @file NSBUnified.cpp
 * @brief Unified NSB Client implementation with Socket and RabbitMQ backends.
 */

#include "NSBUnified.hpp"

// Socket includes
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

// RabbitMQ includes
#include <SimpleAmqpClient/SimpleAmqpClient.h>

// Redis includes
#include <hiredis/hiredis.h>

namespace nsb_unified {

// ═════════════════════════════════════════════════════════════════════════════
// SocketBackend
// ═════════════════════════════════════════════════════════════════════════════

SocketBackend::SocketBackend(const std::string& host, int port)
    : host_(host), port_(port) {
    connect(NSB_UNIFIED_SERVER_CONNECTION_TIMEOUT);
}

SocketBackend::~SocketBackend() {
    close();
}

void SocketBackend::connect(int timeout_seconds) {
    auto start  = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::seconds(timeout_seconds);

    std::vector<Channel> channels = {Channel::CTRL, Channel::SEND, Channel::RECV};
    for (auto ch : channels) {
        while (std::chrono::steady_clock::now() < deadline) {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                throw std::runtime_error("SocketBackend: socket() failed");

            int opt = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
            ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = ::inet_addr(host_.c_str());
            addr.sin_port        = htons(static_cast<uint16_t>(port_));

            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                conns_[ch] = fd;
                break;
            }
            ::close(fd);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (conns_.find(ch) == conns_.end()) {
            throw std::runtime_error(
                "SocketBackend: connection timed out after " +
                std::to_string(timeout_seconds) + "s");
        }
    }
    // Set all sockets non-blocking after connection
    for (auto& [ch, fd] : conns_) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void SocketBackend::close() {
    if (closed_) return;
    closed_ = true;
    for (auto& [ch, fd] : conns_) {
        ::shutdown(fd, SHUT_WR);
        ::close(fd);
    }
    conns_.clear();
}

int SocketBackend::fd(Channel ch) const {
    auto it = conns_.find(ch);
    if (it == conns_.end())
        throw std::runtime_error("SocketBackend: channel not connected");
    return it->second;
}

void SocketBackend::sendMsg(Channel ch, const std::string& bytes,
                            const std::string& /*dest_id*/) {
    int sock = fd(ch);
    int total_sent = 0;
    int total_size = static_cast<int>(bytes.size());
    while (total_sent < total_size) {
        int sent = ::send(sock, bytes.data() + total_sent,
                          total_size - total_sent, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throw std::runtime_error(
                std::string("SocketBackend::sendMsg failed: ") + strerror(errno));
        }
        total_sent += sent;
    }
}

bool SocketBackend::recvMsg(Channel ch, std::string& out,
                            int timeout_seconds) {
    int sock = fd(ch);
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock, &read_fds);

    timeval  tv{};
    timeval* tv_ptr = nullptr;
    if (timeout_seconds >= 0) {
        tv.tv_sec  = timeout_seconds;
        tv.tv_usec = 0;
        tv_ptr     = &tv;
    }

    int activity = ::select(sock + 1, &read_fds, nullptr, nullptr, tv_ptr);
    if (activity <= 0) return false;

    out.clear();
    char buf[NSB_UNIFIED_RECEIVE_BUFFER_SIZE];
    bool got_data = false;
    while (true) {
        int n = ::recv(sock, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            out.append(buf, n);
            got_data = true;
        } else {
            break;
        }
    }
    return got_data;
}

// ═════════════════════════════════════════════════════════════════════════════
// RabbitMQBackendImpl (hidden implementation)
// ═════════════════════════════════════════════════════════════════════════════

static const std::string kExchange = "nsb";

class RabbitMQBackendImpl {
  public:
    RabbitMQBackendImpl(const std::string& host, int port,
                        const std::string& client_id)
        : host_(host), port_(port), id_(client_id) {}

    void connect(int timeout_seconds) {
        std::string uri = "amqp://guest:guest@" + host_ + ":" + std::to_string(port_);
        auto start = std::chrono::steady_clock::now();
        while (true) {
            try {
                chan_ = AmqpClient::Channel::CreateFromUri(uri);
                declareQueues();
                return;
            } catch (const std::exception& e) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed >= timeout_seconds)
                    throw std::runtime_error(
                        std::string("RabbitMQ connect timeout: ") + e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }

    void close() {
        chan_.reset();
    }

    std::string queueName(Channel ch) const {
        switch (ch) {
        case Channel::CTRL: return q_ctrl_;
        case Channel::SEND: return q_send_;
        case Channel::RECV: return q_recv_;
        }
        return {};
    }

    void send(Channel ch, const std::string& bytes,
              const std::string& dest_id) {
        if (!chan_)
            throw std::runtime_error("RabbitMQ not connected");
        std::string routing;
        if (ch == Channel::CTRL) {
            routing = "nsb.config.request";
        } else if (ch == Channel::SEND && !dest_id.empty()) {
            routing = "nsb.recv." + dest_id;
        } else {
            routing = queueName(ch);
        }
        auto msg = AmqpClient::BasicMessage::Create(bytes);
        msg->DeliveryMode(AmqpClient::BasicMessage::dm_nonpersistent);
        chan_->BasicPublish(kExchange, routing, msg);
    }

    bool recv(Channel ch, std::string& out, int timeout_seconds) {
        if (!chan_)
            throw std::runtime_error("RabbitMQ not connected");
        const std::string q   = queueName(ch);
        const std::string tag = chan_->BasicConsume(q, "", true, true, false);
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(timeout_seconds < 0 ? 3600 : timeout_seconds);

        while (true) {
            AmqpClient::Envelope::ptr_t env;
            bool ok = false;
            if (timeout_seconds < 0) {
                env = chan_->BasicConsumeMessage(tag);
                ok  = static_cast<bool>(env);
            } else {
                int ms = 0;
                if (timeout_seconds == 0) {
                    ms = 0;
                } else {
                    auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        chan_->BasicCancel(tag);
                        return false;
                    }
                    ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - now).count());
                    if (ms < 0) ms = 0;
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
        }
    }

  private:
    std::string host_;
    int         port_;
    std::string id_;
    AmqpClient::Channel::ptr_t chan_;
    std::string q_ctrl_;
    std::string q_send_;
    std::string q_recv_;

    void declareQueues() {
        chan_->DeclareExchange(kExchange, AmqpClient::Channel::EXCHANGE_TYPE_DIRECT,
                              false, true);
        q_ctrl_ = "nsb.config.response." + id_;
        chan_->DeclareQueue(q_ctrl_, false, true, false, false);
        chan_->BindQueue(q_ctrl_, kExchange, q_ctrl_);

        q_send_ = "nsb.send." + id_;
        chan_->DeclareQueue(q_send_, false, true, false, false);
        chan_->BindQueue(q_send_, kExchange, q_send_);

        q_recv_ = "nsb.recv." + id_;
        chan_->DeclareQueue(q_recv_, false, true, false, false);
        chan_->BindQueue(q_recv_, kExchange, q_recv_);

        chan_->DeclareQueue("nsb.config.request", false, true, false, false);
        chan_->BindQueue("nsb.config.request", kExchange, "nsb.config.request");
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// RabbitMQBackend (pimpl forwarding)
// ═════════════════════════════════════════════════════════════════════════════

RabbitMQBackend::RabbitMQBackend(const std::string& host, int port,
                                 const std::string& client_id)
    : impl_(std::make_unique<RabbitMQBackendImpl>(host, port, client_id)) {
    impl_->connect(NSB_UNIFIED_SERVER_CONNECTION_TIMEOUT);
}

RabbitMQBackend::~RabbitMQBackend() = default;

void RabbitMQBackend::sendMsg(Channel ch, const std::string& bytes,
                              const std::string& dest_id) {
    impl_->send(ch, bytes, dest_id);
}

bool RabbitMQBackend::recvMsg(Channel ch, std::string& out,
                              int timeout_seconds) {
    return impl_->recv(ch, out, timeout_seconds);
}

void RabbitMQBackend::close() {
    impl_->close();
}

// ═════════════════════════════════════════════════════════════════════════════
// RedisConnector
// ═════════════════════════════════════════════════════════════════════════════

RedisConnector::RedisConnector(const std::string& client_id,
                               const std::string& address, int port)
    : client_id_(client_id), address_(address), port_(port) {
    doConnect();
}

RedisConnector::~RedisConnector() {
    if (isConnected()) doDisconnect();
}

bool RedisConnector::isConnected() const {
    auto* ctx = static_cast<redisContext*>(context_);
    return ctx != nullptr && ctx->err == REDIS_OK;
}

bool RedisConnector::doConnect() {
    context_ = redisConnect(address_.c_str(), port_);
    auto* ctx = static_cast<redisContext*>(context_);
    if (ctx == nullptr || ctx->err) {
        std::cerr << "RedisConnector: connection failed";
        if (ctx) std::cerr << " — " << ctx->errstr;
        std::cerr << std::endl;
        return false;
    }
    return true;
}

void RedisConnector::doDisconnect() {
    if (context_) {
        redisFree(static_cast<redisContext*>(context_));
        context_ = nullptr;
    }
}

std::string RedisConnector::generateKey() {
    std::lock_guard<std::mutex> lock(mtx_);
    counter_ = (counter_ + 1) & 0xFFFFF;
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return std::to_string(now) + "-" + client_id_ + "-" + std::to_string(counter_);
}

std::string RedisConnector::store(const std::string& value) {
    if (!isConnected()) {
        std::cerr << "RedisConnector: not connected, cannot store" << std::endl;
        return "";
    }
    std::string key = generateKey();
    auto* ctx = static_cast<redisContext*>(context_);
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx, "SET %s %b", key.c_str(), value.data(), value.size()));
    if (ctx->err) {
        std::cerr << "RedisConnector SET error: " << ctx->errstr << std::endl;
        if (reply) freeReplyObject(reply);
        return "";
    }
    if (reply) freeReplyObject(reply);
    return key;
}

std::string RedisConnector::checkOut(const std::string& key) {
    if (!isConnected()) return "";
    auto* ctx = static_cast<redisContext*>(context_);
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx, "GETDEL %s", key.c_str()));
    if (ctx->err || !reply) return "";
    std::string result;
    if (reply->type != REDIS_REPLY_NIL && reply->str)
        result.assign(reply->str, reply->len);
    freeReplyObject(reply);
    return result;
}

std::string RedisConnector::peek(const std::string& key) {
    if (!isConnected()) return "";
    auto* ctx = static_cast<redisContext*>(context_);
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx, "GET %s", key.c_str()));
    if (!reply) return "";
    std::string result;
    if (reply->type != REDIS_REPLY_NIL && reply->str)
        result.assign(reply->str, reply->len);
    freeReplyObject(reply);
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// NSBClient (base)
// ═════════════════════════════════════════════════════════════════════════════

NSBClient::NSBClient(const std::string& id, const std::string& host, int port,
                     Backend backend, nsb::nsbm::Manifest::Originator og)
    : id_(id), host_(host), port_(port), backend_(backend), og_(og) {
    if (backend_ == Backend::SOCKET) {
        comms_ = std::make_unique<SocketBackend>(host_, port_);
    } else {
        comms_ = std::make_unique<RabbitMQBackend>(host_, port_, id_);
    }
}

NSBClient::~NSBClient() {
    try { exit(); } catch (...) {}
}

nsb::nsbm NSBClient::buildBaseMsg(nsb::nsbm::Manifest::Operation op) const {
    nsb::nsbm msg;
    msg.mutable_manifest()->set_op(op);
    msg.mutable_manifest()->set_og(og_);
    return msg;
}

std::string NSBClient::getPayload(const nsb::nsbm& msg) const {
    if (cfg_.use_db && db_) {
        return db_->checkOut(msg.msg_key());
    }
    return msg.payload();
}

void NSBClient::setPayload(nsb::nsbm& msg, const std::string& payload) {
    if (cfg_.use_db && db_) {
        std::string key = db_->store(payload);
        msg.set_msg_key(key);
    } else {
        msg.set_payload(payload);
    }
}

void NSBClient::initialize(int timeout_sec) {
    // Build INIT request
    auto msg = buildBaseMsg(nsb::nsbm::Manifest::INIT);
    msg.mutable_manifest()->set_code(nsb::nsbm::Manifest::SUCCESS);
    auto* intro = msg.mutable_intro();
    intro->set_identifier(id_);

    if (backend_ == Backend::SOCKET) {
        auto* sb = dynamic_cast<SocketBackend*>(comms_.get());
        if (sb) {
            // Get local address from CTRL socket
            struct sockaddr_storage addr;
            socklen_t len = sizeof(addr);
            if (getsockname(sb->fd(Channel::CTRL),
                            reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
                if (addr.ss_family == AF_INET) {
                    auto* s = reinterpret_cast<sockaddr_in*>(&addr);
                    intro->set_address(inet_ntoa(s->sin_addr));
                    intro->set_ch_ctrl(ntohs(s->sin_port));
                }
            }
            struct sockaddr_storage addr2;
            socklen_t len2 = sizeof(addr2);
            if (getsockname(sb->fd(Channel::SEND),
                            reinterpret_cast<sockaddr*>(&addr2), &len2) == 0) {
                if (addr2.ss_family == AF_INET) {
                    auto* s = reinterpret_cast<sockaddr_in*>(&addr2);
                    intro->set_ch_send(ntohs(s->sin_port));
                }
            }
            struct sockaddr_storage addr3;
            socklen_t len3 = sizeof(addr3);
            if (getsockname(sb->fd(Channel::RECV),
                            reinterpret_cast<sockaddr*>(&addr3), &len3) == 0) {
                if (addr3.ss_family == AF_INET) {
                    auto* s = reinterpret_cast<sockaddr_in*>(&addr3);
                    intro->set_ch_recv(ntohs(s->sin_port));
                }
            }
        }
    } else {
        intro->set_address(host_);
        intro->set_ch_ctrl(0);
        intro->set_ch_send(0);
        intro->set_ch_recv(0);
    }

    std::string wire;
    msg.SerializeToString(&wire);
    comms_->sendMsg(Channel::CTRL, wire);

    // Receive INIT response
    std::string resp;
    if (!comms_->recvMsg(Channel::CTRL, resp, timeout_sec)) {
        throw std::runtime_error("NSB INIT timeout");
    }
    nsb::nsbm m;
    if (!m.ParseFromString(resp))
        throw std::runtime_error("NSB INIT parse error");
    if (m.manifest().op() != nsb::nsbm::Manifest::INIT)
        throw std::runtime_error("NSB INIT unexpected operation");
    // Accept both SUCCESS and DAEMON_RESPONSE codes for cross-daemon compat
    if (m.manifest().code() != nsb::nsbm::Manifest::SUCCESS &&
        m.manifest().code() != nsb::nsbm::Manifest::DAEMON_RESPONSE) {
        throw std::runtime_error("NSB INIT failed");
    }

    // Parse config
    cfg_ = Config(m);

    // Set up Redis if needed
    if (cfg_.use_db) {
        db_ = std::make_unique<RedisConnector>(id_, cfg_.db_address, cfg_.db_port);
        if (!db_->isConnected()) {
            throw std::runtime_error("NSB INIT: failed to connect to Redis");
        }
    }
}

bool NSBClient::ping(int timeout_sec) {
    auto msg = buildBaseMsg(nsb::nsbm::Manifest::PING);
    msg.mutable_manifest()->set_code(nsb::nsbm::Manifest::SUCCESS);
    std::string wire;
    msg.SerializeToString(&wire);
    comms_->sendMsg(Channel::CTRL, wire);

    std::string resp;
    if (!comms_->recvMsg(Channel::CTRL, resp, timeout_sec))
        return false;
    nsb::nsbm m;
    if (!m.ParseFromString(resp)) return false;
    if (m.manifest().op() != nsb::nsbm::Manifest::PING) return false;
    return (m.manifest().code() == nsb::nsbm::Manifest::SUCCESS ||
            m.manifest().code() == nsb::nsbm::Manifest::DAEMON_RESPONSE);
}

void NSBClient::exit() {
    if (!comms_) return;
    auto msg = buildBaseMsg(nsb::nsbm::Manifest::EXIT);
    msg.mutable_manifest()->set_code(nsb::nsbm::Manifest::SUCCESS);
    std::string wire;
    msg.SerializeToString(&wire);
    comms_->sendMsg(Channel::CTRL, wire);
    comms_->close();
    comms_.reset();
}

// ═════════════════════════════════════════════════════════════════════════════
// NSBAppClient
// ═════════════════════════════════════════════════════════════════════════════

NSBAppClient::NSBAppClient(const std::string& id, const std::string& host,
                           int port, Backend backend)
    : NSBClient(id, host, port, backend, nsb::nsbm::Manifest::APP_CLIENT) {
    initialize();
}

std::string NSBAppClient::send(const std::string& dest_id,
                               const std::string& payload) {
    auto msg = buildBaseMsg(nsb::nsbm::Manifest::SEND);
    msg.mutable_manifest()->set_code(nsb::nsbm::Manifest::MESSAGE);
    auto* meta = msg.mutable_metadata();
    meta->set_src_id(id_);
    meta->set_dest_id(dest_id);
    meta->set_payload_size(static_cast<int>(payload.size()));

    setPayload(msg, payload);

    std::string wire;
    msg.SerializeToString(&wire);
    comms_->sendMsg(Channel::SEND, wire, dest_id);

    return cfg_.use_db ? msg.msg_key() : "";
}

MessageEntry NSBAppClient::receive(int timeout_seconds,
                                   const std::string& dest_id) {
    // In PULL mode with socket backend, send a RECEIVE request first
    if (backend_ == Backend::SOCKET &&
        cfg_.sys_mode == Config::SystemMode::PULL) {
        auto req = buildBaseMsg(nsb::nsbm::Manifest::RECEIVE);
        req.mutable_manifest()->set_code(nsb::nsbm::Manifest::SUCCESS);
        req.mutable_metadata()->set_dest_id(dest_id.empty() ? id_ : dest_id);
        std::string wire;
        req.SerializeToString(&wire);
        comms_->sendMsg(Channel::RECV, wire);
    } else if (backend_ == Backend::SOCKET &&
               cfg_.sys_mode == Config::SystemMode::PUSH) {
        timeout_seconds = 0;
    }

    std::string resp;
    if (!comms_->recvMsg(Channel::RECV, resp, timeout_seconds))
        return MessageEntry();

    nsb::nsbm m;
    if (!m.ParseFromString(resp)) return MessageEntry();

    auto op = m.manifest().op();
    if (op != nsb::nsbm::Manifest::SEND &&
        op != nsb::nsbm::Manifest::RECEIVE &&
        op != nsb::nsbm::Manifest::FORWARD &&
        op != nsb::nsbm::Manifest::POST) {
        return MessageEntry();
    }

    if (m.manifest().code() == nsb::nsbm::Manifest::MESSAGE) {
        std::string payload = getPayload(m);
        return MessageEntry(
            m.metadata().src_id(),
            m.metadata().dest_id(),
            payload,
            m.metadata().payload_size());
    }
    return MessageEntry();
}

// ═════════════════════════════════════════════════════════════════════════════
// NSBSimClient
// ═════════════════════════════════════════════════════════════════════════════

NSBSimClient::NSBSimClient(const std::string& id, const std::string& host,
                           int port, Backend backend)
    : NSBClient(id, host, port, backend, nsb::nsbm::Manifest::SIM_CLIENT) {
    initialize();
}

MessageEntry NSBSimClient::fetch(int timeout_seconds,
                                 const std::string& src_id) {
    // In PULL mode with socket backend, send a FETCH request first
    if (backend_ == Backend::SOCKET &&
        cfg_.sys_mode == Config::SystemMode::PULL) {
        auto req = buildBaseMsg(nsb::nsbm::Manifest::FETCH);
        req.mutable_manifest()->set_code(nsb::nsbm::Manifest::SUCCESS);
        if (!src_id.empty()) {
            req.mutable_metadata()->set_src_id(src_id);
        } else if (cfg_.sim_mode == Config::SimulatorMode::PER_NODE) {
            req.mutable_metadata()->set_src_id(id_);
        }
        // SYSTEM_WIDE with no src_id: leave src_id unset
        std::string wire;
        req.SerializeToString(&wire);
        comms_->sendMsg(Channel::RECV, wire);
    } else if (backend_ == Backend::SOCKET &&
               cfg_.sys_mode == Config::SystemMode::PUSH) {
        timeout_seconds = 0;
    }

    std::string resp;
    if (!comms_->recvMsg(Channel::RECV, resp, timeout_seconds))
        return MessageEntry();

    nsb::nsbm m;
    if (!m.ParseFromString(resp)) return MessageEntry();

    auto op = m.manifest().op();
    if (op != nsb::nsbm::Manifest::SEND &&
        op != nsb::nsbm::Manifest::FETCH &&
        op != nsb::nsbm::Manifest::FORWARD) {
        return MessageEntry();
    }

    if (m.manifest().code() == nsb::nsbm::Manifest::MESSAGE) {
        std::string payload = getPayload(m);
        return MessageEntry(
            m.metadata().src_id(),
            m.metadata().dest_id(),
            payload,
            m.metadata().payload_size());
    }
    return MessageEntry();
}

std::string NSBSimClient::post(const std::string& src_id,
                               const std::string& dest_id,
                               const std::string& payload) {
    auto msg = buildBaseMsg(nsb::nsbm::Manifest::POST);
    msg.mutable_manifest()->set_code(nsb::nsbm::Manifest::MESSAGE);
    auto* meta = msg.mutable_metadata();
    meta->set_src_id(src_id);
    meta->set_dest_id(dest_id);
    meta->set_payload_size(static_cast<int>(payload.size()));

    setPayload(msg, payload);

    std::string wire;
    msg.SerializeToString(&wire);
    comms_->sendMsg(Channel::SEND, wire, dest_id);

    return cfg_.use_db ? msg.msg_key() : "";
}

} // namespace nsb_unified
