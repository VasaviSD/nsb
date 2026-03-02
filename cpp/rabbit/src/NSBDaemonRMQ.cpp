#include "NSBDaemonRMQ.hpp"
#include <stdexcept>
#include <iostream>
#include <chrono>
#include <thread>

namespace nsb {

static const std::string kExchange = "nsb";

NSBDaemonRMQ::NSBDaemonRMQ(const std::string& host, int port)
    : host_(host), port_(port) {
    // default config
    cfg_.set_sys_mode(nsb::nsbm_ConfigParams_SystemMode_PULL);
    cfg_.set_sim_mode(nsb::nsbm_ConfigParams_SimulatorMode_SYSTEM_WIDE);
    cfg_.set_use_db(false);
}

NSBDaemonRMQ::~NSBDaemonRMQ() { stop(); }

void NSBDaemonRMQ::set_config(int sys_mode, int sim_mode, bool use_db,
                              const std::string& db_address, int db_port, int db_num) {
    cfg_.set_sys_mode(static_cast<nsb::nsbm_ConfigParams_SystemMode>(sys_mode));
    cfg_.set_sim_mode(static_cast<nsb::nsbm_ConfigParams_SimulatorMode>(sim_mode));
    cfg_.set_use_db(use_db);
    cfg_.set_db_address(db_address);
    cfg_.set_db_port(db_port);
    cfg_.set_db_num(db_num);
}

void NSBDaemonRMQ::start() {
    if (running_)
        return;
    // Connect
    std::string uri = "amqp://guest:guest@" + host_ + ":" + std::to_string(port_);
    chan_           = AmqpClient::Channel::CreateFromUri(uri);
    chan_->DeclareExchange(kExchange, AmqpClient::Channel::EXCHANGE_TYPE_DIRECT, false, true);
    // Ensure request queue exists
    chan_->DeclareQueue("nsb.config.request", false, true, false, false);
    chan_->BindQueue("nsb.config.request", kExchange, "nsb.config.request");

    running_ = true;
    worker_  = std::thread(&NSBDaemonRMQ::run_loop, this);
}

void NSBDaemonRMQ::stop() {
    if (!running_)
        return;
    running_ = false;
    if (worker_.joinable())
        worker_.join();
    if (chan_) {
        try {
            chan_->DeleteQueue("nsb.config.request");
        } catch (const std::exception& e) {
            std::cerr << "Daemon cleanup warning (queue): " << e.what() << std::endl;
        }
        try {
            chan_->DeleteExchange(kExchange);
        } catch (const std::exception& e) {
            std::cerr << "Daemon cleanup warning (exchange): " << e.what() << std::endl;
        }
        chan_.reset();
    }
}

void NSBDaemonRMQ::run_loop() {
    const std::string tag = chan_->BasicConsume("nsb.config.request", "", true, true, false);
    while (running_) {
        AmqpClient::Envelope::ptr_t env;
        // 500ms poll time
        bool ok = chan_->BasicConsumeMessage(tag, env, 500);
        if (!ok || !env)
            continue;
        try {
            handle_message(env);
        } catch (const std::exception& e) {
            std::cerr << "Daemon error: " << e.what() << std::endl;
        }
    }
    try {
        chan_->BasicCancel(tag);
    } catch (...) {
    }
}

void NSBDaemonRMQ::handle_message(const AmqpClient::Envelope::ptr_t& env) {
    nsb::nsbm req;
    if (!req.ParseFromString(env->Message()->Body()))
        return;
    if (!req.has_manifest())
        return;

    switch (req.manifest().op()) {
    case nsb::nsbm::Manifest::INIT:
        reply_init(req);
        break;
    case nsb::nsbm::Manifest::PING:
        reply_ping(req);
        break;
    case nsb::nsbm::Manifest::EXIT:
        break;
    default:
        break;
    }
}

void NSBDaemonRMQ::reply_init(const nsb::nsbm& req) {
    nsb::nsbm resp;
    resp.mutable_manifest()->set_op(nsb::nsbm::Manifest::INIT);
    resp.mutable_manifest()->set_og(nsb::nsbm::Manifest::DAEMON);
    resp.mutable_manifest()->set_code(nsb::nsbm::Manifest::DAEMON_RESPONSE);
    *resp.mutable_config() = cfg_;

    // response routing key
    std::string client_id;
    if (req.has_intro())
        client_id = req.intro().identifier();
    if (client_id.empty())
        return;
    const std::string routing = "nsb.config.response." + client_id;

    std::string wire;
    resp.SerializeToString(&wire);
    auto msg = AmqpClient::BasicMessage::Create(wire);
    msg->DeliveryMode(AmqpClient::BasicMessage::dm_nonpersistent);
    chan_->BasicPublish(kExchange, routing, msg);
}

void NSBDaemonRMQ::reply_ping(const nsb::nsbm& req) {
    nsb::nsbm resp;
    resp.mutable_manifest()->set_op(nsb::nsbm::Manifest::PING);
    resp.mutable_manifest()->set_og(nsb::nsbm::Manifest::DAEMON);
    resp.mutable_manifest()->set_code(nsb::nsbm::Manifest::DAEMON_RESPONSE);

    std::string client_id;
    if (req.has_intro())
        client_id = req.intro().identifier();
    // If no intro, try metadata.src_id as fallback
    if (client_id.empty() && req.has_metadata())
        client_id = req.metadata().src_id();
    if (client_id.empty())
        return;
    const std::string routing = "nsb.config.response." + client_id;

    std::string wire;
    resp.SerializeToString(&wire);
    auto msg = AmqpClient::BasicMessage::Create(wire);
    msg->DeliveryMode(AmqpClient::BasicMessage::dm_nonpersistent);
    chan_->BasicPublish(kExchange, routing, msg);
}

}
