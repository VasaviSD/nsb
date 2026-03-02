/**
 * @file example_unified.cpp
 * @brief Demonstrates the unified NSB client with backend selection.
 *
 * Usage:
 *   ./example_unified              # defaults to RabbitMQ backend
 *   ./example_unified socket       # use socket backend (needs socket daemon)
 *   ./example_unified rabbitmq     # use RabbitMQ backend (needs RabbitMQ daemon)
 */

#include <iostream>
#include <string>
#include "NSBUnified.hpp"

int main(int argc, char* argv[]) {
    // Parse backend from command line
    nsb_unified::Backend backend = nsb_unified::Backend::RABBITMQ;
    std::string host = "localhost";
    int port = 5672;

    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "socket") {
            backend = nsb_unified::Backend::SOCKET;
            port    = 5555;
        } else if (arg == "rabbitmq") {
            backend = nsb_unified::Backend::RABBITMQ;
            port    = 5672;
        } else {
            std::cerr << "Usage: " << argv[0] << " [socket|rabbitmq]" << std::endl;
            return 1;
        }
    }

    std::string backend_name = (backend == nsb_unified::Backend::SOCKET) ? "socket" : "rabbitmq";
    std::cout << "Using " << backend_name << " backend on " << host << ":" << port << std::endl;

    try {
        // Create two app clients with the chosen backend
        nsb_unified::NSBAppClient sender("unified_sender", host, port, backend);
        nsb_unified::NSBAppClient receiver("unified_receiver", host, port, backend);

        std::cout << "Config: sys_mode=" << static_cast<int>(sender.config().sys_mode)
                  << " sim_mode=" << static_cast<int>(sender.config().sim_mode)
                  << " use_db=" << sender.config().use_db << std::endl;

        // Send a message
        std::string payload = "Hello from unified C++ client (" + backend_name + ")";
        sender.send("unified_receiver", payload);
        std::cout << "Sent: " << payload << std::endl;

        // Receive the message
        auto msg = receiver.receive(5);
        if (msg.exists()) {
            std::cout << "Received from " << msg.src_id << ": " << msg.payload << std::endl;
        } else {
            std::cerr << "Timeout waiting for message" << std::endl;
            return 2;
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
