"""
Example usage of unified NSB client with backend toggle.

Demonstrates how to use the unified NSB client with both socket and
RabbitMQ backends, showcasing the seamless backend switching capability.
"""

import time
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import nsb_unified as nsb


def example_socket_backend():
    """
    Example 1: Using Socket Backend
    
    This example demonstrates basic send/receive using the traditional
    socket-based backend.
    """
    print("\n" + "=" * 70)
    print("Example 1: Socket Backend")
    print("=" * 70)
    
    server_address = "localhost"
    server_port = 5555
    
    print(f"Creating clients with socket backend...")
    app1 = nsb.NSBAppClient("node1", server_address, server_port, backend="socket")
    app2 = nsb.NSBAppClient("node2", server_address, server_port, backend="socket")
    
    print(f"Configuration: {app1.cfg}")
    print(f"Backend: {app1.backend}")
    
    payload = b"Hello from node1 via socket!"
    print(f"\nSending message from node1 to node2...")
    app1.send("node2", payload)
    
    time.sleep(0.5)
    
    print(f"Receiving message at node2...")
    msg = app2.receive(timeout=5)
    
    if msg:
        print(f"[OK] Received: {msg.payload.decode()}")
        print(f"  From: {msg.src_id}")
        print(f"  To: {msg.dest_id}")
        print(f"  Size: {msg.payload_size} bytes")
    else:
        print("[FAIL] No message received")
    
    app1.exit()
    app2.exit()
    print("\n[DONE] Socket backend example completed")


def example_rabbitmq_backend():
    """
    Example 2: Using RabbitMQ Backend
    
    This example demonstrates the same send/receive pattern using the
    RabbitMQ-based backend.
    """
    print("\n" + "=" * 70)
    print("Example 2: RabbitMQ Backend")
    print("=" * 70)
    
    server_address = "localhost"
    server_port = 5672
    
    print(f"Creating clients with RabbitMQ backend...")
    app1 = nsb.NSBAppClient("node1", server_address, server_port, backend="rabbitmq")
    app2 = nsb.NSBAppClient("node2", server_address, server_port, backend="rabbitmq")
    
    print(f"Configuration: {app1.cfg}")
    print(f"Backend: {app1.backend}")
    
    payload = b"Hello from node1 via RabbitMQ!"
    print(f"\nSending message from node1 to node2...")
    app1.send("node2", payload)
    
    time.sleep(0.5)
    
    print(f"Receiving message at node2...")
    msg = app2.receive(timeout=5)
    
    if msg:
        print(f"[OK] Received: {msg.payload.decode()}")
        print(f"  From: {msg.src_id}")
        print(f"  To: {msg.dest_id}")
        print(f"  Size: {msg.payload_size} bytes")
    else:
        print("[FAIL] No message received")
    
    app1.exit()
    app2.exit()
    print("\n[DONE] RabbitMQ backend example completed")


def example_backend_comparison():
    """
    Example 3: Backend Comparison
    
    This example demonstrates that the same code works with both backends,
    only changing the backend parameter.
    """
    print("\n" + "=" * 70)
    print("Example 3: Backend Comparison")
    print("=" * 70)
    
    def send_receive_test(backend_name, server_addr, server_port):
        """Helper function to test send/receive with any backend."""
        print(f"\n--- Testing with {backend_name.upper()} backend ---")
        
        sender = nsb.NSBAppClient(
            f"sender_{backend_name}", server_addr, server_port, backend=backend_name
        )
        receiver = nsb.NSBAppClient(
            f"receiver_{backend_name}", server_addr, server_port, backend=backend_name
        )
        
        payload = f"Test message via {backend_name}".encode()
        
        start_time = time.time()
        sender.send(f"receiver_{backend_name}", payload)
        time.sleep(0.3)
        msg = receiver.receive(timeout=5)
        elapsed = time.time() - start_time
        
        if msg:
            print(f"[OK] Message delivered successfully")
            print(f"  Latency: {elapsed:.3f}s")
            print(f"  Payload: {msg.payload.decode()}")
        else:
            print(f"[FAIL] Message delivery failed")
        
        sender.exit()
        receiver.exit()
        
        return msg is not None
    
    socket_success = send_receive_test("socket", "localhost", 5555)
    rabbitmq_success = send_receive_test("rabbitmq", "localhost", 5672)
    
    print(f"\n--- Results ---")
    print(f"Socket backend: {'[PASS]' if socket_success else '[FAIL]'}")
    print(f"RabbitMQ backend: {'[PASS]' if rabbitmq_success else '[FAIL]'}")
    print("\n[DONE] Backend comparison completed")


def example_redis_integration():
    """
    Example 4: Redis Database Integration
    
    This example demonstrates using Redis for large payload storage
    with both backends.
    """
    print("\n" + "=" * 70)
    print("Example 4: Redis Database Integration")
    print("=" * 70)
    
    server_address = "localhost"
    server_port = 5672
    
    print(f"Creating clients with RabbitMQ backend and Redis...")
    app1 = nsb.NSBAppClient(
        "sender_redis", server_address, server_port, backend="rabbitmq"
    )
    app2 = nsb.NSBAppClient(
        "receiver_redis", server_address, server_port, backend="rabbitmq"
    )
    
    if not app1.cfg.use_db:
        print("[SKIP] Redis not enabled in daemon configuration")
        app1.exit()
        app2.exit()
        return
    
    print(f"[OK] Redis enabled at {app1.cfg.db_address}:{app1.cfg.db_port}")
    
    large_payload = b"X" * (100 * 1024)
    print(f"\nSending large payload ({len(large_payload)} bytes)...")
    
    key = app1.send("receiver_redis", large_payload)
    print(f"[OK] Stored in Redis with key: {key}")
    
    time.sleep(0.5)
    
    print(f"Receiving large payload...")
    msg = app2.receive(timeout=5)
    
    if msg:
        print(f"[OK] Received {len(msg.payload)} bytes")
        print(f"  Payload matches: {msg.payload == large_payload}")
    else:
        print("[FAIL] Failed to receive message")
    
    app1.exit()
    app2.exit()
    print("\n[DONE] Redis integration example completed")


def example_pull_vs_push_mode():
    """
    Example 5: PULL vs PUSH Mode Behavior
    
    This example demonstrates the difference between PULL and PUSH modes.
    """
    print("\n" + "=" * 70)
    print("Example 5: PULL vs PUSH Mode Behavior")
    print("=" * 70)
    
    server_address = "localhost"
    server_port = 5672
    
    app = nsb.NSBAppClient("mode_test", server_address, server_port, backend="rabbitmq")
    
    mode = app.cfg.system_mode
    print(f"Current system mode: {mode.name}")
    
    if mode == nsb.Config.SystemMode.PULL:
        print("\nPULL Mode Behavior:")
        print("- Clients actively request messages from daemon/broker")
        print("- Polling with timeout is common")
        print("- Good for intermittent communication patterns")
        
        print("\nDemonstrating polling...")
        for i in range(3):
            msg = app.receive(timeout=1)
            if msg:
                print(f"  Poll {i+1}: Message received")
            else:
                print(f"  Poll {i+1}: No message (expected)")
    
    elif mode == nsb.Config.SystemMode.PUSH:
        print("\nPUSH Mode Behavior:")
        print("- Daemon/broker automatically forwards messages to clients")
        print("- Lower latency for message delivery")
        print("- Good for real-time communication patterns")
        
        print("\nNote: In PUSH mode, receive() uses timeout=0 for immediate check")
    
    app.exit()
    print("\n[DONE] Mode behavior example completed")


def example_simulator_client():
    """
    Example 6: Simulator Client Usage
    
    This example demonstrates using the simulator client with both backends.
    """
    print("\n" + "=" * 70)
    print("Example 6: Simulator Client Usage")
    print("=" * 70)
    
    server_address = "localhost"
    server_port = 5672
    
    print(f"Creating application and simulator clients...")
    app = nsb.NSBAppClient("app_node", server_address, server_port, backend="rabbitmq")
    sim = nsb.NSBSimClient("simulator", server_address, server_port, backend="rabbitmq")
    
    print(f"Simulator mode: {sim.cfg.simulator_mode.name}")
    
    payload = b"Message to be simulated"
    print(f"\nApplication sending message...")
    app.send("destination", payload)
    
    time.sleep(0.5)
    
    print(f"Simulator fetching message...")
    msg = sim.fetch(timeout=5)
    
    if msg:
        print(f"[OK] Simulator fetched message")
        print(f"  From: {msg.src_id}")
        print(f"  To: {msg.dest_id}")
        print(f"  Payload: {msg.payload.decode()}")
        
        print(f"\nSimulator posting message after network simulation...")
        sim.post(msg.src_id, msg.dest_id, msg.payload)
        print(f"[OK] Message posted back to network")
    else:
        print("[FAIL] No message to fetch")
    
    app.exit()
    sim.exit()
    print("\n[DONE] Simulator client example completed")


def example_environment_based_backend():
    """
    Example 7: Environment-Based Backend Selection
    
    This example shows how to select backend based on environment variable,
    making it easy to switch backends via configuration.
    """
    print("\n" + "=" * 70)
    print("Example 7: Environment-Based Backend Selection")
    print("=" * 70)
    
    backend = os.environ.get("NSB_BACKEND", "socket")
    server_address = os.environ.get("NSB_SERVER", "localhost")
    
    if backend == "socket":
        server_port = int(os.environ.get("NSB_PORT", "5555"))
    else:
        server_port = int(os.environ.get("NSB_PORT", "5672"))
    
    print(f"Backend from environment: {backend}")
    print(f"Server: {server_address}:{server_port}")
    
    print(f"\nCreating client with environment-based configuration...")
    app = nsb.NSBAppClient("env_test", server_address, server_port, backend=backend)
    
    print(f"[OK] Client created successfully")
    print(f"  Backend: {app.backend}")
    print(f"  Configuration: {app.cfg}")
    
    print(f"\nTo use different backend, set environment variables:")
    print(f"  export NSB_BACKEND=rabbitmq")
    print(f"  export NSB_SERVER=localhost")
    print(f"  export NSB_PORT=5672")
    
    app.exit()
    print("\n[DONE] Environment-based backend example completed")


def main():
    """Run all examples."""
    print("\n" + "=" * 70)
    print("NSB Unified Client - Example Usage")
    print("=" * 70)
    print("\nThis script demonstrates the unified NSB client with backend toggle.")
    print("You can seamlessly switch between socket and RabbitMQ backends.")
    print("\nPrerequisites:")
    print("- NSB socket daemon running on localhost:5555")
    print("- RabbitMQ broker and NSB daemon on localhost:5672")
    print("- Redis server on localhost:6379 (optional, for Example 4)")
    print("=" * 70)
    
    examples = [
        ("Socket Backend", example_socket_backend),
        ("RabbitMQ Backend", example_rabbitmq_backend),
        ("Backend Comparison", example_backend_comparison),
        ("Redis Integration", example_redis_integration),
        ("PULL vs PUSH Mode", example_pull_vs_push_mode),
        ("Simulator Client", example_simulator_client),
        ("Environment-Based Backend", example_environment_based_backend),
    ]
    
    for i, (name, func) in enumerate(examples, 1):
        try:
            func()
        except Exception as e:
            print(f"\n[ERROR] Example {i} ({name}) failed: {e}")
            import traceback
            traceback.print_exc()
        
        if i < len(examples):
            print("\n" + "-" * 70)
            time.sleep(1)
    
    print("\n" + "=" * 70)
    print("All examples completed!")
    print("=" * 70)


if __name__ == "__main__":
    main()
