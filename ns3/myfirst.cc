/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/aodv-module.h"
#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
// #include <boost/any.hpp>
#include <vector>
#include <cstddef>
#include <bit>
#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <assert.h>
#include <pthread.h>
#include <chrono>
#include <thread>
#include <cmath>
#include "nsb_payload.h"
#include <sstream>
#include <map>
#include <tuple>
#include <functional> // Added for std::bind if needed, or for general callback patterns
#include <sys/stat.h> // Added for mkdir

// Default Network Topology
//
//       Wireless Ad-hoc Network
// Nodes created based on aliasmap.txt, each polling NSB server.
//    (802.11b, 1Mbps, Range ~500m, AODV routing)
//

using namespace ns3;

#define DEBUG 0
#define ACTIVE_DEBUG 0

NS_LOG_COMPONENT_DEFINE("FirstScriptExample");

// --- Constants for Polling ---
const static double POLLING_INTERVAL = 10.0; // seconds
const static double INITIAL_POLL_DELAY_BASE = 2.0; // seconds
const static double POLL_STAGGER_INTERVAL = 0.5; // seconds to stagger initial polls

// --- Forward Declarations of new functions ---
struct NodeMapping; // Defined later
std::vector<NodeMapping> ReadAliasmap(const std::string& aliasmapPath, bool& success, std::ostream& mainLog);
bool BuildNetworkTopology(const std::vector<NodeMapping>& nodeMappings, 
                          NodeContainer& allSimNodes, 
                          NetDeviceContainer& wifiDevices, 
                          Ipv4InterfaceContainer& allInterfaces,
                          std::ostream& mainLog);
void SetupUdpReceiveSockets(const std::vector<NodeMapping>& nodeMappings, const NodeContainer& allSimNodes, std::ostream& mainLog);
void HandleNsbRelayAndNotify(const nsbHeaderAndData_t& nsbMessage, 
                             Ptr<Node> fetchingNs3Node, 
                             Ipv4Address fetchingNs3NodeIp, // For logging or context
                             NSBConnector* connector,
                             std::ostream& logStream);
void PollAndProcessNsbMessage(NSBConnector* connector, NodeMapping nodeInfo);
void CreateLogDirectory(const std::string& path, std::ostream* mainLog);

const char *_OH_MSG_TYPES_STR[] = {
    "OH_DELIVER_MSG",
    "OH_DELIVER_MSG_ACK",
    "OH_RECV_MSG",
    "OH_RESP_MSG",
    "OH_MSG_GETSTATE",
    "OH_MSG_STATE",
    "OH_MSG_DUMMY"
};

const char *_ERROR_CODES[] = {
    "SUCCESS",
    "MESSAGE_NOT_FOUND",
    "MESSAGE_ALREADY_DELIVERED",
    "MESSAGE_IN_WRONG_QUEUE",
    "MESSAGE_TO_SELF",
    "OH_ERR_SOCK_CREATE",
    "OH_ERR_INVALID_ADDR",
    "OH_ERR_CONN_FAIL"
};

int getConnection (void)
/*
Establishes a connection to the NSB server. Returns the socket integer.
*/
{
    if (DEBUG) printf("Connecting to the socket...");
    // Used code from: https://www.geeksforgeeks.org/socket-programming-cc/
    int sock = 0;
    struct sockaddr_in serv_addr;
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return -OH_ERR_SOCK_CREATE;
    }
 
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(O_SERVER_PORT);
 
    // Convert IPv4 and IPv6 addresses from text to binary form.
    if (inet_pton(AF_INET, O_SERVER_ADDRESS, &serv_addr.sin_addr)
        <= 0) {
        printf("\nInvalid address/ Address not supported \n");
        return -OH_ERR_INVALID_ADDR;
    }
 
    if (connect(sock, (struct sockaddr*)&serv_addr,
                sizeof(serv_addr))
        < 0) {
        printf("\nConnection Failed \n");
        return -OH_ERR_CONN_FAIL;
    }
    if (DEBUG) printf("\n ... Connected!\n");
    return sock;
}

static void nsbSendMsg(int sock, void* msg, uint32_t msgsize)
/*
Sends a message over the previously established socket.
*/
{
    if (write(sock, msg, msgsize) < 0)
    {
        printf("Can't send message.\n");
        close(sock);
        exit(1);
    }
//    printf("Message sent (%d bytes).\n", msgsize);
    return;
}

static void dumpHeader(oh_header_t *oh)
/*
For debugging purposes, prints the header of a NSB message.
*/
{
    if (!DEBUG) return; // Only print if DEBUG is on

    printf("HEADER - Type %s (%d) Len %u srcid %u dstid %u msgid %u\n",
        _OH_MSG_TYPES_STR[oh->type], oh->type, oh->len,
        oh->srcid, oh->dstid, oh->msgid);
}

static int recv_header(int sock, oh_header_t *h, size_t sz)
/*
Receives just the header of a NSB message.
*/
{
//    printf("READING HEADER -> ");
    int nread = read(sock, h, sz);
//    printf("HEADER %d bytes (sz %ld)\n", nread, sz);
    dumpHeader(h);
    return nread;
}

/*
LOW LEVEL FUNCTIONS (LEGACY).

These functions are one-directional and are used to send requests and receive 
responses from the NSB server. They are good to use when you are looking for 
non-blocking implementations, but will require that you implement both parts of
each transaction.
*/

int send_oh_recv_msg(int sock, uint32_t srcid)
{
    oh_header_t header;

    header.type = OH_RECV_MSG,
    header.len = 0,
    header.dstid = (uint32_t)-1,
    header.msgid = (uint32_t)-1,
    header.srcid = srcid;

    nsbSendMsg(sock, &header, sizeof(oh_header_t));
    return 0;
}

void * recv_oh_resp_msg(int sock, oh_header_t *header)
{
    typedef struct {
        uint32_t    id;
        double      temp;
        uint32_t    counter;
    } oh_resp_msg_t; // This typedef seems unused in this function

    void *data = NULL;

    ssize_t bytes_read = recv_header(sock, header, sizeof(oh_header_t));
    if (bytes_read != (ssize_t)sizeof(oh_header_t)) { // Check if exact header size was read
        if (DEBUG) perror("recv_oh_resp_msg: recv_header failed or read incomplete header");
        return nullptr; 
    }

    // It's good practice to validate header->type if it's within expected range
    // For example: if (header->type >= OH_MSG_DUMMY /* or max valid type */ ) { handle error }
    assert(header->type == OH_RESP_MSG);

    if (header->len > 0) {
        data = malloc(header->len);
        if (!data) {
             if (DEBUG) perror("recv_oh_resp_msg: malloc failed");
             return nullptr;
        }
        bytes_read = read(sock, data, header->len);
        if (bytes_read != (ssize_t)header->len) { // Check if exact payload size was read
            if (DEBUG) perror("recv_oh_resp_msg: read data failed or read incomplete data");
            free(data);
            return nullptr;
        }
    }
    return data;
}

int send_oh_deliver_msg(int sock, uint32_t len, uint32_t srcid,
    uint32_t dstid, uint32_t msgid, void *data)
{
    void * pkt = malloc(sizeof(oh_header_t) + len);

    oh_header_t *header = (oh_header_t *)pkt;

    header->type = OH_DELIVER_MSG,
    header->len = len,
    header->msgid = msgid,
    header->srcid = srcid,
    header->dstid = dstid;

    memcpy((char *)pkt + sizeof(oh_header_t), data, len);
    nsbSendMsg(sock, pkt, sizeof(oh_header_t) + len);

    return 0;
}
oh_deliver_msg_ack_t * recv_oh_deliver_msg_ack(int sock)
{
    oh_header_t header;

    oh_deliver_msg_ack_t *oh_ack =
        (oh_deliver_msg_ack_t *) malloc(sizeof(oh_deliver_msg_ack_t));
    if (!oh_ack) {
        if (DEBUG) perror("recv_oh_deliver_msg_ack: malloc for oh_ack failed");
        return nullptr;
    }

    ssize_t bytes_read = recv_header(sock, &header, sizeof(oh_header_t));
    if (bytes_read != (ssize_t)sizeof(oh_header_t)) {
        if (DEBUG) perror("recv_oh_deliver_msg_ack: recv_header failed or read incomplete header");
        free(oh_ack);
        return nullptr;
    }

    assert(header.type == OH_DELIVER_MSG_ACK);
    
    // Check if the received header length matches the expected size for oh_deliver_msg_ack_t
    if (header.len != sizeof(oh_deliver_msg_ack_t)) {
        if (DEBUG) printf("recv_oh_deliver_msg_ack: header length mismatch. Expected %zu, got %u\n", 
                           sizeof(oh_deliver_msg_ack_t), header.len);
        free(oh_ack);
        return nullptr; 
    }

    bytes_read = read(sock, oh_ack, sizeof(oh_deliver_msg_ack_t)); // Use sizeof directly as we've validated header.len
    if (bytes_read != (ssize_t)sizeof(oh_deliver_msg_ack_t)) {
        if (DEBUG) perror("recv_oh_deliver_msg_ack: read data for oh_ack failed or read incomplete data");
        free(oh_ack);
        return nullptr;
    }
    
    if (DEBUG) printf("Received returnCode=%s (%d)\n", _ERROR_CODES[oh_ack->returnCode], oh_ack->returnCode);

    return oh_ack;
}

NSBConnector::NSBConnector(){
    // Set up socket.
    if (ACTIVE_DEBUG) printf("Setting up socket.\n");
    sock = getConnection();
    if (ACTIVE_DEBUG) printf("Connection set.\n\n");
    // printf("Socket:");
}

NSBConnector::~NSBConnector(){
    // Close socket.
    if (DEBUG) printf("Deconstructor called. Closing socket.\n");
     close(sock);
}
nsbHeaderAndData_t NSBConnector::receiveMessageFromNsbServer(std::string sourceAddress){
    // Set up socket.
    if (ACTIVE_DEBUG) printf("Setting up socket\n"); 
    int sock = getConnection();
    if (sock < 0) { // getConnection returns negative on error
        if (DEBUG || ACTIVE_DEBUG) printf("NSBConnector::receiveMessageFromNsbServer: getConnection failed.\n");
        nsbHeaderAndData_t errReturn; 
        errReturn.header.type = OH_MSG_DUMMY; // Indicate error
        errReturn.header.len = 0;
        errReturn.data = nullptr;
        return errReturn;
    }
    if (ACTIVE_DEBUG) printf("Socket established for receiveMessageFromNsbServer: %d\n", sock);

    // Convert string IP address to uint32_t.
    if (DEBUG) printf("Source address for NSB request: %s\n", sourceAddress.c_str());
    uint32_t sourceAddress_u32 = htonl(inet_addr(sourceAddress.c_str()));
    
    /* SEND REQUEST FOR MESSAGE. */
    oh_header_t header;
    header.type = OH_RECV_MSG,
    header.len = 0,
    header.dstid = (uint32_t)-1,
    header.msgid = (uint32_t)-1,
    header.srcid = sourceAddress_u32;

    if (DEBUG) printf("Sending request for message to NSB server.\n");
    nsbSendMsg(sock, &header, sizeof(oh_header_t));

    if (DEBUG) {
        printf("Header bytes sent (request): ");
        for (size_t i = 0; i < sizeof(oh_header_t); i++){ // Changed int to size_t
            printf("%02x ", ((uint8_t *)&header)[i]);
        }
        printf("\n");
    }

    /* RECEIVE RESPONSE FROM SERVER. */
    oh_header_t resp_header; // Use a new header for the response
    ssize_t bytes_actually_read = recv_header(sock, &resp_header, sizeof(oh_header_t));
    
    nsbHeaderAndData_t headerAndData; // Prepare return struct

    if (bytes_actually_read != (ssize_t)sizeof(oh_header_t)) {
        if (DEBUG) perror("NSBConnector::receiveMessageFromNsbServer: recv_header failed or read incomplete header");
        headerAndData.header.type = OH_MSG_DUMMY; // Indicate error
        headerAndData.header.len = 0;
        headerAndData.data = nullptr;
        close(sock);
        if (ACTIVE_DEBUG) printf("Closed socket %d (error path recv_header)\n", sock);
        return headerAndData;
    }
    
    // dumpHeader(&resp_header); // Already called inside recv_header if DEBUG
    assert(resp_header.type == OH_RESP_MSG); // Check type after successful read

    headerAndData.header = resp_header; // Store the received header
    headerAndData.data = nullptr;     // Default to no data

    if (resp_header.len > 0) {
        void *data = malloc(resp_header.len);
        if(!data) {
            if(DEBUG) perror("NSBConnector::receiveMessageFromNsbServer: malloc failed");
            headerAndData.header.type = OH_MSG_DUMMY; // Indicate error
            headerAndData.header.len = 0; // Reset len on error
            // headerAndData.data is already nullptr
            close(sock);
            if (ACTIVE_DEBUG) printf("Closed socket %d (error path malloc)\n", sock);
            return headerAndData;
        }
        bytes_actually_read = read(sock, data, resp_header.len);
        if (bytes_actually_read != (ssize_t)resp_header.len) {
            if (DEBUG) perror("NSBConnector::receiveMessageFromNsbServer: read data failed or read incomplete data");
            free(data);
            headerAndData.header.type = OH_MSG_DUMMY; // Indicate error
            headerAndData.header.len = 0; // Reset len on error
            // headerAndData.data is already nullptr
            close(sock);
            if (ACTIVE_DEBUG) printf("Closed socket %d (error path read data)\n", sock);
            return headerAndData;
        }
        headerAndData.data = data; // Store successfully read data
    }
    
    close(sock); // Close socket on successful completion too
    if (ACTIVE_DEBUG) printf("Closed socket %d (successful path)\n", sock);
    return headerAndData;
}

int NSBConnector::notifyDeliveryToNsbServer(int msgLen, uint32_t sourceAddress, uint32_t destinationAddress, int msgId, void *message){
    // This function uses this->sock (the one from constructor)

    void * pkt = malloc(sizeof(oh_header_t) + msgLen);
    if (!pkt) {
        if (DEBUG) perror("NSBConnector::notifyDeliveryToNsbServer: malloc for pkt failed");
        return 1; // Error
    }

    oh_header_t *header_to_send = (oh_header_t *)pkt;

    header_to_send->type = OH_DELIVER_MSG,
    header_to_send->len = msgLen,
    header_to_send->dstid = destinationAddress,
    header_to_send->msgid = msgId,
    header_to_send->srcid = sourceAddress;

    memcpy((char *)pkt + sizeof(oh_header_t), message, msgLen);
    
    if (DEBUG) printf("Sending delivery notification to NSB server.\n");
    nsbSendMsg(sock, pkt, sizeof(oh_header_t) + msgLen); // Use this->sock

    if (DEBUG) {
        printf("Header bytes sent (delivery notification): ");
        for (size_t i = 0; i < sizeof(oh_header_t) + (size_t)msgLen; i++){ // Changed int to size_t, cast msgLen
            printf("%02x ", ((uint8_t *)pkt)[i]);
        }
        printf("\n");
    }

    /* RECEIVE RESPONSE FROM SERVER. */
    oh_header_t ack_pkt_header; // Use a new header for the ACK packet
    ssize_t bytes_actually_read = recv_header(sock, &ack_pkt_header, sizeof(oh_header_t)); // Use this->sock
    
    if (bytes_actually_read != (ssize_t)sizeof(oh_header_t)) {
        if (DEBUG) perror("NSBConnector::notifyDeliveryToNsbServer: recv_header for ack failed or incomplete");
        free(pkt); 
        return 1; // Error
    }
    
    // dumpHeader(&ack_pkt_header); // Called in recv_header
    assert(ack_pkt_header.type == OH_DELIVER_MSG_ACK);

    if (ack_pkt_header.len > 0) {
        // Check if the received ACK payload size matches oh_deliver_msg_ack_t
        if (ack_pkt_header.len != sizeof(oh_deliver_msg_ack_t)) {
            if (DEBUG) printf("NSBConnector::notifyDeliveryToNsbServer: ACK header length %u unexpected for oh_deliver_msg_ack_t (expected %zu)\n", 
                               ack_pkt_header.len, sizeof(oh_deliver_msg_ack_t));
            free(pkt);
            return 1; // Protocol error
        }

        oh_deliver_msg_ack_t ack_payload_data; // Stack variable for the ACK payload
        bytes_actually_read = read(sock, &ack_payload_data, ack_pkt_header.len); // Use ack_pkt_header.len (should be sizeof struct)
        if (bytes_actually_read != (ssize_t)ack_pkt_header.len) {
            if (DEBUG) perror("NSBConnector::notifyDeliveryToNsbServer: read ack_payload_data failed or incomplete");
            free(pkt);
            return 1; // Error
        }
        
        if (DEBUG) printf("Received ACK returnCode=%s (%d)\n", _ERROR_CODES[ack_payload_data.returnCode], ack_payload_data.returnCode);
        free(pkt); 
        return (ack_payload_data.returnCode == SUCCESS) ? 0 : 1;

    } else { // No payload in ACK, but oh_deliver_msg_ack_t is expected to have content (returnCode)
        if (DEBUG) printf("NSBConnector::notifyDeliveryToNsbServer: ACK received with no payload data, but expected oh_deliver_msg_ack_t payload.\n");
        free(pkt); 
        return 1; // Error, as we expect a returnCode in the payload
    }
}

// Function to handle received packets
void ReceivePacket(Ptr<Socket> socket)
{
  Address from;
  Ptr<Packet> packet = socket->RecvFrom(from);
  // InetSocketAddress address = InetSocketAddress::ConvertFrom(from); // Commented out: address was unused
  
  uint32_t size = packet->GetSize();
  
  Ptr<Ipv4> ipv4 = socket->GetNode()->GetObject<Ipv4>();
  Ipv4Address receiverIp = Ipv4Address::GetAny(); 
  bool foundIp = false;
  for (uint32_t i = 0; i < ipv4->GetNInterfaces(); ++i) {
      for (uint32_t j = 0; j < ipv4->GetNAddresses(i); ++j) {
          if (ipv4->GetAddress(i,j).GetLocal() != Ipv4Address("127.0.0.1")) {
              receiverIp = ipv4->GetAddress(i,j).GetLocal();
              foundIp = true;
              break;
          }
      }
      if (foundIp) break;
  }
  
  // Commenting out direct console log from ReceivePacket
  /*
  std::cout << "Node " << receiverIp << " received NSB message of size " << size << " bytes from " 
            << address.GetIpv4() << ":" << address.GetPort() << std::endl;
  */
  
  uint8_t *buffer = new uint8_t[size];
  packet->CopyData(buffer, size);
  
  // You can now process the received data
  // For example, you could notify NSB server about delivery (current logic does this from sender side)
  
  delete[] buffer;
}

// Function to send NSB message via NS-3 UDP socket
void SendNsbMessageViaUdp(Ptr<Node> sourceNode, Ipv4Address destAddr, uint16_t port, 
                         void* msgData, uint32_t msgLen)
{
  // Create a UDP socket
  Ptr<Socket> socket = Socket::CreateSocket(sourceNode, UdpSocketFactory::GetTypeId());
  
  // Create an IP destination address
  InetSocketAddress destination(destAddr, port);
  
  // Connect the socket to the destination
  socket->Connect(destination);
  
  // Create packet from the message data
  Ptr<Packet> packet;
  if (msgData != nullptr && msgLen > 0) {
    // Create a buffer from the message data
    uint8_t* buffer = new uint8_t[msgLen];
    memcpy(buffer, msgData, msgLen);
    
    // Create a packet from the buffer
    packet = Create<Packet>(buffer, msgLen);
    delete[] buffer;
  } else {
    // Empty packet if no data
    packet = Create<Packet>();
  }
  
  // Send the packet
  socket->Send(packet);
  std::cout << "Sent NSB message of size " << msgLen << " bytes to " 
            << destAddr << ":" << port << std::endl;
  
  // Close the socket
  socket->Close();
}

// --- Structure to hold node information from aliasmap.txt ---
struct NodeMapping {
    std::string ipString;
    std::string nodeIdString;
    ns3::Ipv4Address ipv4Addr;
    ns3::Ptr<ns3::Node> nodePtr;
    uint32_t wifiDeviceIndex; 
    std::ofstream* logFile; // Added for per-node logging
};

// --- Implementation of new functions ---

void CreateLogDirectory(const std::string& path, std::ostream* mainLog) {
    int status = mkdir(path.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    if (mainLog) {
        if (status == 0) {
            *mainLog << Simulator::Now().GetSeconds() << "s [INFO] Main: Log directory '" << path << "' created successfully." << std::endl;
        } else if (errno == EEXIST) {
            *mainLog << Simulator::Now().GetSeconds() << "s [INFO] Main: Log directory '" << path << "' already exists." << std::endl;
        } else {
            *mainLog << Simulator::Now().GetSeconds() << "s [WARN] Main: Error creating log directory '" << path << "': " << strerror(errno) << std::endl;
        }
    } else {
        if (status == 0) {
            NS_LOG_INFO("Log directory '" << path << "' created successfully or already exists.");
        } else if (errno == EEXIST) {
            NS_LOG_INFO("Log directory '" << path << "' already exists.");
        } else {
            NS_LOG_WARN("Error creating log directory '" << path << "': " << strerror(errno));
        }
    }
}

std::vector<NodeMapping> ReadAliasmap(const std::string& aliasmapPath, bool& success, std::ostream& mainLog) {
    std::vector<NodeMapping> mappings;
    std::ifstream aliasFile(aliasmapPath);
    std::string line;
    success = false;

    if (!aliasFile.is_open()) {
        mainLog << Simulator::Now().GetSeconds() << "s [ERROR] Main: Could not open aliasmap file: " << aliasmapPath << std::endl;
        return mappings;
    }

    mainLog << Simulator::Now().GetSeconds() << "s [INFO] Main: Reading aliasmap from: " << aliasmapPath << std::endl;
    while (std::getline(aliasFile, line)) {
        std::stringstream ss(line);
        std::string ipPart, nodeIdPart;
        if (std::getline(ss, ipPart, ':') && std::getline(ss, nodeIdPart)) {
            if (ipPart.empty() || nodeIdPart.empty()) {
                mainLog << Simulator::Now().GetSeconds() << "s [WARN] Main: Skipping malformed line in aliasmap: " << line << std::endl;
                continue;
            }
            NodeMapping mapping;
            mapping.ipString = ipPart;
            mapping.nodeIdString = nodeIdPart;
            mapping.logFile = nullptr; // Initialize logFile pointer
            try {
                mapping.ipv4Addr = Ipv4Address(ipPart.c_str());
            } catch (const std::exception& e) {
                mainLog << Simulator::Now().GetSeconds() << "s [WARN] Main: Invalid IP address format in aliasmap: " << ipPart << " on line: " << line << ". Error: " << e.what() << std::endl;
                continue;
            }
            mappings.push_back(mapping);
            mainLog << Simulator::Now().GetSeconds() << "s [INFO] Main: Read mapping: IP=" << ipPart << ", NodeID=" << nodeIdPart << std::endl;
        } else {
             mainLog << Simulator::Now().GetSeconds() << "s [WARN] Main: Skipping malformed line (no colon separator?) in aliasmap: " << line << std::endl;
        }
    }
    aliasFile.close();

    if (!mappings.empty()) {
        mainLog << Simulator::Now().GetSeconds() << "s [INFO] Main: Successfully read " << mappings.size() << " node mappings." << std::endl;
        success = true;
    } else {
        mainLog << Simulator::Now().GetSeconds() << "s [ERROR] Main: No valid node mappings found in " << aliasmapPath << "." << std::endl;
    }
    return mappings;
}

bool BuildNetworkTopology(const std::vector<NodeMapping>& nodeMappings, 
                          NodeContainer& allSimNodes, 
                          NetDeviceContainer& wifiDevices, 
                          Ipv4InterfaceContainer& allInterfaces,
                          std::ostream& mainLog)
{
    if (nodeMappings.empty()) {
        mainLog << Simulator::Now().GetSeconds() << "s [ERROR] Main: BuildNetworkTopology: nodeMappings is empty." << std::endl;
        return false;
    }

    mainLog << Simulator::Now().GetSeconds() << "s [INFO] Main: Creating Dynamic Wireless Topology with " << nodeMappings.size() << " nodes." << std::endl;
    allSimNodes.Create(nodeMappings.size());

    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channel.AddPropagationLoss("ns3::RangePropagationLossModel", "MaxRange", DoubleValue(500.0));
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

    wifiDevices = wifi.Install(phy, mac, allSimNodes); // Fills the passed NetDeviceContainer
    
    // Mobility
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator>();
    int nodesPerRow = 10; 
    double spacing = 50.0; 
    for (size_t i = 0; i < nodeMappings.size(); ++i) {
        double x = (i % nodesPerRow) * spacing;
        double y = floor(i / nodesPerRow) * spacing;
        positionAlloc->Add(Vector(x, y, 0.0));
        mainLog << Simulator::Now().GetSeconds() << "s [INFO] Main: Node " << nodeMappings[i].nodeIdString << " (" << nodeMappings[i].ipString 
                    << ") initial position: (" << x << ", " << y << ", 0.0)" << std::endl;
    }
    mobility.SetPositionAllocator(positionAlloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(allSimNodes);

    // Internet Stack
    InternetStackHelper stack;
    AodvHelper aodv; 
    stack.SetRoutingHelper(aodv);
    stack.Install(allSimNodes); 

    // Assign IP Addresses 
    for (size_t i = 0; i < nodeMappings.size(); ++i) {
        Ptr<Ipv4> ipv4 = allSimNodes.Get(i)->GetObject<Ipv4>();
        // wifiDevices.Get(i) corresponds to allSimNodes.Get(i)
        uint32_t interfaceIndex = ipv4->AddInterface(wifiDevices.Get(i)); 
        Ipv4InterfaceAddress ipv4IfAddr(nodeMappings[i].ipv4Addr, Ipv4Mask("255.255.255.0"));
        ipv4->AddAddress(interfaceIndex, ipv4IfAddr);
        ipv4->SetMetric(interfaceIndex, 1);
        ipv4->SetUp(interfaceIndex);
        allInterfaces.Add(ipv4, interfaceIndex); 
        mainLog << Simulator::Now().GetSeconds() << "s [INFO] Main: Assigned IP " << nodeMappings[i].ipv4Addr << " to NodeID " << nodeMappings[i].nodeIdString 
                    << " (Node index " << i << ")" << std::endl;
    }
    return true;
}

void SetupUdpReceiveSockets(const std::vector<NodeMapping>& nodeMappings, const NodeContainer& allSimNodes, std::ostream& mainLog) {
    mainLog << Simulator::Now().GetSeconds() << "s [INFO] Main: Setting up UDP receive sockets on all nodes." << std::endl;
    for (size_t i = 0; i < nodeMappings.size(); ++i) {
        Ptr<Socket> recvSocket = Socket::CreateSocket(allSimNodes.Get(i), UdpSocketFactory::GetTypeId());
        InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), 5000); 
        if (recvSocket->Bind(local) == -1) {
            mainLog << Simulator::Now().GetSeconds() << "s [ERROR] Main: Node " << nodeMappings[i].nodeIdString << " (" << nodeMappings[i].ipString 
                       << ") failed to bind UDP socket to port 5000." << std::endl;
        } else {
             mainLog << Simulator::Now().GetSeconds() << "s [INFO] Main: Node " << nodeMappings[i].nodeIdString << " (" << nodeMappings[i].ipString 
                       << ") bound UDP socket to port 5000." << std::endl;
        }
        recvSocket->SetRecvCallback(MakeCallback(&ReceivePacket));
    }
}

void HandleNsbRelayAndNotify(const nsbHeaderAndData_t& nsbMessage, 
                             Ptr<Node> fetchingNs3Node, 
                             Ipv4Address fetchingNs3NodeIp,
                             NSBConnector* connector,
                             std::ostream& logStream)
{
    uint32_t nsb_payload_srcid_raw = nsbMessage.header.srcid; 
    uint32_t nsb_payload_dstid_raw = nsbMessage.header.dstid;

    if (ACTIVE_DEBUG) {
        // Convert Ipv4Address to string for logging
        std::ostringstream ipStrStream;
        fetchingNs3NodeIp.Print(ipStrStream);
        
        logStream << Simulator::Now().GetSeconds() << "s [Node " << ipStrStream.str() 
                  << "] HandleNsbRelayAndNotify: Fetched by " << ipStrStream.str() 
                  << ". NSB Payload Original SrcID: " << nsb_payload_srcid_raw 
                  << ", Final DstID: " << nsb_payload_dstid_raw 
                  << ", Len: " << nsbMessage.header.len << std::endl;
    }
    
    Ipv4Address ns3ActualDestIp(ntohl(nsb_payload_dstid_raw)); 
    if (ACTIVE_DEBUG) {
        logStream << Simulator::Now().GetSeconds() << "s [Node " << fetchingNs3NodeIp 
                  << "] NSB message payload is for ns-3 destination IP: " << ns3ActualDestIp << std::endl;
    }

    Simulator::Schedule(Seconds(0.01), 
                      &SendNsbMessageViaUdp, 
                      fetchingNs3Node,           
                      ns3ActualDestIp,                 
                      5000,                            
                      nsbMessage.data,                    
                      nsbMessage.header.len);                  
                              
    int result = connector->notifyDeliveryToNsbServer(nsbMessage.header.len, 
                                                    nsb_payload_srcid_raw, 
                                                    nsb_payload_dstid_raw, 
                                                    nsbMessage.header.msgid, 
                                                    nsbMessage.data);
    if (result == 0) {
        if (ACTIVE_DEBUG) logStream << Simulator::Now().GetSeconds() << "s [Node " << fetchingNs3NodeIp 
                                  << "] Notification of delivery/forwarding to NSB server successful for msgid " << nsbMessage.header.msgid << std::endl;
    } else {
        if (ACTIVE_DEBUG) logStream << Simulator::Now().GetSeconds() << "s [Node " << fetchingNs3NodeIp 
                                  << "] Notification of delivery/forwarding to NSB server failed (Code: " << result << ") for msgid " << nsbMessage.header.msgid << std::endl;
    }
}

void PollAndProcessNsbMessage(NSBConnector* connector, NodeMapping nodeInfo) {
    std::ostream& log = *(nodeInfo.logFile); // Get reference to the log stream

    if (ACTIVE_DEBUG) {
        log << Simulator::Now().GetSeconds() << "s [Node " << nodeInfo.ipString 
            << ", Ptr: " << nodeInfo.nodePtr << "] is polling NSB server." << std::endl;
    }

    nsbHeaderAndData_t nsb_message = connector->receiveMessageFromNsbServer(nodeInfo.ipString);

    if (nsb_message.header.type != (OH_MSG_TYPES)-1) { 
        if (nsb_message.header.len > 0 && nsb_message.data != nullptr) {
            if (ACTIVE_DEBUG) {
                log << Simulator::Now().GetSeconds() << "s [Node " << nodeInfo.ipString << "] received a message from NSB. Relaying..." << std::endl;
                // dumpHeader(&nsb_message.header); // Consider adding logStream to dumpHeader or reimplementing here
                log << Simulator::Now().GetSeconds() << "s [Node " << nodeInfo.ipString 
                    << "] HEADER - Type " << _OH_MSG_TYPES_STR[nsb_message.header.type] << " (" << nsb_message.header.type 
                    << ") Len " << nsb_message.header.len << " srcid " << nsb_message.header.srcid 
                    << " dstid " << nsb_message.header.dstid << " msgid " << nsb_message.header.msgid << std::endl;
            }
            HandleNsbRelayAndNotify(nsb_message, nodeInfo.nodePtr, nodeInfo.ipv4Addr, connector, log);
        } else {
             if (ACTIVE_DEBUG) log << Simulator::Now().GetSeconds() << "s [Node " << nodeInfo.ipString << "] received NSB response with no payload (len=" 
                                       << nsb_message.header.len << ", data=" 
                                       << (nsb_message.data ? "non-null" : "null") 
                                       << ")." << std::endl;
        }
    } else {
        if (ACTIVE_DEBUG) log << Simulator::Now().GetSeconds() << "s [Node " << nodeInfo.ipString << "] failed to properly receive message structure from NSB server (error indicated by header type -1)." << std::endl;
    }

    if (nsb_message.data != nullptr) { 
        free(nsb_message.data);
    }

    // Reschedule polling for this node
    Simulator::Schedule(Seconds(POLLING_INTERVAL), &PollAndProcessNsbMessage, connector, nodeInfo);
}

int
main(int argc, char* argv[])
{
    CommandLine cmd(__FILE__);
    cmd.Parse(argc, argv);

    Time::SetResolution(Time::NS);
    // LogComponentEnable("FirstScriptExample", LOG_LEVEL_INFO); // Keep for potential other ns-3 logs, but our main logs are redirected

    // Open main log file first
    std::ofstream mainLogFile;
    // Initial call to CreateLogDirectory without mainLogFile, or pass nullptr safely
    CreateLogDirectory("log", nullptr); 
    mainLogFile.open("log/main_simulation.log", std::ios_base::app);
    if (!mainLogFile.is_open()) {
        NS_LOG_ERROR("FATAL: Could not open main_simulation.log. Exiting."); // Use NS_LOG as fallback if main log fails
        // Optionally print to cerr as well
        std::cerr << "FATAL: Could not open log/main_simulation.log. Exiting." << std::endl;
        return 1;
    }
    mainLogFile << "--- Main Simulation Log Started at Real Time: " << std::time(0) << " ---" << std::endl;
    // Now pass mainLogFile to CreateLogDirectory for its own logging
    CreateLogDirectory("log", &mainLogFile); 

    bool aliasmapReadSuccess = false;
    std::vector<NodeMapping> nodeMappings = ReadAliasmap("scratch/aliasmap.txt", aliasmapReadSuccess, mainLogFile);

    if (!aliasmapReadSuccess || nodeMappings.empty()) {
        mainLogFile << Simulator::Now().GetSeconds() << "s [ERROR] Main: Exiting: Failed to read aliasmap or no nodes defined." << std::endl;
        mainLogFile.close();
        return 1;
    }

    NodeContainer allSimNodes; 
    NetDeviceContainer wifiDevices; 
    Ipv4InterfaceContainer allInterfaces;

    if (!BuildNetworkTopology(nodeMappings, allSimNodes, wifiDevices, allInterfaces, mainLogFile)) {
        mainLogFile << Simulator::Now().GetSeconds() << "s [ERROR] Main: Exiting: Failed to build network topology." << std::endl;
        mainLogFile.close();
        return 1;
    }

    for (size_t i = 0; i < nodeMappings.size(); ++i) {
        nodeMappings[i].nodePtr = allSimNodes.Get(i);
        nodeMappings[i].wifiDeviceIndex = i; 

        std::string logFilePath = "log/node_" + nodeMappings[i].ipString + "_output.log";
        nodeMappings[i].logFile = new std::ofstream(logFilePath, std::ios_base::app); 
        if (!nodeMappings[i].logFile->is_open()) {
            mainLogFile << Simulator::Now().GetSeconds() << "s [ERROR] Main: Failed to open log file for node " << nodeMappings[i].ipString << " at " << logFilePath << std::endl;
            delete nodeMappings[i].logFile; 
            nodeMappings[i].logFile = nullptr; 
        } else {
            mainLogFile << Simulator::Now().GetSeconds() << "s [INFO] Main: Opened log file for node " << nodeMappings[i].ipString << " at " << logFilePath << std::endl;
            *(nodeMappings[i].logFile) << "--- Log started for node " << nodeMappings[i].ipString << " (" << nodeMappings[i].nodeIdString << ") at sim time " << Simulator::Now().GetSeconds() << "s --- " << std::endl;
        }
    }

    SetupUdpReceiveSockets(nodeMappings, allSimNodes, mainLogFile);

    NSBConnector nsbConnector; 

    mainLogFile << Simulator::Now().GetSeconds() << "s [INFO] Main: Scheduling initial polling tasks for " << nodeMappings.size() << " nodes." << std::endl;
    for (size_t i = 0; i < nodeMappings.size(); ++i) {
        Time initialDelay = Seconds(INITIAL_POLL_DELAY_BASE + i * POLL_STAGGER_INTERVAL);
        NodeMapping currentMapping = nodeMappings[i]; 
        
        if (!currentMapping.nodePtr) {
             mainLogFile << Simulator::Now().GetSeconds() << "s [ERROR] Main: Node " << currentMapping.nodeIdString << " has NULL nodePtr. Skipping polling schedule." << std::endl;
             continue;
        }
        if (!currentMapping.logFile || !currentMapping.logFile->is_open()){ 
            mainLogFile << Simulator::Now().GetSeconds() << "s [WARN] Main: Log file for node " << currentMapping.nodeIdString << " is not open. Skipping polling schedule." << std::endl;
            continue;
        }

        Simulator::Schedule(initialDelay, &PollAndProcessNsbMessage, &nsbConnector, currentMapping);
        mainLogFile << Simulator::Now().GetSeconds() << "s [INFO] Main: Scheduled polling for " << currentMapping.nodeIdString << " (" << currentMapping.ipString 
                    << ") to start at " << initialDelay.GetSeconds() << "s." << std::endl;
    }

    double simulationTime = 60.0; 
    if (!nodeMappings.empty()){ // If there are nodes, make sim time longer than a few poll cycles
         simulationTime = INITIAL_POLL_DELAY_BASE + nodeMappings.size() * POLL_STAGGER_INTERVAL + 3 * POLLING_INTERVAL + 5.0;
    }
    mainLogFile << Simulator::Now().GetSeconds() << "s [INFO] Main: Simulation will run for " << simulationTime << " seconds." << std::endl;
    // Simulator::Stop(Seconds(simulationTime)); // Removed to allow indefinite running

    mainLogFile << Simulator::Now().GetSeconds() << "s [INFO] Main: Starting Simulation." << std::endl;
    Simulator::Run();
    mainLogFile << Simulator::Now().GetSeconds() << "s [INFO] Main: Simulation Finished (or interrupted)." << std::endl;

    // Close all log files
    for (size_t i = 0; i < nodeMappings.size(); ++i) {
        if (nodeMappings[i].logFile && nodeMappings[i].logFile->is_open()) {
            *(nodeMappings[i].logFile) << "--- Log ended for node " << nodeMappings[i].ipString << " at sim time " << Simulator::Now().GetSeconds() << "s --- " << std::endl;
            nodeMappings[i].logFile->close();
            delete nodeMappings[i].logFile;
            nodeMappings[i].logFile = nullptr;
        }
    }

    if (mainLogFile.is_open()) {
        mainLogFile << "--- Main Simulation Log Ended at Real Time: " << std::time(0) << " ---" << std::endl;
        mainLogFile.close();
    }

    Simulator::Destroy();
    return 0;
}

