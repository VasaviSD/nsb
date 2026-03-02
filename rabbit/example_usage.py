"""
@file example_usage.py
@brief Example usage of NSB RabbitMQ implementation

This file demonstrates how to use the RabbitMQ-based NSB clients.
Before running this example, make sure:
1. RabbitMQ is running (e.g., docker run -d --name rabbitmq -p 5672:5672 rabbitmq:latest)
2. The NSB daemon is running (python nsb_daemon.py)
"""

import time
import asyncio
from nsb_rabbitmq import NSBAppClient, NSBSimClient


def example_basic_send_receive():
    """
    @brief Example: Basic send and receive between two application clients.
    """
    print("\nExample 1: Basic Send/Receive")
    
    # Create two application clients
    sender = NSBAppClient("sender", "localhost", 5672)
    receiver = NSBAppClient("receiver", "localhost", 5672)
    
    # Sender sends a message
    payload = b"Hello from sender"
    sender.send("receiver", payload)
    print(f"Sender: Sent '{payload.decode()}' to receiver")
    
    # Receiver gets the message
    msg = receiver.receive(timeout=5)
    if msg:
        print(f"Receiver: Got message from {msg.src_id}: {msg.payload.decode()}")
    else:
        print("Receiver: No message received (timeout)")
    
    # Cleanup
    sender.exit()
    receiver.exit()


def example_simulator_fetch_post():
    """
    @brief Example: Simulator fetching and posting messages.
    """
    print("\nExample 2: Simulator Fetch/Post")
    
    # Create app client and simulator
    app = NSBAppClient("app_node", "localhost", 5672)
    sim = NSBSimClient("simulator", "localhost", 5672)
    
    # App sends a message
    payload = b"Packet to be simulated"
    app.send("app_node", payload)
    print(f"App: Sent '{payload.decode()}' for simulation")
    
    # Simulator fetches the message
    msg = sim.fetch(timeout=5)
    if msg:
        print(f"Simulator: Fetched message from {msg.src_id} to {msg.dest_id}")
        print(f"Simulator: Payload: {msg.payload.decode()}")
        
        # Simulate processing (e.g., add latency, modify packet)
        processed_payload = b"[SIMULATED] " + msg.payload
        
        # Post the result back
        sim.post(msg.src_id, msg.dest_id, processed_payload)
        print(f"Simulator: Posted processed message back")
    else:
        print("Simulator: No message to fetch (timeout)")
    
    # App receives the processed message
    result = app.receive(timeout=5)
    if result:
        print(f"App: Received processed message: {result.payload.decode()}")
    else:
        print("App: No processed message received (timeout)")
    
    # Cleanup
    app.exit()
    sim.exit()


def example_multiple_clients():
    """
    @brief Example: Multiple clients communicating.
    """
    print("\nExample 3: Multiple Clients")
    
    # Create multiple clients
    clients = {
        "node_0": NSBAppClient("node_0", "localhost", 5672),
        "node_1": NSBAppClient("node_1", "localhost", 5672),
        "node_2": NSBAppClient("node_2", "localhost", 5672),
    }
    
    # Send messages in a chain
    messages = [
        ("node_0", "node_1", b"Message from node_0"),
        ("node_1", "node_2", b"Message from node_1"),
        ("node_2", "node_0", b"Message from node_2"),
    ]
    
    for src, dst, payload in messages:
        clients[src].send(dst, payload)
        print(f"{src} -> {dst}: {payload.decode()}")
    
    # Receive messages
    time.sleep(1)  # Give time for messages to propagate
    
    for node_id, client in clients.items():
        msg = client.receive(timeout=2)
        if msg:
            print(f"{node_id} received from {msg.src_id}: {msg.payload.decode()}")
        else:
            print(f"{node_id} received nothing")
    
    # Cleanup
    for client in clients.values():
        client.exit()


async def example_async_listen():
    """
    @brief Example: Asynchronous listening for messages.
    """
    print("\nExample 4: Async Listen")
    
    # Create clients
    sender = NSBAppClient("async_sender", "localhost", 5672)
    listener = NSBAppClient("async_listener", "localhost", 5672)
    
    # Define async listener task
    async def listen_task():
        print("Listener: Starting async listen...")
        for i in range(3):
            msg = await listener.listen()
            if msg:
                print(f"Listener: Got message {i+1}: {msg.payload.decode()}")
            else:
                print(f"Listener: Timeout on message {i+1}")
    
    # Define async sender task
    async def send_task():
        await asyncio.sleep(0.5)  # Let listener start
        for i in range(3):
            payload = f"Async message {i+1}".encode()
            sender.send("async_listener", payload)
            print(f"Sender: Sent '{payload.decode()}'")
            await asyncio.sleep(0.5)
    
    # Run both tasks concurrently
    await asyncio.gather(listen_task(), send_task())
    
    # Cleanup
    sender.exit()
    listener.exit()


def example_with_database():
    """
    @brief Example: Using Redis database for large payloads.
    
    Note: This example assumes the daemon is configured with use_db=True
    and Redis is running.
    """
    print("\n=== Example 5: With Database (Redis) ===")
    print("Note: Requires daemon with use_db=True and Redis running")
    
    try:
        # Create clients
        sender = NSBAppClient("db_sender", "localhost", 5672)
        receiver = NSBAppClient("db_receiver", "localhost", 5672)
        
        # Check if database is enabled
        if sender.cfg.use_db:
            # Create a large payload
            large_payload = b"X" * 1000000  # 1MB payload
            print(f"Sender: Sending large payload ({len(large_payload)} bytes)")
            
            # Send the message (will be stored in Redis)
            sender.send("db_receiver", large_payload)
            
            # Receive the message (will be retrieved from Redis)
            msg = receiver.receive(timeout=5)
            if msg:
                print(f"Receiver: Got large payload ({len(msg.payload)} bytes)")
            else:
                print("Receiver: No message received (timeout)")
        else:
            print("Database is not enabled in daemon configuration")
        
        # Cleanup
        sender.exit()
        receiver.exit()
    except Exception as e:
        print(f"Error: {e}")


def main():
    """
    @brief Run all examples.
    """
    print("NSB RabbitMQ Implementation Examples")
    print("Make sure the RabbitMQ broker **AND** the NSB daemon are running.")
    
    try:
        # Run synchronous examples
        example_basic_send_receive()
        time.sleep(1)
        
        example_simulator_fetch_post()
        time.sleep(1)
        
        example_multiple_clients()
        time.sleep(1)
        
        # Run async example
        asyncio.run(example_async_listen())
        time.sleep(1)
        
        # Run database example (may fail if not configured)
        try:
            example_with_database()
        except Exception as e:
            print(f"Database example skipped: {e}")
        
        print("\nAll examples completed")
    except Exception as e:
        print(f"\nError running examples: {e}")
        import traceback
        traceback.print_exc()


if __name__ == "__main__":
    main()
