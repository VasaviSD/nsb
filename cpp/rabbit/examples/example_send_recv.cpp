#include <iostream>
#include "NSBAppClientRMQ.hpp"
#include "NSBSimClientRMQ.hpp"

int main() {
    try {
        const std::string host = "localhost";
        const int         port = 5672;

        nsb::NSBAppClientRMQ sender("cpp_sender", host, port);
        nsb::NSBAppClientRMQ receiver("cpp_receiver", host, port);

        sender.initialize();
        receiver.initialize();

        std::string payload = "Hello from cpp_sender";
        sender.send("cpp_receiver", payload);
        std::cout << "Sent: " << payload << std::endl;

        nsb::MessageEntry msg;
        if (receiver.receive(msg, 5)) {
            std::cout << "Received from " << msg.src_id << ": " << msg.payload << std::endl;
            return 0;
        } else {
            std::cerr << "Timeout waiting for message" << std::endl;
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
