# NSB RabbitMQ Implementation

## Core Implementation Files

### nsb_rabbitmq.py
Main implementation file containing all client and interface classes

Classes:
- `Config`: System configuration management
- `MessageEntry`: Message container
- `Comms`: Base communication interface
- `RabbitMQInterface`: RabbitMQ communication layer
- `DBConnector`: Database connector base class
- `RedisConnector`: Redis database integration
- `NSBClient`: Base client class
- `NSBAppClient`: Application client (send/receive)
- `NSBSimClient`: Simulator client (fetch/post)

Key Features:
- 3-channel architecture (CTRL, SEND, RECV)
- Synchronous and asynchronous operations
- PULL and PUSH mode support
- SYSTEM_WIDE and PER_NODE simulator modes
- Redis database integration

### nsb_daemon.py
Daemon service for configuration management

Classes:
- `NSBDaemonConfig`: Configuration holder
- `NSBDaemon`: Configuration service

Features:
- Listens for INIT requests from clients
- Responds with system-wide configuration
- Handles PING and EXIT messages
- Graceful shutdown with signal handling

### IMPLEMENTATION_SUMMARY.md
High-level implementation overview

Contents:
- Overview of all created files
- Key features checklist
- Queue architecture diagram
- Message flow examples
- Configuration examples
- Comparison with socket implementation
- Performance characteristics
- Validation checklist

### TODO.txt
Implementation status and future enhancements

Contents:
- Completed items
- Future enhancements

## Example and Testing Files

### example_usage.py
Working examples demonstrating all features

Examples:
1. Basic send/receive between clients
2. Simulator fetch/post workflow
3. Multiple client communication
4. Asynchronous listening
5. Database integration

Can be run directly to test the implementation:
```bash
python example_usage.py
```

## Quick Start

### 1. Prerequisites
```bash
pip install pika redis
```

### 2. Start RabbitMQ
```bash
docker run -d --name rabbitmq -p 5672:5672 rabbitmq:latest
```

### 3. Start Daemon
```bash
python nsb_daemon.py
```

### 4. Run Examples
```bash
python example_usage.py
```

## Architecture Highlights

### Message Routing
- **Direct Exchange**: RabbitMQ direct exchange for routing
- **Queue Naming**: `nsb.{channel}.{client_id}` convention
- **Transient Messages**: Delivery mode 1 for speed (mode 2 for persistence)
- **Auto-Acknowledgment**: Messages acknowledged after processing

### Client Communication
- **Synchronous**: `send()`, `receive()`, `fetch()`, `post()`
- **Asynchronous**: `listen()` methods with asyncio
- **Timeout Support**: Configurable timeouts for all receive operations
- **Error Recovery**: Automatic retry on connection failures

### Configuration Management
- **Centralized**: Single daemon for all configuration
- **Per-Client**: Individual response queues for each client
- **Flexible**: Easy to modify system-wide settings
- **Persistent**: Configuration survives client disconnections

## Usage Patterns

### Application Client
```python
from nsb_rabbitmq import NSBAppClient

client = NSBAppClient("app_id", "localhost", 5672)
client.send("dest_id", b"payload")
msg = client.receive(timeout=5)
client.exit()
```

### Simulator Client
```python
from nsb_rabbitmq import NSBSimClient

sim = NSBSimClient("sim_id", "localhost", 5672)
msg = sim.fetch(timeout=5)
sim.post(msg.src_id, msg.dest_id, b"result")
sim.exit()
```

### Daemon Service
```python
from nsb_daemon import NSBDaemon, NSBDaemonConfig

config = NSBDaemonConfig(sys_mode=0, sim_mode=0, use_db=False)
daemon = NSBDaemon("localhost", 5672, config)
daemon.start()
```