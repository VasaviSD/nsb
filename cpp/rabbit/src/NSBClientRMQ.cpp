#include "NSBClientRMQ.hpp"
#include <stdexcept>
#include <iostream>

namespace nsb {

NSBClientRMQ::NSBClientRMQ(std::string id, std::string host, int port, Originator og)
    : id_(std::move(id)), host_(std::move(host)), port_(port), og_(og) {
    comms_ = std::make_unique<RabbitMQInterface>(host_, port_, id_);
    comms_->connect(10);
}

NSBClientRMQ::~NSBClientRMQ() {
    try {
        exit();
    } catch (...) {
    }
}

nsb::nsbm NSBClientRMQ::buildBaseMsg(nsb::nsbm::Manifest::Operation op) const {
    nsb::nsbm msg;
    msg.mutable_manifest()->set_op(op);
    msg.mutable_manifest()->set_og(
        og_ == Originator::APP_CLIENT ? nsb::nsbm::Manifest::APP_CLIENT : nsb::nsbm::Manifest::SIM_CLIENT);
    return msg;
}

void NSBClientRMQ::initialize(int timeout_sec) {
    // Build INIT request
    auto msg = buildBaseMsg(nsb::nsbm::Manifest::INIT);
    msg.mutable_manifest()->set_code(nsb::nsbm::Manifest::CLIENT_REQUEST);
    auto intro = msg.mutable_intro();
    intro->set_identifier(id_);
    intro->set_address(host_);
    intro->set_ch_ctrl(0);
    intro->set_ch_send(1);
    intro->set_ch_recv(2);

    std::string wire;
    msg.SerializeToString(&wire);
    comms_->send(ChannelKind::CTRL, wire);

    // Receive INIT response on CTRL queue
    std::string resp;
    if (!comms_->recv(ChannelKind::CTRL, resp, timeout_sec)) {
        throw std::runtime_error("NSB INIT timeout");
    }
    nsb::nsbm m;
    if (!m.ParseFromString(resp))
        throw std::runtime_error("NSB INIT parse error");
    if (m.manifest().op() != nsb::nsbm::Manifest::INIT ||
        m.manifest().code() != nsb::nsbm::Manifest::DAEMON_RESPONSE) {
        throw std::runtime_error("NSB INIT invalid response");
    }
    // Fill config
    cfg_.sys_mode   = (m.config().sys_mode() == nsb::nsbm_ConfigParams_SystemMode_PULL)
                          ? Config::SystemMode::PULL
                          : Config::SystemMode::PUSH;
    cfg_.sim_mode   = (m.config().sim_mode() == nsb::nsbm_ConfigParams_SimulatorMode_SYSTEM_WIDE)
                          ? Config::SimulatorMode::SYSTEM_WIDE
                          : Config::SimulatorMode::PER_NODE;
    cfg_.use_db     = m.config().use_db();
    cfg_.db_address = m.config().db_address();
    cfg_.db_port    = m.config().db_port();
    cfg_.db_num     = m.config().db_num();
}

void NSBClientRMQ::ping() {
    auto msg = buildBaseMsg(nsb::nsbm::Manifest::PING);
    msg.mutable_manifest()->set_code(nsb::nsbm::Manifest::CLIENT_REQUEST);
    std::string wire;
    msg.SerializeToString(&wire);
    comms_->send(ChannelKind::CTRL, wire);
}

void NSBClientRMQ::exit() {
    if (!comms_)
        return;
    auto msg = buildBaseMsg(nsb::nsbm::Manifest::EXIT);
    msg.mutable_manifest()->set_code(nsb::nsbm::Manifest::CLIENT_REQUEST);
    std::string wire;
    msg.SerializeToString(&wire);
    comms_->send(ChannelKind::CTRL, wire);
    comms_->close();
}

}
