# NSB RabbitMQ Implementation

## Architecture Overview

### Key Architectural Advantage

Unlike the socket-based implementation where the daemon acts as a message router, the RabbitMQ implementation leverages the broker's built-in routing capabilities. Clients consume messages directly from their queues without daemon intermediation for message delivery. The daemon's role is limited to configuration management during client initialization.

### Key Components

1. **RabbitMQInterface** (`nsb_rabbitmq.py`)
   - Implements the same interface as `SocketInterface` from the socket-based implementation
   - Manages three communication channels (CTRL, SEND, RECV) using RabbitMQ queues
   - Provides both synchronous and asynchronous message operations
   - Handles connection management and error recovery

2. **NSB Clients** (`nsb_rabbitmq.py`)
   - `NSBAppClient`: Application client for sending and receiving payloads
   - `NSBSimClient`: Simulator client for fetching and posting payloads
   - Both maintain identical semantics to the socket-based clients

3. **NSB Daemon** (`nsb_daemon.py`)
   - Separate service that handles configuration management
   - Listens for INIT requests from clients and responds with system-wide configuration
   - Handles PING and EXIT messages
   - Runs independently and can be started/stopped separately

### Queue Architecture

The implementation uses a topic-based routing scheme with the following queue naming convention:

```
nsb.{channel}.{client_id}
├── nsb.ctrl.{client_id}      # Control channel for INIT, PING, EXIT
├── nsb.send.{client_id}      # Send channel (for app clients)
├── nsb.recv.{client_id}      # Receive channel (for app/sim clients)
├── nsb.config.request        # Daemon configuration request queue
└── nsb.config.response.{client_id}  # Client-specific config response
```

### Message Flow

#### Client Initialization

```
1. Client creates NSBAppClient or NSBSimClient
2. Client sends INIT message to nsb.config.request queue
3. Daemon receives INIT, responds to nsb.config.response.{client_id}
4. Client receives config and initializes
5. Client is ready to send/receive or fetch/post
```

#### Message Sending (App Client)

```
1. App Client calls send(dest_id, payload)
2. Message is published with routing_key=nsb.recv.{dest_id}
3. RabbitMQ broker routes to destination's RECV queue
4. Destination client calls receive()
5. Message consumed directly from RECV queue (no daemon mediation necessary)
```

#### Message Receiving (App Client)

```
1. App Client calls receive(timeout=5)
2. Client consumes directly from nsb.recv.{client_id} queue
3. Message returned as MessageEntry or None on timeout
4. No daemon polling required - direct RMQ queue consumption
```

#### Message Fetching (Simulator Client)

```
1. Simulator Client calls fetch()
2. Simulator consumes directly from nsb.recv.{simulator_id} queue
3. RabbitMQ broker has already routed SEND messages here
4. Message returned as MessageEntry
5. Simulator calls post() to return processed message
```

## Usage

### Starting the Daemon

```python
from nsb_daemon import NSBDaemon, NSBDaemonConfig

# Create configuration
config = NSBDaemonConfig(
    sys_mode=0,      # PULL mode
    sim_mode=0,      # SYSTEM_WIDE mode
    use_db=False,    # Disable database
    db_address="localhost",
    db_port=6379
)

# Create and start daemon
daemon = NSBDaemon("localhost", 5672, config)
daemon.start()
```

Or run directly:

```bash
python nsb_daemon.py
```

### Creating an Application Client

```python
from nsb_rabbitmq import NSBAppClient

# Create client
client = NSBAppClient("app_node_0", "localhost", 5672)

# Send a message
client.send("app_node_1", b"Hello, World!")

# Receive a message
msg = client.receive(timeout=5)
if msg:
    print(f"Received from {msg.src_id}: {msg.payload}")

# Cleanup
client.exit()
```

### Creating a Simulator Client

```python
from nsb_rabbitmq import NSBSimClient

# Create client
sim = NSBSimClient("simulator", "localhost", 5672)

# Fetch a message to simulate
msg = sim.fetch(timeout=5)
if msg:
    print(f"Simulating: {msg.src_id} -> {msg.dest_id}")
    # Process the message...
    
    # Post the result back
    sim.post(msg.src_id, msg.dest_id, b"Processed payload")

# Cleanup
sim.exit()
```

### Asynchronous Operations

```python
import asyncio
from nsb_rabbitmq import NSBAppClient

async def listen_for_messages():
    client = NSBAppClient("listener", "localhost", 5672)
    
    while True:
        msg = await client.listen()
        if msg:
            print(f"Received: {msg.payload}")

# Run async listener
asyncio.run(listen_for_messages())
```

## Configuration

### System Modes

- **PULL Mode (0)**: Clients poll the daemon/broker for messages. Recommended for most configurations.
- **PUSH Mode (1)**: Daemon automatically forwards messages to clients. Better latency but may not work with all configurations.

### Simulator Modes

- **SYSTEM_WIDE (0)**: Single simulator client fetches all messages. Good for top-down simulators (ns-3).
- **PER_NODE (1)**: Each node has its own simulator client. Good for bottom-up simulators (OMNeT++).

### Database Configuration

The implementation supports optional Redis database storage for large payloads:

```python
config = NSBDaemonConfig(
    use_db=True,
    db_address="localhost",
    db_port=6379,
    db_num=0
)
```

When enabled, large payloads are stored in Redis and only the key is transmitted via RabbitMQ.

## Architecture Differences from Socket Implementation

### Similarities

- Maintains identical 3-channel architecture (CTRL, SEND, RECV)
- Uses same protobuf message format (nsbm)
- Supports both PULL and PUSH modes
- Supports SYSTEM_WIDE and PER_NODE simulator modes
- Optional Redis database integration
- Same client API (send, receive, fetch, post, listen)

### Differences from Socket Implementation

- **Transport**: RabbitMQ queues instead of TCP sockets
- **Routing**: Broker handles routing instead of daemon
- **Daemon Role**: Configuration service instead of message router
- **Scalability**: Better horizontal scaling with RabbitMQ
- **Async**: Uses asyncio-based polling instead of socket select

## Dependencies

- `pika`: RabbitMQ Python client
- `redis`: Redis Python client (optional, for database support)
- `proto.nsb_pb2`: NSB protobuf definitions

Install dependencies:

```bash
pip install pika redis
```

## Testing

Example test script:

```python
from nsb_rabbitmq import NSBAppClient, NSBSimClient
import time

# Start daemon first: python nsb_daemon.py

# Create clients
app = NSBAppClient("app", "localhost", 5672)
sim = NSBSimClient("sim", "localhost", 5672)

# Send message
app.send("app", b"test payload")

# Fetch and post
msg = sim.fetch(timeout=5)
if msg:
    sim.post(msg.src_id, msg.dest_id, msg.payload)

# Receive
received = app.receive(timeout=5)
print(f"Received: {received.payload if received else 'Nothing'}")

# Cleanup
app.exit()
sim.exit()
```
