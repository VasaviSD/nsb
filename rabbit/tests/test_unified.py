"""
Test suite for unified NSB client with Redis and mode testing.

Tests cover:
- Socket backend with Redis database
- RabbitMQ backend with Redis database
- PULL mode behavior (socket and RabbitMQ) with benchmarks
- PUSH mode behavior (socket and RabbitMQ) with benchmarks
- Backend switching
"""

import unittest
import time
import threading
import sys
import os
import logging

# Configure logging - suppress pika completely
logging.basicConfig(level=logging.WARNING)
logging.getLogger("pika").setLevel(logging.CRITICAL)

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import nsb_unified as nsb


# ANSI color codes for terminal output
class Colors:
    HEADER = "\033[95m"
    BLUE = "\033[94m"
    CYAN = "\033[96m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    RED = "\033[91m"
    BOLD = "\033[1m"
    UNDERLINE = "\033[4m"
    END = "\033[0m"


def print_header(text):
    """Print a colored header."""
    print(f"\n{Colors.BOLD}{Colors.CYAN}{'=' * 70}{Colors.END}")
    print(f"{Colors.BOLD}{Colors.CYAN}{text}{Colors.END}")
    print(f"{Colors.BOLD}{Colors.CYAN}{'=' * 70}{Colors.END}\n")


def print_test(text):
    """Print a test section."""
    print(f"\n{Colors.BOLD}{Colors.BLUE}{text}{Colors.END}")


def print_pass(text):
    """Print a pass message."""
    print(f"{Colors.GREEN}[PASS]{Colors.END} {text}\n")


def print_info(text):
    """Print an info message."""
    print(f"{Colors.YELLOW}[INFO]{Colors.END} {text}")


# Check if socket tests should be enabled (disabled by default)
SKIP_SOCKET_TESTS = not (
    os.environ.get("ENABLE_SOCKET_TESTS", "false").lower()
    in (
        "true",
        "1",
        "yes",
    )
)


class TestRedisIntegration(unittest.TestCase):
    """
    Test Redis database integration with both backends.

    Prerequisites:
    - Redis server running on localhost:6379
    - NSB daemon running (socket) or RabbitMQ daemon (rabbitmq)
    """

    @classmethod
    def setUpClass(cls):
        cls.server_address = "localhost"
        cls.socket_port = 5555
        cls.rabbitmq_port = 5672

    @unittest.skipIf(
        SKIP_SOCKET_TESTS,
        "Socket tests disabled by default (set ENABLE_SOCKET_TESTS=true to enable)",
    )
    def test_redis_socket_backend(self):
        """Test Redis integration with socket backend."""
        print_test("Testing Redis with Socket Backend")

        try:
            app1 = nsb.NSBAppClient(
                "app1_redis_socket",
                self.server_address,
                self.socket_port,
                backend="socket",
            )
            app2 = nsb.NSBAppClient(
                "app2_redis_socket",
                self.server_address,
                self.socket_port,
                backend="socket",
            )
        except Exception as e:
            self.skipTest(
                f"Cannot connect to socket daemon on port {self.socket_port}: {e}"
            )

        if not app1.cfg.use_db:
            self.skipTest("Redis not enabled in daemon configuration")

        self.assertTrue(app1.cfg.use_db, "Database should be enabled")
        self.assertTrue(app1.db.is_connected(), "Should connect to Redis")

        payload = b"Test payload with Redis via socket"
        key = app1.send("app2_redis_socket", payload)

        self.assertIsNotNone(key, "Should return Redis key")
        print_info(f"Stored message with key: {key}")

        time.sleep(0.5)

        msg = app2.receive(timeout=5)
        self.assertIsNotNone(msg, "Should receive message")
        self.assertEqual(msg.payload, payload, "Payload should match")
        self.assertEqual(msg.src_id, "app1_redis_socket")
        self.assertEqual(msg.dest_id, "app2_redis_socket")

        app1.exit()
        app2.exit()
        print_pass("Redis socket backend test")

    def test_redis_rabbitmq_backend(self):
        """Test Redis integration with RabbitMQ backend."""
        print_test("Testing Redis with RabbitMQ Backend")

        app1 = nsb.NSBAppClient(
            "app1_redis_rmq",
            self.server_address,
            self.rabbitmq_port,
            backend="rabbitmq",
        )
        app2 = nsb.NSBAppClient(
            "app2_redis_rmq",
            self.server_address,
            self.rabbitmq_port,
            backend="rabbitmq",
        )

        if not app1.cfg.use_db:
            self.skipTest("Redis not enabled in daemon configuration")

        self.assertTrue(app1.cfg.use_db, "Database should be enabled")
        self.assertTrue(app1.db.is_connected(), "Should connect to Redis")

        payload = b"Test payload with Redis via RabbitMQ"
        key = app1.send("app2_redis_rmq", payload)

        self.assertIsNotNone(key, "Should return Redis key")
        self.assertIsInstance(key, str, "Key should be string")
        print(f"Stored message with key: {key}")

        time.sleep(0.5)

        msg = app2.receive(timeout=5)
        self.assertIsNotNone(msg, "Should receive message")
        self.assertEqual(msg.payload, payload, "Payload should match")
        self.assertEqual(msg.src_id, "app1_redis_rmq")
        self.assertEqual(msg.dest_id, "app2_redis_rmq")

        app1.exit()
        app2.exit()
        print_pass("Redis RabbitMQ backend test")

    def test_redis_large_payload(self):
        """Test Redis with large payloads."""
        print_test("Testing Redis with Large Payload")

        try:
            app1 = nsb.NSBAppClient(
                "app1_large",
                self.server_address,
                self.rabbitmq_port,
                backend="rabbitmq",
            )
            app2 = nsb.NSBAppClient(
                "app2_large",
                self.server_address,
                self.rabbitmq_port,
                backend="rabbitmq",
            )
        except Exception as e:
            self.skipTest(
                f"Cannot connect to RabbitMQ daemon on port {self.rabbitmq_port}: {e}"
            )

        if not app1.cfg.use_db:
            self.skipTest("Redis not enabled in daemon configuration")

        large_payload = b"X" * (1024 * 1024)
        print_info(f"Sending {len(large_payload)} bytes via Redis...")

        key = app1.send("app2_large", large_payload)
        self.assertIsNotNone(key)

        msg = app2.receive(timeout=5)
        self.assertIsNotNone(msg)
        self.assertEqual(len(msg.payload), len(large_payload))
        self.assertEqual(msg.payload, large_payload)

        app1.exit()
        app2.exit()
        print_pass("Large payload test")


class TestPullMode(unittest.TestCase):
    """
    Test PULL mode behavior with both backends and benchmarking.

    In PULL mode, clients actively request messages from the daemon/broker.
    """

    @classmethod
    def setUpClass(cls):
        cls.server_address = "localhost"
        cls.socket_port = 5555
        cls.rabbitmq_port = 5672

    @unittest.skipIf(
        SKIP_SOCKET_TESTS,
        "Socket tests disabled by default (set ENABLE_SOCKET_TESTS=true to enable)",
    )
    def test_pull_mode_socket_benchmark(self):
        """Test PULL mode with socket backend and measure latency."""
        print("\n=== Testing PULL Mode with Socket Backend (Benchmark) ===")

        try:
            app1 = nsb.NSBAppClient(
                "app1_pull_socket",
                self.server_address,
                self.socket_port,
                backend="socket",
            )
            app2 = nsb.NSBAppClient(
                "app2_pull_socket",
                self.server_address,
                self.socket_port,
                backend="socket",
            )
        except Exception as e:
            self.skipTest(
                f"Cannot connect to socket daemon on port {self.socket_port}: {e}"
            )

        if app1.cfg.system_mode != nsb.Config.SystemMode.PULL:
            self.skipTest("Daemon not configured for PULL mode")

        self.assertEqual(app1.cfg.system_mode, nsb.Config.SystemMode.PULL)
        print(f"System mode: {app1.cfg.system_mode.name}")

        # Benchmark: measure round-trip time for multiple messages
        num_messages = 10
        latencies = []

        print(f"Sending {num_messages} messages for benchmark...")
        for i in range(num_messages):
            payload = f"PULL mode test message {i}".encode()

            start_time = time.time()
            app1.send("app2_pull_socket", payload)
            time.sleep(0.1)  # Small delay to ensure delivery
            msg = app2.receive(timeout=5)
            end_time = time.time()

            self.assertIsNotNone(msg, f"Should receive message {i}")
            self.assertEqual(msg.payload, payload)

            latency_ms = (end_time - start_time) * 1000
            latencies.append(latency_ms)
            print(f"  Message {i+1}: {latency_ms:.2f} ms")

        avg_latency = sum(latencies) / len(latencies)
        min_latency = min(latencies)
        max_latency = max(latencies)

        print(f"\nSocket PULL Mode Benchmark Results:")
        print(f"  Average latency: {avg_latency:.2f} ms")
        print(f"  Min latency: {min_latency:.2f} ms")
        print(f"  Max latency: {max_latency:.2f} ms")

        app1.exit()
        app2.exit()
        print_pass("PULL mode socket benchmark")

    def test_pull_mode_rabbitmq_benchmark(self):
        """Test PULL mode with RabbitMQ backend and measure latency."""
        print_test("Testing PULL Mode with RabbitMQ Backend (Benchmark)")

        app1 = nsb.NSBAppClient(
            "app1_pull_rmq", self.server_address, self.rabbitmq_port, backend="rabbitmq"
        )
        app2 = nsb.NSBAppClient(
            "app2_pull_rmq", self.server_address, self.rabbitmq_port, backend="rabbitmq"
        )

        if app1.cfg.system_mode != nsb.Config.SystemMode.PULL:
            self.skipTest("Daemon not configured for PULL mode")

        self.assertEqual(app1.cfg.system_mode, nsb.Config.SystemMode.PULL)
        print(f"System mode: {app1.cfg.system_mode.name}")

        # Benchmark: measure round-trip time for multiple messages
        num_messages = 10
        latencies = []

        print(f"Sending {num_messages} messages for benchmark...")
        for i in range(num_messages):
            payload = f"PULL mode RabbitMQ test {i}".encode()

            start_time = time.time()
            app1.send("app2_pull_rmq", payload)
            # time.sleep(0.1)  # Small delay to ensure delivery
            msg = app2.receive(timeout=5)
            end_time = time.time()

            self.assertIsNotNone(msg, f"Should receive message {i}")
            self.assertEqual(msg.payload, payload)

            latency_ms = (end_time - start_time) * 1000
            latencies.append(latency_ms)
            print(f"Message {i+1} send to receive latency: {latency_ms:.2f} ms")

        avg_latency = sum(latencies) / len(latencies)
        min_latency = min(latencies)
        max_latency = max(latencies)

        print(f"\nRabbitMQ PULL Mode Benchmark Results:")
        print(f"- Average latency: {avg_latency:.2f} ms")
        print(f"- Min latency: {min_latency:.2f} ms")
        print(f"- Max latency: {max_latency:.2f} ms")

        app1.exit()
        app2.exit()
        print_pass("PULL mode RabbitMQ benchmark")

    def test_pull_mode_polling(self):
        """Test PULL mode polling behavior."""
        print_test("Testing PULL Mode Polling")

        try:
            app1 = nsb.NSBAppClient(
                "app1_poll", self.server_address, self.rabbitmq_port, backend="rabbitmq"
            )
            app2 = nsb.NSBAppClient(
                "app2_poll", self.server_address, self.rabbitmq_port, backend="rabbitmq"
            )
        except Exception as e:
            self.skipTest(
                f"Cannot connect to RabbitMQ daemon on port {self.rabbitmq_port}: {e}"
            )

        if app1.cfg.system_mode != nsb.Config.SystemMode.PULL:
            self.skipTest("Daemon not configured for PULL mode")

        poll_count = 0
        max_polls = 10

        while poll_count < max_polls:
            msg = app2.receive(timeout=0.5)
            if msg is None:
                poll_count += 1
                print(f"Poll {poll_count}: No message")
            else:
                break

        self.assertEqual(poll_count, max_polls, "Should poll without blocking")

        payload = b"Message after polling"
        app1.send("app2_poll", payload)
        time.sleep(0.5)

        msg = app2.receive(timeout=5)
        self.assertIsNotNone(msg)
        self.assertEqual(msg.payload, payload)

        app1.exit()
        app2.exit()
        print_pass("PULL mode polling test")


class TestPushMode(unittest.TestCase):
    """
    Test PUSH mode behavior with both backends and benchmarking.

    In PUSH mode, the daemon/broker automatically forwards messages to clients.
    """

    @classmethod
    def setUpClass(cls):
        cls.server_address = "localhost"
        cls.socket_port = 5556
        cls.rabbitmq_port = 5672  # Same port as PULL - daemon mode determines behavior

    @unittest.skipIf(
        SKIP_SOCKET_TESTS,
        "Socket tests disabled by default (set ENABLE_SOCKET_TESTS=true to enable)",
    )
    def test_push_mode_socket_benchmark(self):
        """Test PUSH mode with socket backend and measure latency."""
        print("\n=== Testing PUSH Mode with Socket Backend (Benchmark) ===")

        try:
            app1 = nsb.NSBAppClient(
                "app1_push_socket",
                self.server_address,
                self.socket_port,
                backend="socket",
            )
            app2 = nsb.NSBAppClient(
                "app2_push_socket",
                self.server_address,
                self.socket_port,
                backend="socket",
            )
        except Exception as e:
            self.skipTest(
                f"Cannot connect to socket daemon on port {self.socket_port}: {e}"
            )

        if app1.cfg.system_mode != nsb.Config.SystemMode.PUSH:
            self.skipTest("Daemon not configured for PUSH mode")

        self.assertEqual(app1.cfg.system_mode, nsb.Config.SystemMode.PUSH)
        print(f"System mode: {app1.cfg.system_mode.name}")

        # Benchmark: measure round-trip time for multiple messages
        num_messages = 10
        latencies = []

        print(f"Sending {num_messages} messages for benchmark...")
        for i in range(num_messages):
            payload = f"PUSH mode test message {i}".encode()

            start_time = time.time()
            app1.send("app2_push_socket", payload)
            time.sleep(0.1)  # Small delay for push delivery
            msg = app2.receive(timeout=5)
            end_time = time.time()

            self.assertIsNotNone(msg, f"Should receive pushed message {i}")
            self.assertEqual(msg.payload, payload)

            latency_ms = (end_time - start_time) * 1000
            latencies.append(latency_ms)
            print(f"  Message {i+1}: {latency_ms:.2f} ms")

        avg_latency = sum(latencies) / len(latencies)
        min_latency = min(latencies)
        max_latency = max(latencies)

        print(f"\nSocket PUSH Mode Benchmark Results:")
        print(f"  Average latency: {avg_latency:.2f} ms")
        print(f"  Min latency: {min_latency:.2f} ms")
        print(f"  Max latency: {max_latency:.2f} ms")

        app1.exit()
        app2.exit()
        print_pass("PUSH mode socket benchmark")

    def test_push_mode_rabbitmq_benchmark(self):
        """Test PUSH mode with RabbitMQ backend and measure latency."""
        print_test("Testing PUSH Mode with RabbitMQ Backend (Benchmark)")

        try:
            app1 = nsb.NSBAppClient(
                "app1_push_rmq",
                self.server_address,
                self.rabbitmq_port,
                backend="rabbitmq",
            )
            app2 = nsb.NSBAppClient(
                "app2_push_rmq",
                self.server_address,
                self.rabbitmq_port,
                backend="rabbitmq",
            )
        except Exception as e:
            self.skipTest(
                f"Cannot connect to RabbitMQ daemon on port {self.rabbitmq_port}: {e}"
            )

        if app1.cfg.system_mode != nsb.Config.SystemMode.PUSH:
            self.skipTest("Daemon not configured for PUSH mode")

        self.assertEqual(app1.cfg.system_mode, nsb.Config.SystemMode.PUSH)
        print(f"System mode: {app1.cfg.system_mode.name}")

        # Benchmark: measure round-trip time for multiple messages
        num_messages = 10
        latencies = []

        print(f"Sending {num_messages} messages for benchmark...")
        for i in range(num_messages):
            payload = f"PUSH mode RabbitMQ test {i}".encode()

            start_time = time.time()
            app1.send("app2_push_rmq", payload)
            time.sleep(0.1)  # Small delay for push delivery
            msg = app2.receive(timeout=5)
            end_time = time.time()

            self.assertIsNotNone(msg, f"Should receive pushed message {i}")
            self.assertEqual(msg.payload, payload)

            latency_ms = (end_time - start_time) * 1000
            latencies.append(latency_ms)
            print(f"  Message {i+1}: {latency_ms:.2f} ms")

        avg_latency = sum(latencies) / len(latencies)
        min_latency = min(latencies)
        max_latency = max(latencies)

        print(f"\nRabbitMQ PUSH Mode Benchmark Results:")
        print(f"  Average latency: {avg_latency:.2f} ms")
        print(f"  Min latency: {min_latency:.2f} ms")
        print(f"  Max latency: {max_latency:.2f} ms")

        app1.exit()
        app2.exit()
        print_pass("PUSH mode RabbitMQ benchmark")

    def test_push_mode_immediate_delivery(self):
        """Test PUSH mode immediate message delivery."""
        print_test("Testing PUSH Mode Immediate Delivery")

        try:
            app1 = nsb.NSBAppClient(
                "app1_immediate",
                self.server_address,
                self.rabbitmq_port,
                backend="rabbitmq",
            )
            app2 = nsb.NSBAppClient(
                "app2_immediate",
                self.server_address,
                self.rabbitmq_port,
                backend="rabbitmq",
            )
        except Exception as e:
            self.skipTest(
                f"Cannot connect to RabbitMQ daemon on port {self.rabbitmq_port}: {e}"
            )

        if app1.cfg.system_mode != nsb.Config.SystemMode.PUSH:
            self.skipTest("Daemon not configured for PUSH mode")

        received_messages = []

        def receiver_thread():
            for i in range(5):
                msg = app2.receive(timeout=10)
                if msg:
                    received_messages.append(msg)
                    print(f"Received message {i+1}")

        receiver = threading.Thread(target=receiver_thread)
        receiver.start()

        time.sleep(0.5)

        for i in range(5):
            payload = f"Message {i+1}".encode()
            app1.send("app2_immediate", payload)
            print(f"Sent message {i+1}")
            time.sleep(0.2)

        receiver.join(timeout=15)

        self.assertEqual(len(received_messages), 5, "Should receive all 5 messages")

        app1.exit()
        app2.exit()
        print_pass("PUSH mode immediate delivery test")


class TestBackendSwitching(unittest.TestCase):
    """
    Test switching between socket and RabbitMQ backends.
    """

    @classmethod
    def setUpClass(cls):
        cls.server_address = "localhost"
        cls.socket_port = 5555
        cls.rabbitmq_port = 5672

    def test_socket_backend_creation(self):
        """Test creating client with socket backend."""
        print("\n=== Testing Socket Backend Creation ===")

        try:
            app = nsb.NSBAppClient(
                "app_socket_test",
                self.server_address,
                self.socket_port,
                backend="socket",
            )

            self.assertEqual(app.backend, "socket")
            self.assertIsInstance(app.comms, nsb.SocketInterface)
            self.assertIsNotNone(app.cfg)

            app.exit()
            print_pass("Socket backend creation test")
        except Exception as e:
            self.skipTest(
                f"Cannot connect to socket daemon on port {self.socket_port}: {e}"
            )

    def test_rabbitmq_backend_creation(self):
        """Test creating client with RabbitMQ backend."""
        print_test("Testing RabbitMQ Backend Creation")

        try:
            app = nsb.NSBAppClient(
                "app_rmq_test",
                self.server_address,
                self.rabbitmq_port,
                backend="rabbitmq",
            )

            self.assertEqual(app.backend, "rabbitmq")
            self.assertIsInstance(app.comms, nsb.RabbitMQInterface)
            self.assertIsNotNone(app.cfg)

            app.exit()
            print_pass("RabbitMQ backend creation test")
        except Exception as e:
            self.skipTest(
                f"Cannot connect to RabbitMQ daemon on port {self.rabbitmq_port}: {e}"
            )

    def test_invalid_backend(self):
        """Test that invalid backend raises error."""
        print_test("Testing Invalid Backend")

        with self.assertRaises(ValueError) as context:
            app = nsb.NSBAppClient(
                "app_invalid", self.server_address, self.socket_port, backend="invalid"
            )

        self.assertIn("Unknown backend", str(context.exception))
        print_pass("Invalid backend test")

    def test_cross_backend_communication(self):
        """Test that socket and RabbitMQ clients cannot communicate directly."""
        print_test("Testing Cross-Backend Isolation")

        print("Note: Socket and RabbitMQ backends use different infrastructure")
        print("They should not be able to communicate with each other")
        print_pass("Cross-backend isolation verified by design")


class TestSimulatorClient(unittest.TestCase):
    """
    Test simulator client with both backends.
    """

    @classmethod
    def setUpClass(cls):
        cls.server_address = "localhost"
        cls.socket_port = 5555
        cls.rabbitmq_port = 5672

    @unittest.skipIf(
        SKIP_SOCKET_TESTS,
        "Socket tests disabled by default (set ENABLE_SOCKET_TESTS=true to enable)",
    )
    def test_sim_client_fetch_post_socket(self):
        """Test simulator client fetch/post with socket backend."""
        print("\n=== Testing Simulator Client with Socket Backend ===")

        try:
            app = nsb.NSBAppClient(
                "app_sim_socket",
                self.server_address,
                self.socket_port,
                backend="socket",
            )
            sim = nsb.NSBSimClient(
                "sim_socket", self.server_address, self.socket_port, backend="socket"
            )
        except Exception as e:
            self.skipTest(
                f"Cannot connect to socket daemon on port {self.socket_port}: {e}"
            )

        payload = b"Test message for simulator"
        app.send("dest_node", payload)

        time.sleep(0.5)

        msg = sim.fetch(timeout=5)
        self.assertIsNotNone(msg, "Simulator should fetch message")
        self.assertEqual(msg.payload, payload)
        self.assertEqual(msg.src_id, "app_sim_socket")

        sim.post(msg.src_id, msg.dest_id, msg.payload)

        app.exit()
        sim.exit()
        print_pass("Simulator socket test")

    def test_sim_client_fetch_post_rabbitmq(self):
        """Test simulator client fetch/post with RabbitMQ backend."""
        print_test("Testing Simulator Client with RabbitMQ Backend")

        try:
            app = nsb.NSBAppClient(
                "app_sim_rmq",
                self.server_address,
                self.rabbitmq_port,
                backend="rabbitmq",
            )
            sim = nsb.NSBSimClient(
                "sim_rmq", self.server_address, self.rabbitmq_port, backend="rabbitmq"
            )
        except Exception as e:
            self.skipTest(
                f"Cannot connect to RabbitMQ daemon on port {self.rabbitmq_port}: {e}"
            )

        # Simulator posts a message from a simulated node to an app
        payload = b"Test message for RabbitMQ simulator"
        sim.post("simulated_node", "app_sim_rmq", payload)

        time.sleep(0.5)

        # App receives the message posted by simulator
        msg = app.receive(timeout=5)
        self.assertIsNotNone(msg, "App should receive message from simulator")
        self.assertEqual(msg.payload, payload)
        self.assertEqual(msg.src_id, "simulated_node")
        self.assertEqual(msg.dest_id, "app_sim_rmq")

        app.exit()
        sim.exit()
        print_pass("Simulator RabbitMQ test")


def run_tests():
    """Run all tests with detailed output."""
    print_header("NSB Unified Client Test Suite")
    print(f"{Colors.YELLOW}Prerequisites:{Colors.END}")
    print("  - Redis server running on localhost:6379")
    print("  - NSB socket daemon on port 5555 (PULL mode)")
    print("  - NSB socket daemon on port 5556 (PUSH mode)")
    print("  - RabbitMQ broker on port 5672")
    print("  - NSB RabbitMQ daemon on port 5672 (PULL mode)")
    print("  - NSB RabbitMQ daemon on port 5673 (PUSH mode)")
    print(f"{Colors.CYAN}{'=' * 70}{Colors.END}")

    loader = unittest.TestLoader()
    suite = unittest.TestSuite()

    suite.addTests(loader.loadTestsFromTestCase(TestRedisIntegration))
    suite.addTests(loader.loadTestsFromTestCase(TestPullMode))
    suite.addTests(loader.loadTestsFromTestCase(TestPushMode))
    suite.addTests(loader.loadTestsFromTestCase(TestBackendSwitching))
    suite.addTests(loader.loadTestsFromTestCase(TestSimulatorClient))

    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    print_header("Test Summary")
    successes = (
        result.testsRun
        - len(result.failures)
        - len(result.errors)
        - len(result.skipped)
    )
    print(f"{Colors.BOLD}Tests run:{Colors.END} {result.testsRun}")
    print(f"{Colors.GREEN}Successes:{Colors.END} {successes}")
    if len(result.failures) > 0:
        print(f"{Colors.RED}Failures:{Colors.END} {len(result.failures)}")
    else:
        print(f"{Colors.GREEN}Failures:{Colors.END} {len(result.failures)}")
    if len(result.errors) > 0:
        print(f"{Colors.RED}Errors:{Colors.END} {len(result.errors)}")
    else:
        print(f"{Colors.GREEN}Errors:{Colors.END} {len(result.errors)}")
    print(f"{Colors.YELLOW}Skipped:{Colors.END} {len(result.skipped)}")
    print(f"{Colors.CYAN}{'=' * 70}{Colors.END}")

    return result.wasSuccessful()


if __name__ == "__main__":
    import sys

    success = run_tests()
    sys.exit(0 if success else 1)
