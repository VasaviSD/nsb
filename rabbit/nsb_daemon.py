"""
@file nsb_daemon.py
@module nsb_daemon
@brief RabbitMQ-based NSB Daemon Client for Configuration Management
@details This module provides a daemon client that runs separately and handles
         configuration management for NSB clients. When clients connect, they
         request configuration from this daemon, which responds with system-wide
         settings. After initialization, clients communicate directly through
         the RabbitMQ broker.
"""

import time
import logging
from enum import IntEnum
import threading
import signal
import sys

import pika
import pika.exceptions
import proto.nsb_pb2 as nsb_pb2

# Set up logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s.%(msecs)03d\t%(name)s\t%(levelname)s\t%(message)s",
    datefmt="%H:%M:%S",
    handlers=[
        logging.StreamHandler(),
    ],
)

logger = logging.getLogger("NSBDaemon")

## @var DAEMON_RESPONSE_TIMEOUT
# Maximum time a daemon will wait for operations.
DAEMON_RESPONSE_TIMEOUT = 600


class NSBDaemonConfig:
    """
    @brief Configuration for the NSB Daemon.

    This class holds the system-wide configuration that will be sent to
    clients upon their initialization.
    """

    def __init__(
        self,
        sys_mode: int = 0,
        sim_mode: int = 0,
        use_db: bool = False,
        db_address: str = "localhost",
        db_port: int = 6379,
        db_num: int = 0,
    ):
        """
        @brief Initializes daemon configuration.

        @param sys_mode System mode (0=PULL, 1=PUSH)
        @param sim_mode Simulator mode (0=SYSTEM_WIDE, 1=PER_NODE)
        @param use_db Whether to use Redis database
        @param db_address Redis server address
        @param db_port Redis server port
        @param db_num Redis database number
        """
        self.sys_mode = sys_mode
        self.sim_mode = sim_mode
        self.use_db = use_db
        self.db_address = db_address
        self.db_port = db_port
        self.db_num = db_num

    def to_protobuf(self) -> nsb_pb2.nsbm:
        """
        @brief Converts configuration to protobuf message.

        @return NSB message with config field populated
        """
        msg = nsb_pb2.nsbm()
        msg.config.sys_mode = self.sys_mode
        msg.config.sim_mode = self.sim_mode
        msg.config.use_db = self.use_db
        if self.use_db:
            msg.config.db_address = self.db_address
            msg.config.db_port = self.db_port
            msg.config.db_num = self.db_num
        return msg


class NSBDaemon:
    """
    @brief NSB Daemon for RabbitMQ-based configuration management.

    This daemon runs as a separate service and listens for INIT requests from
    NSB clients. When a client sends an INIT message, the daemon responds with
    the system-wide configuration. The daemon also handles PING and EXIT messages.
    """

    def __init__(self, broker_address: str, broker_port: int, config: NSBDaemonConfig):
        """
        @brief Initializes the NSB Daemon.

        @param broker_address The address of the RabbitMQ broker
        @param broker_port The port of the RabbitMQ broker
        @param config The system-wide configuration to send to clients
        """
        self.broker_addr = broker_address
        self.broker_port = broker_port
        self.config = config
        self.logger = logging.getLogger(f"NSBDaemon({broker_address}:{broker_port})")

        self._connection = None
        self._channel = None
        self._running = False

        # Register signal handlers for graceful shutdown
        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)

    def _signal_handler(self, sig, frame):
        """
        @brief Handles shutdown signals.
        """
        self.logger.info("Received shutdown signal. Stopping daemon...")
        self._running = False
        self.stop()
        sys.exit(0)

    def _connect(self):
        """
        @brief Connects to the RabbitMQ broker.

        @exception RuntimeError Raised if connection fails
        """
        self.logger.info(
            f"Connecting to broker@{self.broker_addr}:{self.broker_port}..."
        )

        try:
            params = pika.ConnectionParameters(
                host=self.broker_addr,
                port=self.broker_port,
                connection_attempts=3,
                retry_delay=1,
            )
            self._connection = pika.BlockingConnection(params)
            self._channel = self._connection.channel()

            # Declare exchange and config queue
            queue_name = "nsb.config.request"

            self._channel.exchange_declare(
                exchange="nsb", exchange_type="direct", durable=True
            )
            self._channel.queue_declare(queue=queue_name, durable=True)
            self._channel.queue_bind(
                exchange="nsb",
                queue=queue_name,
                routing_key=queue_name,
            )

            self.logger.info(f"Connected to broker and config queue established.")
        except pika.exceptions.AMQPConnectionError as e:
            self.logger.error(f"Failed to connect to broker: {e}")
            raise RuntimeError(f"Failed to connect to RabbitMQ broker: {e}")

    def _handle_init(self, client_id: str, response_queue: str):
        """
        @brief Handles INIT request from a client.

        @param client_id The identifier of the client
        @param response_queue The queue to send the response to
        """
        self.logger.info(f"Handling INIT request from {client_id}")

        # Create response message
        response = nsb_pb2.nsbm()
        response.manifest.op = nsb_pb2.nsbm.Manifest.Operation.INIT
        response.manifest.og = nsb_pb2.nsbm.Manifest.Originator.DAEMON
        response.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.SUCCESS

        # Populate config
        response.config.sys_mode = self.config.sys_mode
        response.config.sim_mode = self.config.sim_mode
        response.config.use_db = self.config.use_db
        if self.config.use_db:
            response.config.db_address = self.config.db_address
            response.config.db_port = self.config.db_port
            response.config.db_num = self.config.db_num

        # Send response to client's config response queue
        try:
            self._channel.basic_publish(
                exchange="nsb",
                routing_key=response_queue,
                body=response.SerializeToString(),
                properties=pika.BasicProperties(delivery_mode=2),
            )
            self.logger.info(f"Sent INIT response to {client_id}")
        except Exception as e:
            self.logger.error(f"Failed to send INIT response: {e}")

    def _handle_ping(self, client_id: str, response_queue: str):
        """
        @brief Handles PING request from a client.

        @param client_id The identifier of the client
        @param response_queue The queue to send the response to
        """
        self.logger.debug(f"Handling PING request from {client_id}")

        # Create response message
        response = nsb_pb2.nsbm()
        response.manifest.op = nsb_pb2.nsbm.Manifest.Operation.PING
        response.manifest.og = nsb_pb2.nsbm.Manifest.Originator.DAEMON
        response.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.SUCCESS

        # Send response
        try:
            self._channel.basic_publish(
                exchange="nsb",
                routing_key=response_queue,
                body=response.SerializeToString(),
                properties=pika.BasicProperties(delivery_mode=2),
            )
            self.logger.debug(f"Sent PING response to {client_id}")
        except Exception as e:
            self.logger.error(f"Failed to send PING response: {e}")

    def _handle_exit(self, client_id: str):
        """
        @brief Handles EXIT request from a client.

        @param client_id The identifier of the client
        """
        self.logger.info(f"Handling EXIT request from {client_id}")

    def _process_message(self, method, properties, body):
        """
        @brief Processes an incoming message from the config request queue.

        @param method The method frame
        @param properties The properties frame
        @param body The message body
        """
        try:
            # Parse the message
            msg = nsb_pb2.nsbm()
            msg.ParseFromString(body)

            client_id = msg.intro.identifier
            response_queue = f"nsb.config.response.{client_id}"

            # Handle based on operation
            if msg.manifest.op == nsb_pb2.nsbm.Manifest.Operation.INIT:
                self._handle_init(client_id, response_queue)
            elif msg.manifest.op == nsb_pb2.nsbm.Manifest.Operation.PING:
                self._handle_ping(client_id, response_queue)
            elif msg.manifest.op == nsb_pb2.nsbm.Manifest.Operation.EXIT:
                self._handle_exit(client_id)
            else:
                self.logger.warning(f"Unknown operation: {msg.manifest.op}")
        except Exception as e:
            self.logger.error(f"Error processing message: {e}")

    def start(self):
        """
        @brief Starts the daemon and begins listening for client requests.
        """
        self._connect()
        self._running = True

        queue_name = "nsb.config.request"

        self.logger.info("NSB Daemon started. Listening for client requests...")

        try:
            # Start consuming messages
            self._channel.basic_consume(
                queue=queue_name,
                on_message_callback=self._on_message,
                auto_ack=True,
            )
            self._channel.start_consuming()
        except KeyboardInterrupt:
            self.logger.info("Received shutdown signal. Stopping daemon...")
            self.stop()
        except Exception as e:
            self.logger.error(f"Daemon error: {e}")
            self.stop()

    def _on_message(self, ch, method, properties, body):
        """
        @brief Callback for when a message is received.

        @param ch The channel
        @param method The method frame
        @param properties The properties frame
        @param body The message body
        """
        self._process_message(method, properties, body)

    def stop(self):
        """
        @brief Stops the daemon and closes connections.
        """
        self.logger.info("Stopping NSB Daemon")
        self._running = False

        try:
            if self._channel and self._channel.is_open:
                self._channel.stop_consuming()
                self._channel.close()
            if self._connection and self._connection.is_open:
                self._connection.close()
        except Exception as e:
            self.logger.error(f"Error stopping daemon: {e}")

        self.logger.info("NSB Daemon stopped.")


def main():
    """
    @brief Main entry point for the NSB Daemon.

    This function creates a daemon with default configuration and starts it.
    You can customize the configuration by modifying the NSBDaemonConfig parameters.
    """
    import argparse

    parser = argparse.ArgumentParser(description="NSB RabbitMQ Daemon")
    parser.add_argument(
        "--broker-host",
        type=str,
        default="localhost",
        help="RabbitMQ broker host (default: localhost)",
    )
    parser.add_argument(
        "--broker-port",
        type=int,
        default=5672,
        help="RabbitMQ broker port (default: 5672)",
    )
    parser.add_argument(
        "--mode",
        type=int,
        default=0,
        choices=[0, 1],
        help="System mode: 0=PULL, 1=PUSH (default: 0)",
    )
    parser.add_argument(
        "--sim-mode",
        type=int,
        default=0,
        choices=[0, 1],
        help="Simulator mode: 0=SYSTEM_WIDE, 1=PER_NODE (default: 0)",
    )
    parser.add_argument(
        "--redis",
        action="store_true",
        default=True,
        help="Enable Redis database (default: True)",
    )
    parser.add_argument(
        "--no-redis",
        action="store_false",
        dest="redis",
        help="Disable Redis database",
    )
    parser.add_argument(
        "--redis-host",
        type=str,
        default="localhost",
        help="Redis host (default: localhost)",
    )
    parser.add_argument(
        "--redis-port",
        type=int,
        default=6379,
        help="Redis port (default: 6379)",
    )

    args = parser.parse_args()

    # Create configuration from arguments
    config = NSBDaemonConfig(
        sys_mode=args.mode,
        sim_mode=args.sim_mode,
        use_db=args.redis,
        db_address=args.redis_host,
        db_port=args.redis_port,
    )

    mode_name = "PULL" if args.mode == 0 else "PUSH"
    logger.info(f"Starting NSB Daemon in {mode_name} mode")
    logger.info(
        f"Connecting to RabbitMQ broker at {args.broker_host}:{args.broker_port}"
    )
    if args.redis:
        logger.info(f"Redis enabled at {args.redis_host}:{args.redis_port}")

    # Create and start daemon
    daemon = NSBDaemon(args.broker_host, args.broker_port, config)
    daemon.start()


if __name__ == "__main__":
    main()
