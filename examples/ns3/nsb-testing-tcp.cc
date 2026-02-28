/* SPDX-License-Identifier: GPL-2.0-only */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/aodv-module.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <map>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <signal.h>
#include <sys/stat.h>
#include "nsb_client.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("NSBTestingTCP");

// ==================== CONFIGURATION CONSTANTS ====================
// NSB Daemon Mode: "PULL" or "PUSH"
const std::string NSB_DAEMON_MODE = "PULL";  // Change to "PULL" for polling mode

const int DEFAULT_NUM_HOSTS = 10;
const int TCP_PORT = 5000;
const int NSB_DAEMON_PORT = 65432;
const std::string NSB_DAEMON_ADDRESS = "127.0.0.1";
const std::string BASE_IP_PREFIX = "10.1.1.";

// Mode-specific settings
// Use a small blocking timeout so we don't spin and we still wait long enough
// for the daemon to respond in both PUSH and PULL modes.
const int ASYNC_FETCH_TIMEOUT_SEC = 0;
const int QUEUE_CHECK_INTERVAL_MS = 100;      // For PUSH mode async queue check
const double POLL_INTERVAL_SEC = 2.0;         // For PULL mode

const double WIFI_RANGE_METERS = 500.0;
const double NODE_SPACING_METERS = 50.0;
const int NODES_PER_ROW = 10;

// ==================== GLOBAL STATE ====================
std::shared_ptr<nsb::NSBSimClient> simClient;
std::mutex simClientMutex;

// Async listener state
std::queue<nsb::MessageEntry> messageQueue;
std::mutex queueMutex;
std::atomic<bool> listenerRunning(false);
std::thread listenerThread;

// TCP transfer tracking
struct PendingTransfer {
    std::string srcId;
    std::string destId;
    std::string payload;
    uint32_t bytesSent;
    Ptr<Socket> socket;
    PendingTransfer() : bytesSent(0) {}
};
std::map<Ptr<Socket>, PendingTransfer> activeSends;

// TCP receive tracking
struct ReceiveBuffer {
    std::string srcId;
    std::string destId;
    std::string data;
};
std::map<Ptr<Socket>, ReceiveBuffer> activeReceives;

// Node configuration
struct NodeMapping {
    std::string ipString;
    std::string nodeIdString;
    Ipv4Address ipv4Addr;
    Ptr<Node> nodePtr;
    uint32_t wifiDeviceIndex;
};

// ==================== ASYNC LISTENER ====================
void AsyncListenerThread() {
    std::cout << "[ASYNC-LISTENER] Started background thread (blocking timeout: " 
              << ASYNC_FETCH_TIMEOUT_SEC << "s)" << std::endl;
    
    while (listenerRunning) {
        try {
            int fetchTimeout = (NSB_DAEMON_MODE == "PULL") ? DAEMON_RESPONSE_TIMEOUT
                                                           : ASYNC_FETCH_TIMEOUT_SEC;
            nsb::MessageEntry entry;
            {
                std::lock_guard<std::mutex> lock(simClientMutex);
                entry = simClient->fetch(nullptr, fetchTimeout);
            }
            
            if (entry.exists()) {
                std::lock_guard<std::mutex> lock(queueMutex);
                messageQueue.push(entry);
            }
            // If no message after timeout, loop immediately continues - no gaps!
        } catch (const std::exception& e) {
            std::cerr << "[ASYNC-LISTENER] Exception: " << e.what() << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    std::cout << "[ASYNC-LISTENER] Stopped" << std::endl;
}

void StartAsyncListener() {
    if (!listenerRunning) {
        listenerRunning = true;
        listenerThread = std::thread(AsyncListenerThread);
        std::cout << "[ASYNC-LISTENER] Launched" << std::endl;
    }
}

void StopAsyncListener() {
    if (listenerRunning) {
        listenerRunning = false;
        if (listenerThread.joinable()) {
            listenerThread.join();
        }
        std::cout << "[ASYNC-LISTENER] Stopped" << std::endl;
    }
}

void SignalHandler(int signum) {
    std::cout << "\n[SIGNAL] Interrupt (" << signum << ") - shutting down..." << std::endl;
    StopAsyncListener();
    Simulator::Stop();
    exit(signum);
}

// ==================== TCP SEND ====================
void WriteUntilBufferFull(Ptr<Socket> socket, uint32_t txSpace) {
    if (activeSends.find(socket) == activeSends.end()) {
        return;
    }
    
    PendingTransfer& transfer = activeSends[socket];
    
    while (transfer.bytesSent < transfer.payload.size() && socket->GetTxAvailable() > 0) {
        uint32_t remaining = transfer.payload.size() - transfer.bytesSent;
        uint32_t toSend = std::min(remaining, socket->GetTxAvailable());
        
        Ptr<Packet> pkt = Create<Packet>(
            (uint8_t*)(transfer.payload.c_str() + transfer.bytesSent), 
            toSend
        );
        
        int sent = socket->Send(pkt);
        if (sent < 0) return;
        
        transfer.bytesSent += sent;
    }
    
    if (transfer.bytesSent >= transfer.payload.size()) {
        std::cout << "[TCP-SEND] Complete: " << transfer.bytesSent << " bytes from " 
                  << transfer.srcId << " to " << transfer.destId << std::endl;
        
        activeSends.erase(socket);
        socket->Close();
    }
}

// ==================== PULL MODE: Polling ====================
void PollToFetch() {
    nsb::MessageEntry entry;
    
    try {
        std::lock_guard<std::mutex> lock(simClientMutex);
        entry = simClient->fetch();
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Exception during fetch: " << e.what() << std::endl;
        Simulator::Schedule(Seconds(POLL_INTERVAL_SEC), &PollToFetch);
        return;
    }
    
    if (!entry.exists()) {
        Simulator::Schedule(Seconds(POLL_INTERVAL_SEC), &PollToFetch);
        return;
    }

    std::string srcId = entry.source;
    std::string destId = entry.destination;
    std::string payload = entry.payload_obj;
    
    std::cout << "\n[POLL-FETCH] Got message:" << std::endl;
    std::cout << "  Source: " << srcId << std::endl;
    std::cout << "  Destination: " << destId << std::endl;
    std::cout << "  Payload size: " << payload.size() << " bytes" << std::endl;

    if (srcId.empty() || destId.empty()) {
        std::cerr << "[ERROR] Invalid message: empty srcId or destId" << std::endl;
        Simulator::Schedule(Seconds(POLL_INTERVAL_SEC), &PollToFetch);
        return;
    }
    
    if (srcId.substr(0, 4) != "host" || destId.substr(0, 4) != "host") {
        std::cerr << "[ERROR] Invalid ID format. Expected 'hostX'" << std::endl;
        Simulator::Schedule(Seconds(POLL_INTERVAL_SEC), &PollToFetch);
        return;
    }

    try {
        int destIndex = std::stoi(destId.substr(4));
        std::string destIpStr = BASE_IP_PREFIX + std::to_string(destIndex + 1);
        Ipv4Address destAddr(destIpStr.c_str());

        uint32_t srcIndex = std::stoi(srcId.substr(4));
        Ptr<Node> srcNode = NodeList::GetNode(srcIndex);

        Ptr<Socket> socket = Socket::CreateSocket(srcNode, TcpSocketFactory::GetTypeId());
        InetSocketAddress remote = InetSocketAddress(destAddr, TCP_PORT);
        
        int connectResult = socket->Connect(remote);
        if (connectResult == -1) {
            std::cerr << "[ERROR] TCP Connect failed from " << srcId << " to " << destId << std::endl;
            socket->Close();
        } else {
            PendingTransfer transfer;
            transfer.srcId = srcId;
            transfer.destId = destId;
            transfer.payload = payload;
            transfer.bytesSent = 0;
            transfer.socket = socket;
            activeSends[socket] = transfer;
            
            socket->SetSendCallback(MakeCallback(&WriteUntilBufferFull));
            
            std::cout << "[TCP-SEND] Starting transfer of " << payload.size() 
                      << " bytes (TCP chunked)" << std::endl;
            WriteUntilBufferFull(socket, socket->GetTxAvailable());
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to parse/send: " << e.what() << std::endl;
    }

    Simulator::Schedule(Seconds(POLL_INTERVAL_SEC), &PollToFetch);
}

// ==================== PUSH MODE: Queue Processor ====================
void ProcessQueuedMessages() {
    nsb::MessageEntry entry;
    bool hasMessage = false;
    
    while (true) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!messageQueue.empty()) {
                entry = messageQueue.front();
                messageQueue.pop();
                hasMessage = true;
            } else {
                hasMessage = false;
            }
        }

        if (!hasMessage) {
            break;
        }

        std::string srcId = entry.source;
        std::string destId = entry.destination;
        std::string payload = entry.payload_obj;
        
        std::cout << "\n[PROCESS] Message from queue:" << std::endl;
        std::cout << "  Source: " << srcId << std::endl;
        std::cout << "  Destination: " << destId << std::endl;
        std::cout << "  Payload size: " << payload.size() << " bytes" << std::endl;

        if (srcId.empty() || destId.empty()) {
            std::cerr << "[ERROR] Invalid message: empty srcId or destId" << std::endl;
        } else if (srcId.substr(0, 4) != "host" || destId.substr(0, 4) != "host") {
            std::cerr << "[ERROR] Invalid ID format. Expected 'hostX'" << std::endl;
        } else {
            try {
                int destIndex = std::stoi(destId.substr(4));
                std::string destIpStr = BASE_IP_PREFIX + std::to_string(destIndex + 1);
                Ipv4Address destAddr(destIpStr.c_str());

                uint32_t srcIndex = std::stoi(srcId.substr(4));
                Ptr<Node> srcNode = NodeList::GetNode(srcIndex);

                Ptr<Socket> socket = Socket::CreateSocket(srcNode, TcpSocketFactory::GetTypeId());
                InetSocketAddress remote = InetSocketAddress(destAddr, TCP_PORT);
                
                int connectResult = socket->Connect(remote);
                if (connectResult == -1) {
                    std::cerr << "[ERROR] TCP Connect failed from " << srcId << " to " << destId << std::endl;
                    socket->Close();
                } else {
                    PendingTransfer transfer;
                    transfer.srcId = srcId;
                    transfer.destId = destId;
                    transfer.payload = payload;
                    transfer.bytesSent = 0;
                    transfer.socket = socket;
                    activeSends[socket] = transfer;
                    
                    socket->SetSendCallback(MakeCallback(&WriteUntilBufferFull));
                    
                    std::cout << "[TCP-SEND] Starting transfer of " << payload.size() 
                              << " bytes (TCP chunked)" << std::endl;
                    WriteUntilBufferFull(socket, socket->GetTxAvailable());
                }
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] Failed to parse/send: " << e.what() << std::endl;
            }
        }
    }

    Simulator::Schedule(MilliSeconds(QUEUE_CHECK_INTERVAL_MS), &ProcessQueuedMessages);
}

// ==================== TCP RECEIVE ====================
void TcpReceiveCallback(Ptr<Socket> socket) {
    Ptr<Packet> pkt;
    Address from;
    
    while ((pkt = socket->RecvFrom(from))) {
        if (pkt->GetSize() == 0) break;
        
        if (activeReceives.find(socket) == activeReceives.end()) {
            Ptr<Node> thisNode = socket->GetNode();
            uint32_t nodeId = thisNode->GetId();
            std::string destId = "host" + std::to_string(nodeId);
            
            InetSocketAddress srcSockAddr = InetSocketAddress::ConvertFrom(from);
            std::ostringstream oss;
            oss << srcSockAddr.GetIpv4();
            std::string srcIpStr = oss.str();
            
            size_t lastDot = srcIpStr.find_last_of('.');
            if (lastDot == std::string::npos) {
                std::cerr << "[ERROR] Invalid source IP format" << std::endl;
                return;
            }
            
            int srcIndex = std::stoi(srcIpStr.substr(lastDot + 1)) - 1;
            std::string srcId = "host" + std::to_string(srcIndex);
            
            ReceiveBuffer buf;
            buf.srcId = srcId;
            buf.destId = destId;
            buf.data = "";
            activeReceives[socket] = buf;
            
            std::cout << "[TCP-RECV] Started receiving from " << srcId << " to " << destId << std::endl;
        }
        
        ReceiveBuffer& buf = activeReceives[socket];
        uint32_t packetSize = pkt->GetSize();
        std::vector<uint8_t> data(packetSize);
        pkt->CopyData(data.data(), packetSize);
        buf.data.append(data.begin(), data.end());
    }
}

void TcpCloseCallback(Ptr<Socket> socket) {
    if (activeReceives.find(socket) == activeReceives.end()) {
        return;
    }
    
    ReceiveBuffer& buf = activeReceives[socket];
    
    std::cout << "[TCP-RECV] Complete: " << buf.data.size() << " bytes from " 
              << buf.srcId << " to " << buf.destId << std::endl;
    
    try {
        {
            std::lock_guard<std::mutex> lock(simClientMutex);
            simClient->post(buf.srcId, buf.destId, buf.data);
        }
        std::cout << "[POST] Successfully posted to NSB from " << buf.srcId 
                  << " to " << buf.destId << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to post to NSB: " << e.what() << std::endl;
    }
    
    activeReceives.erase(socket);
}

bool TcpAcceptCallback(Ptr<Socket> socket, const Address& from) {
    return true;
}

void TcpNewConnectionCallback(Ptr<Socket> connSocket, const Address& from) {
    connSocket->SetRecvCallback(MakeCallback(&TcpReceiveCallback));
    connSocket->SetCloseCallbacks(
        MakeCallback(&TcpCloseCallback),
        MakeCallback(&TcpCloseCallback)
    );
}

// ==================== NETWORK SETUP ====================
void CreateLogDirectory(const std::string& path) {
    mkdir(path.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
}

bool BuildNetworkTopology(const std::vector<NodeMapping>& nodeMappings,
                          NodeContainer& allSimNodes,
                          NetDeviceContainer& wifiDevices,
                          Ipv4InterfaceContainer& allInterfaces) {
    if (nodeMappings.empty()) return false;
    
    allSimNodes.Create(nodeMappings.size());

    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channel.AddPropagationLoss("ns3::RangePropagationLossModel", "MaxRange", DoubleValue(WIFI_RANGE_METERS));
    Ptr<YansWifiChannel> wifiChannel = channel.Create();

    YansWifiPhyHelper phy;
    phy.SetChannel(wifiChannel);

    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211b);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode", StringValue("DsssRate1Mbps"),
                                 "ControlMode", StringValue("DsssRate1Mbps"));

    wifiDevices = wifi.Install(phy, mac, allSimNodes);

    MobilityHelper mobility;
    Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator>();
    for (size_t i = 0; i < nodeMappings.size(); ++i) {
        double x = (i % NODES_PER_ROW) * NODE_SPACING_METERS;
        double y = floor(i / NODES_PER_ROW) * NODE_SPACING_METERS;
        positionAlloc->Add(Vector(x, y, 0.0));
    }
    mobility.SetPositionAllocator(positionAlloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(allSimNodes);

    InternetStackHelper stack;
    AodvHelper aodv;
    stack.SetRoutingHelper(aodv);
    stack.Install(allSimNodes);

    for (size_t i = 0; i < nodeMappings.size(); ++i) {
        Ptr<Ipv4> ipv4 = allSimNodes.Get(i)->GetObject<Ipv4>();
        uint32_t interfaceIndex = ipv4->AddInterface(wifiDevices.Get(i));
        Ipv4InterfaceAddress ipv4IfAddr(nodeMappings[i].ipv4Addr, Ipv4Mask("255.255.255.0"));
        ipv4->AddAddress(interfaceIndex, ipv4IfAddr);
        ipv4->SetMetric(interfaceIndex, 1);
        ipv4->SetUp(interfaceIndex);
        allInterfaces.Add(ipv4, interfaceIndex);
    }
    
    return true;
}

void SetupTcpReceiveSockets(const std::vector<NodeMapping>& nodeMappings, 
                            const NodeContainer& allSimNodes) {
    for (size_t i = 0; i < nodeMappings.size(); ++i) {
        Ptr<Socket> listenSocket = Socket::CreateSocket(allSimNodes.Get(i), TcpSocketFactory::GetTypeId());
        InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), TCP_PORT);
        listenSocket->Bind(local);
        listenSocket->Listen();
        listenSocket->SetAcceptCallback(
            MakeCallback(&TcpAcceptCallback),
            MakeCallback(&TcpNewConnectionCallback)
        );
    }
    std::cout << "[TCP-SETUP] All nodes listening on port " << TCP_PORT << std::endl;
}

// ==================== MAIN ====================
int main(int argc, char* argv[]) {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    
    int numHosts = DEFAULT_NUM_HOSTS;

    CommandLine cmd(__FILE__);
    cmd.AddValue("numHosts", "Number of simulated NS-3 hosts", numHosts);
    cmd.Parse(argc, argv);

    Time::SetResolution(Time::NS);

    CreateLogDirectory("log");
    
    std::string serverAddr = NSB_DAEMON_ADDRESS;
    simClient = std::make_shared<nsb::NSBSimClient>(
        "simulator", 
        serverAddr, 
        NSB_DAEMON_PORT
    );

    std::vector<NodeMapping> nodeMappings;
    for (int i = 0; i < numHosts; ++i) {
        NodeMapping mapping;
        mapping.ipString = BASE_IP_PREFIX + std::to_string(i + 1);
        mapping.nodeIdString = "node" + std::to_string(i);
        mapping.ipv4Addr = Ipv4Address(mapping.ipString.c_str());
        nodeMappings.push_back(mapping);
    }

    NodeContainer allSimNodes;
    NetDeviceContainer wifiDevices;
    Ipv4InterfaceContainer allInterfaces;

    if (!BuildNetworkTopology(nodeMappings, allSimNodes, wifiDevices, allInterfaces)) {
        std::cerr << "FATAL: Failed to build network topology" << std::endl;
        return 1;
    }

    for (size_t i = 0; i < nodeMappings.size(); ++i) {
        nodeMappings[i].nodePtr = allSimNodes.Get(i);
        nodeMappings[i].wifiDeviceIndex = i;
    }

    SetupTcpReceiveSockets(nodeMappings, allSimNodes);
    
    std::cout << "\n========================================================" << std::endl;
    std::cout << "  NS-3 NSB TCP Testing" << std::endl;
    std::cout << "  Mode: " << NSB_DAEMON_MODE << std::endl;
    std::cout << "  Nodes: " << numHosts << std::endl;
    std::cout << "  TCP Port: " << TCP_PORT << std::endl;
    std::cout << "  NSB Daemon: " << NSB_DAEMON_ADDRESS << ":" << NSB_DAEMON_PORT << std::endl;
    std::cout << "========================================================\n" << std::endl;
    
    // Start appropriate listener based on mode
    if (NSB_DAEMON_MODE == "PUSH") {
        std::cout << "[MODE] Using PUSH mode with async listener (continuous)" << std::endl;
        StartAsyncListener();
        Simulator::Schedule(MilliSeconds(QUEUE_CHECK_INTERVAL_MS), &ProcessQueuedMessages);
    } else if (NSB_DAEMON_MODE == "PULL") {
        std::cout << "[MODE] Using PULL mode with polling (interval: " << POLL_INTERVAL_SEC << "s)" << std::endl;
        Simulator::Schedule(Seconds(POLL_INTERVAL_SEC), &PollToFetch);
    } else {
        std::cerr << "[ERROR] Invalid NSB_DAEMON_MODE: " << NSB_DAEMON_MODE 
                  << " (must be 'PULL' or 'PUSH')" << std::endl;
        return 1;
    }

    Simulator::Run();
    
    std::cout << "\n[CLEANUP] Shutting down..." << std::endl;
    if (NSB_DAEMON_MODE == "PUSH") {
        StopAsyncListener();
    }
    Simulator::Destroy();
    
    return 0;
}
