# RabbitMQ Enhancement Suggestions for NSB

This doc lists opportunities where RabbitMQ features could significantly improve the NSB architecture. These suggestions range from quick work to more substantial redesigns.

---

## 1. Message Priority Queues

### Current State
All messages are treated equally in the current implementation. The `nsb.recv.{client_id}` queues process messages in FIFO order.

### Opportunity
RabbitMQ supports **priority queues** (up to 255 priority levels). This could enable:
- **Control messages** (INIT, PING, EXIT) to take precedence over data messages
- **Urgent simulation events** to bypass queued payloads
- **QoS simulation** where high-priority network traffic is modeled

### Impact
- More realistic network simulation (priority-based routing)
- Better responsiveness for control plane operations
- Enables modeling of DiffServ or similar QoS mechanisms

---

## 2. Dead Letter Exchanges (DLX) for Message Tracking

### Current State
Messages that fail to be processed or time out are simply lost. There's no visibility into failed deliveries.

### Opportunity
RabbitMQ's **Dead Letter Exchanges** can capture:
- Messages that exceed TTL (simulating network timeouts)
- Rejected/failed messages
- Messages from deleted queues (client disconnection)

### Impact
- **Debugging**: Track why messages weren't delivered
- **Metrics**: Count dropped packets for simulation accuracy
- **Timeout simulation**: Model network latency/drops via TTL
- **Audit trail**: Log all failed communications

---

## 3. Topic Exchange for Multicast/Broadcast

### Current State
The implementation uses a **direct exchange** with point-to-point routing (`nsb.recv.{client_id}`). Broadcasting requires sending to each client individually.

### Opportunity
A **topic exchange** enables:
- **Multicast**: `nsb.recv.group.*` → all nodes in a group
- **Broadcast**: `nsb.recv.#` → all nodes
- **Hierarchical addressing**: `nsb.recv.datacenter1.rack2.node5`

### Impact
- **Efficient broadcast**: Single publish reaches all subscribers
- **Group communication**: Model network segments, VLANs, multicast groups
- **Hierarchical networks**: Natural fit for datacenter/IoT topologies in sims

---

## 4. Message TTL for Network Latency Simulation

### Current State
Messages are delivered as fast as possible. Network latency must be simulated externally.

### Opportunity
RabbitMQ's **per-message TTL** and **delayed message plugins** can model:
- Network propagation delay
- Congestion-induced delays
- Timeout behaviors

However this might be behavior that we want to isolate to the network simulator, so it's not a clear win.

### Impact
- **Realistic latency modeling** without application-level sleep()
- **Configurable per-link delays** for heterogeneous networks
- **Decoupled timing** from application logic

---

## 5. Publisher Confirms for Reliable Delivery

### Current State
`basic_publish()` is fire-and-forget. No confirmation that messages reached the broker.

### Opportunity
**Publisher confirms** provide acknowledgment that messages were:
- Received by the broker
- Persisted to disk (if durable)
- Routed to at least one queue

### Impact
- **Guaranteed delivery** for critical simulation events
- **Error detection** when destination doesn't exist
- **Flow control** to prevent overwhelming the broker

---

## 6. Consumer Prefetch for Flow Control

### Current State
The `_recv_msg()` implementation uses `basic_get()` which polls one message at a time.

### Opportunity
**Prefetch (QoS)** settings can:
- Batch message retrieval for throughput
- Limit in-flight messages to prevent memory issues
- Balance load across multiple consumers

### Impact
- **Higher throughput** for high-volume simulations
- **Backpressure** prevents fast producers from overwhelming slow consumers
- **Fair dispatch** across multiple simulator instances

---

## 7. Quorum Queues for High Availability

### Current State
Single-node RabbitMQ setup. Queue data is lost if the broker crashes.

### Opportunity
**Quorum queues** provide:
- Replicated queues across RabbitMQ cluster nodes
- Automatic leader election on failure
- Data safety guarantees


### Impact
- **Fault tolerance** for long-running simulations
- **No message loss** during broker restarts
- **Distributed deployment** for large-scale simulations

---

## 8. Streams for Event Sourcing / Replay

### Current State
Messages are consumed and deleted. No way to replay simulation events.

### Opportunity
RabbitMQ **Streams** (3.9+) provide:
- Persistent, append-only log of messages
- Multiple consumers can read from any offset
- Replay capability for debugging/analysis

### Impact
- **Simulation replay** for debugging
- **Event sourcing** for simulation state reconstruction
- **Analytics** on historical simulation data
- **Multiple consumers** can process same events differently

---

## 9. Alternate Exchange for Unroutable Messages

### Current State
Messages sent to non-existent destinations are silently dropped.

### Opportunity
**Alternate exchanges** catch unroutable messages:

### Impact
- **Error detection** for misconfigured routing
- **Graceful handling** of messages to disconnected clients
- **Debugging** network topology issues

---

## 10. Headers Exchange for Content-Based Routing

### Current State
Routing is based solely on destination client ID.

### Opportunity
**Headers exchange** routes based on message attributes:
- Message type (control vs. data)
- Payload size category
- Simulation metadata


### Impact
- **Content-aware routing** without parsing message bodies
- **Specialized handlers** for different message types
- **Load distribution** based on message characteristics

---

## 11. Connection/Channel Pooling

### Current State
Each client creates its own connection with 3 channels (CTRL, SEND, RECV).

### Opportunity
**Connection pooling** can:
- Reduce connection overhead for many clients
- Share channels across operations
- Improve resource utilization

This is something that we were discussing in past design conversations. The reasons we didn't implement this connection/channel pooling were:
- Difficulty in tracking exact client for send/return messages over shared connection
- Increased overhead per message

### Impact
- **Scalability** for simulations with many nodes
- **Resource efficiency** on broker and clients
- **Connection reuse** reduces setup latency

---


## 13. Management API Integration

### Current State
No programmatic visibility into queue depths, message rates, or client status.

### Opportunity
RabbitMQ's **Management HTTP API** provides:
- Queue statistics (depth, rates, consumers)
- Connection/channel monitoring
- Dynamic configuration

### Impact
- **Monitoring dashboard** for simulation health
- **Dynamic scaling** based on queue depth
- **Client discovery** without daemon tracking
- **Alerting** on queue buildup or disconnections

---

## 14. Lazy Queues for Large Simulations

### Current State
Messages are held in memory, limiting simulation scale.

### Opportunity
**Lazy queues** store messages on disk:
- Support millions of messages per queue
- Reduced memory footprint
- Trade-off: slightly higher latency

### Impact
- **Large-scale simulations** with many queued messages
- **Memory efficiency** for resource-constrained environments
- **Burst handling** without memory exhaustion

---

## Alex's Recommendations

### Short-Term Improvements (Low effort + high value)
1. Dead Letter Exchanges - Immediate debugging visibility
2. Publisher Confirms - Reliability without major changes
3. Consumer Prefetch - Performance improvement

### Medium-Term Improvements
4. Topic Exchange - Enable multicast/broadcast patterns
5. Message TTL - Built-in latency simulation
6. Management API - Monitoring and observability

### Long-Term Strategic Enhancements
7. Streams - Event sourcing and replay capability
8. Priority Queues - QoS simulation
9. Quorum Queues - High availability for production
