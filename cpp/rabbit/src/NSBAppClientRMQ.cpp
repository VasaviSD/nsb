#include "NSBAppClientRMQ.hpp"

namespace nsb {

void NSBAppClientRMQ::send(const std::string& dest_id, const std::string& payload) {
    nsb::nsbm msg;
    msg.mutable_manifest()->set_op(nsb::nsbm::Manifest::SEND);
    msg.mutable_manifest()->set_og(nsb::nsbm::Manifest::APP_CLIENT);
    msg.mutable_manifest()->set_code(nsb::nsbm::Manifest::MESSAGE);
    auto meta = msg.mutable_metadata();
    meta->set_src_id(id_);
    meta->set_dest_id(dest_id);
    meta->set_payload_size(static_cast<int>(payload.size()));
    msg.set_payload(payload);

    std::string wire;
    msg.SerializeToString(&wire);
    comms_->send(ChannelKind::SEND, wire, dest_id);
}

bool NSBAppClientRMQ::receive(MessageEntry& out, int timeout_seconds) {
    std::string wire;
    if (!comms_->recv(ChannelKind::RECV, wire, timeout_seconds))
        return false;
    nsb::nsbm m;
    if (!m.ParseFromString(wire))
        return false;
    if (m.manifest().code() != nsb::nsbm::Manifest::MESSAGE)
        return false;
    out.src_id  = m.metadata().src_id();
    out.dest_id = m.metadata().dest_id();
    if (m.has_payload())
        out.payload = m.payload();
    return true;
}

void NSBAppClientRMQ::listen(MessageCallback callback) {
    comms_->async_listen(ChannelKind::RECV, [callback](const std::string& wire) {
        nsb::nsbm m;
        if (!m.ParseFromString(wire)) return;
        if (m.manifest().code() != nsb::nsbm::Manifest::MESSAGE) return;
        MessageEntry entry;
        entry.src_id  = m.metadata().src_id();
        entry.dest_id = m.metadata().dest_id();
        if (m.has_payload())
            entry.payload = m.payload();
        callback(entry);
    });
}

void NSBAppClientRMQ::stop_listen() {
    comms_->stop_listen();
}

bool NSBAppClientRMQ::is_listening() const {
    return comms_->is_listening();
}

}
