/**
 * @file NSBUnified.hpp
 * @brief Unified NSB Client with Socket and RabbitMQ Backend Support
 * @details Provides a unified interface for NSB clients that can use either
 *          socket-based or RabbitMQ-based communication backends. The backend
 *          is selected at runtime via a parameter, allowing seamless switching
 *          between transport mechanisms without code changes.
 */

#pragma once

#include <string>
#include <memory>
#include <functional>
#include <future>
#include <iostream>
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>
#include <map>
#include <vector>

#include "nsb.pb.h"

#define NSB_UNIFIED_SERVER_CONNECTION_TIMEOUT 10
#define NSB_UNIFIED_DAEMON_RESPONSE_TIMEOUT 30
#define NSB_UNIFIED_RECEIVE_BUFFER_SIZE 4096
#define NSB_UNIFIED_SEND_BUFFER_SIZE 4096

namespace nsb_unified {

// ─────────────────────────────────────────────────────────────────────────────
// Backend selection
// ─────────────────────────────────────────────────────────────────────────────

/** @brief Selects the communication backend at runtime. */
enum class Backend {
    SOCKET,
    RABBITMQ
};

// ─────────────────────────────────────────────────────────────────────────────
// Shared types
// ─────────────────────────────────────────────────────────────────────────────

/** @brief Communication channel identifiers. */
enum class Channel {
    CTRL = 0,
    SEND = 1,
    RECV = 2
};

/** @brief System configuration received from the daemon during INIT. */
struct Config {
    enum class SystemMode {
        PULL = 0,
        PUSH = 1
    };
    enum class SimulatorMode {
        SYSTEM_WIDE = 0,
        PER_NODE = 1
    };
    SystemMode    sys_mode{SystemMode::PULL};
    SimulatorMode sim_mode{SimulatorMode::SYSTEM_WIDE};
    bool          use_db{false};
    std::string   db_address;
    int           db_port{0};
    int           db_num{0};

    /** @brief Default constructor. */
    Config() = default;

    /** @brief Construct from a protobuf INIT response. */
    explicit Config(const nsb::nsbm& msg) {
        const auto& c = msg.config();
        sys_mode   = static_cast<SystemMode>(c.sys_mode());
        sim_mode   = static_cast<SimulatorMode>(c.sim_mode());
        use_db     = c.use_db();
        db_address = c.db_address();
        db_port    = c.db_port();
        db_num     = c.db_num();
    }
};

/** @brief Container for received/fetched messages. */
struct MessageEntry {
    std::string src_id;
    std::string dest_id;
    std::string payload;
    int         payload_size{0};

    /** @brief Default constructor (empty entry). */
    MessageEntry() = default;

    /** @brief Populated constructor. */
    MessageEntry(std::string src, std::string dest, std::string data, int size)
        : src_id(std::move(src)), dest_id(std::move(dest)),
          payload(std::move(data)), payload_size(size) {}

    /** @brief Returns true if the entry contains valid data. */
    bool exists() const {
        return !src_id.empty() && !dest_id.empty() && !payload.empty();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Abstract communication interface
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Abstract base for communication backends.
 *
 * Both SocketBackend and RabbitMQBackend implement this interface so that
 * client code is backend-agnostic.
 */
class IComms {
  public:
    virtual ~IComms() = default;

    /**
     * @brief Send serialized bytes on the given channel.
     * @param ch      Channel to send on.
     * @param bytes   Serialized message.
     * @param dest_id Destination client ID (used by RabbitMQ for routing).
     */
    virtual void sendMsg(Channel ch, const std::string& bytes,
                         const std::string& dest_id = "") = 0;

    /**
     * @brief Receive a message from the given channel.
     * @param ch              Channel to receive on.
     * @param[out] out        Received bytes.
     * @param timeout_seconds Timeout: <0 = wait forever, 0 = poll, >0 = wait.
     * @return true if a message was received, false on timeout/error.
     */
    virtual bool recvMsg(Channel ch, std::string& out,
                         int timeout_seconds = -1) = 0;

    /** @brief Close the connection / release resources. */
    virtual void close() = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Socket backend
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Socket-based communication backend.
 *
 * Wraps raw TCP sockets with three channels (CTRL, SEND, RECV) connected to
 * the NSB daemon.
 */
class SocketBackend : public IComms {
  public:
    SocketBackend(const std::string& host, int port);
    ~SocketBackend() override;

    void sendMsg(Channel ch, const std::string& bytes,
                 const std::string& dest_id = "") override;
    bool recvMsg(Channel ch, std::string& out,
                 int timeout_seconds = -1) override;
    void close() override;

    /** @brief Access the raw socket fd for a channel (needed by initialize). */
    int fd(Channel ch) const;

  private:
    std::string host_;
    int         port_;
    std::map<Channel, int> conns_;
    bool closed_{false};

    void connect(int timeout_seconds);
};

// ─────────────────────────────────────────────────────────────────────────────
// RabbitMQ backend
// ─────────────────────────────────────────────────────────────────────────────

// Forward-declare to avoid pulling SimpleAmqpClient into every translation unit
// that includes this header. The implementation file includes the real header.
class RabbitMQBackendImpl;

/**
 * @brief RabbitMQ-based communication backend (pimpl wrapper).
 *
 * Uses SimpleAmqpClient under the hood. The implementation is hidden behind
 * a pimpl to keep the header free of SimpleAmqpClient includes.
 */
class RabbitMQBackend : public IComms {
  public:
    RabbitMQBackend(const std::string& host, int port,
                    const std::string& client_id);
    ~RabbitMQBackend() override;

    void sendMsg(Channel ch, const std::string& bytes,
                 const std::string& dest_id = "") override;
    bool recvMsg(Channel ch, std::string& out,
                 int timeout_seconds = -1) override;
    void close() override;

  private:
    std::unique_ptr<RabbitMQBackendImpl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Redis connector (optional, for large payloads)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Redis database connector for payload offloading.
 */
class RedisConnector {
  public:
    RedisConnector(const std::string& client_id,
                   const std::string& address, int port);
    ~RedisConnector();

    bool        isConnected() const;
    std::string store(const std::string& value);
    std::string checkOut(const std::string& key);
    std::string peek(const std::string& key);

  private:
    std::string client_id_;
    std::string address_;
    int         port_;
    void*       context_{nullptr};   // redisContext*, kept as void* to avoid hiredis include
    int         counter_{0};
    std::mutex  mtx_;

    bool        doConnect();
    void        doDisconnect();
    std::string generateKey();
};

// ─────────────────────────────────────────────────────────────────────────────
// NSBClient (base)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Base class for unified NSB clients.
 *
 * Handles INIT, PING, EXIT, and backend/config management. Subclasses add
 * role-specific methods (send/receive or fetch/post).
 */
class NSBClient {
  public:
    NSBClient(const std::string& id, const std::string& host, int port,
              Backend backend, nsb::nsbm::Manifest::Originator og);
    virtual ~NSBClient();

    /** @brief Perform INIT handshake with the daemon. */
    void initialize(int timeout_sec = NSB_UNIFIED_DAEMON_RESPONSE_TIMEOUT);

    /** @brief Ping the daemon. Returns true if healthy. */
    bool ping(int timeout_sec = NSB_UNIFIED_DAEMON_RESPONSE_TIMEOUT);

    /** @brief Send EXIT to the daemon and close the connection. */
    void exit();

    const Config&      config() const { return cfg_; }
    const std::string& id() const     { return id_; }
    Backend            backend() const { return backend_; }

  protected:
    std::string                        id_;
    std::string                        host_;
    int                                port_;
    Backend                            backend_;
    nsb::nsbm::Manifest::Originator   og_;
    Config                             cfg_;
    std::unique_ptr<IComms>            comms_;
    std::unique_ptr<RedisConnector>    db_;

    /** @brief Build a protobuf message with manifest fields pre-filled. */
    nsb::nsbm buildBaseMsg(nsb::nsbm::Manifest::Operation op) const;

    /** @brief Get or store payload depending on use_db config. */
    std::string getPayload(const nsb::nsbm& msg) const;
    void        setPayload(nsb::nsbm& msg, const std::string& payload);
};

// ─────────────────────────────────────────────────────────────────────────────
// NSBAppClient
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Application client: send and receive payloads.
 */
class NSBAppClient : public NSBClient {
  public:
    using MessageCallback = std::function<void(const MessageEntry&)>;

    /**
     * @brief Construct and initialize an application client.
     * @param id      Unique client identifier.
     * @param host    Daemon/broker address.
     * @param port    Daemon/broker port.
     * @param backend Backend selection (SOCKET or RABBITMQ).
     */
    NSBAppClient(const std::string& id, const std::string& host, int port,
                 Backend backend = Backend::SOCKET);

    /** @brief Send a payload to the specified destination. */
    std::string send(const std::string& dest_id, const std::string& payload);

    /**
     * @brief Receive a payload.
     * @param timeout_seconds Timeout in seconds (<0 = forever, 0 = poll).
     * @param dest_id         Optional destination filter (defaults to own id).
     * @return MessageEntry (check exists() for validity).
     */
    MessageEntry receive(int timeout_seconds = NSB_UNIFIED_DAEMON_RESPONSE_TIMEOUT,
                         const std::string& dest_id = "");
};

// ─────────────────────────────────────────────────────────────────────────────
// NSBSimClient
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Simulator client: fetch and post payloads.
 */
class NSBSimClient : public NSBClient {
  public:
    using MessageCallback = std::function<void(const MessageEntry&)>;

    /**
     * @brief Construct and initialize a simulator client.
     * @param id      Unique client identifier.
     * @param host    Daemon/broker address.
     * @param port    Daemon/broker port.
     * @param backend Backend selection (SOCKET or RABBITMQ).
     */
    NSBSimClient(const std::string& id, const std::string& host, int port,
                 Backend backend = Backend::SOCKET);

    /**
     * @brief Fetch a message that needs to traverse the simulated network.
     * @param timeout_seconds Timeout in seconds.
     * @param src_id          Optional source filter.
     * @return MessageEntry (check exists() for validity).
     */
    MessageEntry fetch(int timeout_seconds = NSB_UNIFIED_DAEMON_RESPONSE_TIMEOUT,
                       const std::string& src_id = "");

    /**
     * @brief Post a message that has been transmitted over the simulated network.
     * @param src_id  Source identifier.
     * @param dest_id Destination identifier.
     * @param payload The payload bytes.
     * @return Database key if use_db is enabled, else empty string.
     */
    std::string post(const std::string& src_id, const std::string& dest_id,
                     const std::string& payload);
};

} // namespace nsb_unified
