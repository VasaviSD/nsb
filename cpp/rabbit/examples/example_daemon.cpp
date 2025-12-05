#include <iostream>
#include <csignal>
#include <atomic>
#include "NSBDaemonRMQ.hpp"

static std::atomic<bool> g_running{true};

void handle_sigint(int) {
    std::cout << "\nStopping C++ NSB Daemon..." << std::endl;
    g_running = false;
}

int main() {
    const std::string host = "localhost";
    const int port = 5672;

    nsb::NSBDaemonRMQ daemon(host, port);

    // Default config: PULL, SYSTEM_WIDE, no DB
    // sys mode, sim mode, use db
    daemon.set_config(0, 0, false);

    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);

    try {
        daemon.start();
        std::cout << "C++ NSB Daemon started. Listening for client requests... (Ctrl+C to stop)" << std::endl;
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        daemon.stop();
    } catch (const std::exception& e) {
        std::cerr << "Daemon error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
