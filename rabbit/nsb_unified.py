"""
@file nsb_unified.py
@module nsb_unified
@brief Unified NSB Client with Socket and RabbitMQ Backend Support
@details This module provides a unified interface for NSB clients that can use
         either socket-based or RabbitMQ-based communication backends. The backend
         is selected at runtime via a parameter, allowing seamless switching between
         transport mechanisms without code changes.
"""

import logging
import socket
import select
import time
import asyncio
import threading
from enum import Enum, IntEnum

import pika
import redis

import proto.nsb_pb2 as nsb_pb2

# Suppress pika logging completely
logging.getLogger("pika").setLevel(logging.CRITICAL)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s.%(msecs)03d\t%(name)s\t%(levelname)s\t%(message)s",
    datefmt="%H:%M:%S",
    handlers=[
        logging.StreamHandler(),
    ],
)

SERVER_CONNECTION_TIMEOUT = 10
DAEMON_RESPONSE_TIMEOUT = 600
RECEIVE_BUFFER_SIZE = 4096
SEND_BUFFER_SIZE = 4096

### SYSTEM CONFIG ###


class Config:
    """
    @brief Class to maintain property codes.
    """

    class SystemMode(IntEnum):
        """
        @brief Denotes whether the NSB system is in *PUSH* or *PULL* mode.
        """

        PULL = 0
        PUSH = 1

    class SimulatorMode(IntEnum):
        """
        @brief Denotes whether a system-wide (SYSTEM_WIDE) simulator client will
        be used or multiple clients will be created, one per node (PER_NODE).
        """

        SYSTEM_WIDE = 0
        PER_NODE = 1

    def __init__(self, nsb_msg: nsb_pb2.nsbm):
        """
        @brief Initializes configuration with a NSB message.
        """
        self.system_mode = Config.SystemMode(nsb_msg.config.sys_mode)
        self.simulator_mode = Config.SimulatorMode(nsb_msg.config.sim_mode)
        self.use_db = nsb_msg.config.use_db
        if self.use_db:
            self.db_address = nsb_msg.config.db_address
            self.db_port = nsb_msg.config.db_port
            self.db_num = nsb_msg.config.db_num

    def __repr__(self):
        """
        @brief String representation of the configuration.
        """
        s = f"[CONFIG] System Mode: {self.system_mode.name} | Use DB? {self.use_db}"
        if self.use_db:
            s += f" | DB Address: {self.db_address} | DB Port: {self.db_port}"
        return s


class MessageEntry:
    """
    @brief Container for received/fetched messages.
    """

    def __init__(self, src_id, dest_id, payload):
        self.src_id = src_id
        self.dest_id = dest_id
        self.payload = payload
        self.payload_size = len(payload)


### COMMUNICATION INTERFACES ###


class Comms:
    """
    @brief Base class for communication interfaces.
    """

    class Channels(IntEnum):
        """
        @brief Shared enumeration to designate different channels.
        """

        CTRL = 0
        SEND = 1
        RECV = 2


class SocketInterface(Comms):
    """
    @brief Socket interface for client-server communication.
    """

    def __init__(self, server_address: str, server_port: int):
        self.server_addr = server_address
        self.server_port = server_port
        self.logger = logging.getLogger(f"SIF({self.server_addr}:{self.server_port})")
        self.conns = {}
        self._connect()

    def _connect(self, timeout: int = SERVER_CONNECTION_TIMEOUT):
        """
        @brief Connects to the daemon with the stored server address and port.
        """
        self.logger.info(
            f"Connecting to daemon@{self.server_addr}:{self.server_port}..."
        )
        target_time = time.time() + timeout
        for channel in Comms.Channels:
            self.logger.info(f"Configuring & connecting {channel.name}...")
            while time.time() < target_time:
                try:
                    conn = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    conn.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    conn.setsockopt(socket.SOL_TCP, socket.TCP_NODELAY, 1)
                    conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
                    conn.connect((self.server_addr, self.server_port))
                    self.conns[channel] = conn
                    break
                except socket.error as e:
                    self.logger.debug(f"\tRetrying connection...")
                    time.sleep(1)
            if time.time() >= target_time:
                raise TimeoutError(
                    f"Connection to server timed out after {timeout} seconds."
                )
        for _, conn in self.conns.items():
            conn.setblocking(False)
        self.logger.info("\tAll channels connected!")

    def _close(self):
        """
        @brief Healthily closes the socket connection.
        """
        for _, conn in self.conns.items():
            conn.shutdown(socket.SHUT_WR)
            conn.close()

    def _send_msg(self, channel: Comms.Channels, message: bytes, dest_id: str = None):
        """
        @brief Sends a message to the server.
        """
        _, ready_to_send, _ = select.select([], [self.conns[channel]], [])
        if ready_to_send:
            while len(message):
                bytes_sent = self.conns[channel].send(message, SEND_BUFFER_SIZE)
                if bytes_sent == 0:
                    raise RuntimeError("Socket connection broken, nothing sent.")
                message = message[bytes_sent:]
        else:
            self.logger.error("Socket not ready to send, cannot send message.")

    def _recv_msg(self, channel: Comms.Channels, timeout: int | None = None):
        """
        @brief Receives a message from the server.
        """
        args = [[self.conns[channel]], [], []]
        if timeout is not None:
            args.append(timeout)
        ready_to_read, _, _ = select.select(*args)
        if len(ready_to_read) == 0:
            self.logger.error(f"Timed out after {timeout} seconds.")
            return None
        elif len(ready_to_read) > 0:
            data = b""
            while True:
                try:
                    chunk = self.conns[channel].recv(RECEIVE_BUFFER_SIZE)
                    if not chunk:
                        break
                    data += chunk
                except BlockingIOError:
                    break
            return data
        return None


class RabbitMQInterface(Comms):
    """
    @brief RabbitMQ-based interface for client-server communication.
    """

    def __init__(self, server_address: str, server_port: int, client_id: str = None):
        import pika
        import pika.exceptions

        self.pika = pika

        self.server_addr = server_address
        self.server_port = server_port
        self.client_id = client_id
        self.logger = logging.getLogger(
            f"RabbitMQ({self.server_addr}:{self.server_port})"
        )
        self._connection = None
        self._channels = {}
        self._queues = {}
        self._config_queue = "nsb.config.request"  # Initialize config queue name
        self._connect()

    def _connect(self, timeout: int = SERVER_CONNECTION_TIMEOUT):
        """
        @brief Connects to the RabbitMQ broker.
        """
        self.logger.info(
            f"Connecting to rabbitbroker@{self.server_addr}:{self.server_port}..."
        )

        start_time = time.time()
        params = self.pika.ConnectionParameters(
            host=self.server_addr,
            port=self.server_port,
            connection_attempts=3,
            retry_delay=1,
        )

        while time.time() - start_time < timeout:
            try:
                self._connection = self.pika.BlockingConnection(params)

                for ch in Comms.Channels:
                    channel = self._connection.channel()
                    self._channels[ch] = channel

                    if ch == list(Comms.Channels)[0]:
                        channel.exchange_declare(
                            exchange="nsb", exchange_type="direct", durable=True
                        )

                    if ch == Comms.Channels.CTRL:
                        if not self.client_id:
                            raise RuntimeError(
                                "CTRL channel requires client_id for response queue"
                            )
                        qname = f"nsb.config.response.{self.client_id}"
                        channel.queue_declare(queue=qname, durable=True)
                        channel.queue_bind(
                            exchange="nsb", queue=qname, routing_key=qname
                        )
                        self._queues[ch] = qname
                    else:
                        if self.client_id:
                            qname = f"nsb.{ch.name.lower()}.{self.client_id}"
                        else:
                            qname = f"nsb.{ch.name.lower()}"
                        channel.queue_declare(queue=qname, durable=True)
                        channel.queue_bind(
                            exchange="nsb", queue=qname, routing_key=qname
                        )
                        self._queues[ch] = qname

                # Declare config request queue
                config_channel = self._channels[Comms.Channels.CTRL]
                config_queue = "nsb.config.request"

                config_channel.queue_declare(queue=config_queue, durable=True)
                config_channel.queue_bind(
                    exchange="nsb",
                    queue=config_queue,
                    routing_key=config_queue,
                )
                self._config_queue = config_queue

                self.logger.info("Connected and channels established.")
                return
            except self.pika.exceptions.AMQPConnectionError as e:
                self.logger.debug(f"Connection failed: {e}. Retrying...")
                time.sleep(1)

        raise TimeoutError(f"RabbitMQ connection timed out after {timeout} seconds.")

    def _close(self):
        """
        @brief Healthily closes the RabbitMQ connection.
        """
        try:
            for ch in self._channels.values():
                if ch and ch.is_open:
                    ch.close()
            if self._connection and self._connection.is_open:
                self._connection.close()
        except Exception as e:
            self.logger.error(f"Error closing connection: {e}")

    def _send_msg(self, channel: Comms.Channels, message: bytes, dest_id: str = None):
        """
        @brief Sends a message to the broker.
        """
        try:
            ch = self._channels[channel]

            if channel == Comms.Channels.CTRL:
                routing_key = self._config_queue  # Use the mode-specific config queue
            elif dest_id and channel == Comms.Channels.SEND:
                routing_key = f"nsb.recv.{dest_id}"
            else:
                routing_key = self._queues[channel]

            ch.basic_publish(
                exchange="nsb",
                routing_key=routing_key,
                body=message,
                properties=self.pika.BasicProperties(delivery_mode=1),
            )
            self.logger.debug(f"Published to {routing_key}")
        except Exception as e:
            self.logger.error(f"Send failed on {channel.name}: {e}")
            raise RuntimeError(f"RabbitMQ send failed: {e}")

    def _recv_msg(self, channel: Comms.Channels, timeout: int | None = None):
        """
        @brief Receives a message from the broker.
        """
        check_interval = 0.05

        qname = self._queues.get(channel)
        ch = self._channels.get(channel)
        if not ch or not qname:
            self.logger.error(f"No queue for channel {channel.name}")
            return None

        try:
            start_time = time.time()
            while True:
                method_frame, _, body = ch.basic_get(queue=qname, auto_ack=True)
                if method_frame:
                    self.logger.debug(f"Received from {qname}")
                    return body
                if timeout is not None and (time.time() - start_time) > timeout:
                    self.logger.debug(f"Timeout on recv from {channel.name}")
                    return None
                time.sleep(check_interval)
        except Exception as e:
            self.logger.error(f"Receive failed on {channel.name}: {e}")
            return None

    async def _listen_msg(self, channel: Comms.Channels):
        """
        @brief Asynchronously listens for a message.
        """
        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(None, self._recv_msg, channel, None)


### DATABASE CONNECTORS ###


class DBConnector:
    """
    @brief Base class for database connectors.
    """

    def __init__(self, client_id: str):
        self.client_id = client_id
        self.plctr = 0
        self.lock = threading.Lock()

    def generate_payload_id(self):
        """
        @brief Payload ID generator.
        """
        with self.lock:
            self.plctr = (self.plctr + 1) & 0xFFFFF
            tms = int(time.time() * 1000) & 0x1FFFFFFFFFF
            payload_id = f"{tms}-{self.client_id}-{self.plctr}"
            return payload_id


class RedisConnector(DBConnector):
    """
    @brief Connector for Redis Database.
    """

    def __init__(self, client_id: str, address: str, port: int):
        super().__init__(client_id)
        self.address = address
        self.port = port
        self.r = redis.Redis(host=self.address, port=self.port)

    def is_connected(self):
        """
        @brief Checks connection to the Redis server.
        """
        try:
            return self.r.ping()
        except redis.ConnectionError:
            return False

    def store(self, value: bytes):
        """
        @brief Stores a payload.
        """
        key = self.generate_payload_id()
        try:
            self.r.set(key, value)
            return key
        except ConnectionError as e:
            logging.error("Check that the Redis server is online.")
            raise e

    def check_out(self, key: str):
        """
        @brief Checks out a payload.
        """
        try:
            value = self.r.getdel(key)
            if isinstance(value, str):
                value = value.encode("latin1")
            return value
        except ConnectionError as e:
            logging.error("Check that the Redis server is online.")
            raise e

    def peek(self, key: str):
        """
        @brief Peeks at the payload at the given key.
        """
        try:
            return self.r.get(key)
        except ConnectionError as e:
            logging.error("Check that the Redis server is online.")
            raise e

    def __del__(self):
        """
        @brief Closes the Redis connection.
        """
        if self.is_connected():
            self.r.close()


### NSB CLIENT BASE CLASS ###


class NSBClient:
    """
    @brief NSB client base class with backend selection.
    """

    def __init__(
        self,
        server_address: str,
        server_port: int,
        backend: str = "socket",
        client_id: str = None,
    ):
        """
        @brief Base constructor for NSB Clients with backend selection.

        @param server_address The address of the NSB daemon/broker.
        @param server_port The port of the NSB daemon/broker.
        @param backend Communication backend: 'socket' or 'rabbitmq'.
        @param client_id Client identifier (required for RabbitMQ backend).
        """
        self.backend = backend.lower()

        if self.backend == "socket":
            self.comms = SocketInterface(server_address, server_port)
        elif self.backend == "rabbitmq":
            if not client_id:
                raise ValueError("RabbitMQ backend requires client_id parameter")
            self.comms = RabbitMQInterface(server_address, server_port, client_id)
        else:
            raise ValueError(
                f"Unknown backend: {backend}. Must be 'socket' or 'rabbitmq'"
            )

        self.og_indicator = None
        self.cfg = None
        self.db = None

    def initialize(self):
        """
        @brief Initializes configuration with the server/daemon.
        """
        if not hasattr(self, "_id"):
            raise RuntimeError("Client identifier (_id) not set.")

        client_id = self._id
        self.logger.info(f"Initializing {client_id} with {self.backend} backend...")

        nsb_msg = nsb_pb2.nsbm()
        nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.INIT
        nsb_msg.manifest.og = self.og_indicator
        nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.SUCCESS
        nsb_msg.intro.identifier = client_id

        if self.backend == "socket":
            nsb_msg.intro.address = self.comms.conns[Comms.Channels.CTRL].getsockname()[
                0
            ]
            nsb_msg.intro.ch_CTRL = self.comms.conns[Comms.Channels.CTRL].getsockname()[
                1
            ]
            nsb_msg.intro.ch_SEND = self.comms.conns[Comms.Channels.SEND].getsockname()[
                1
            ]
            nsb_msg.intro.ch_RECV = self.comms.conns[Comms.Channels.RECV].getsockname()[
                1
            ]
        else:
            nsb_msg.intro.address = self.comms.server_addr
            nsb_msg.intro.ch_CTRL = 0
            nsb_msg.intro.ch_SEND = 0
            nsb_msg.intro.ch_RECV = 0

        self.comms._send_msg(Comms.Channels.CTRL, nsb_msg.SerializeToString())
        self.logger.debug("Sent INIT. Waiting for response...")

        response = self.comms._recv_msg(
            Comms.Channels.CTRL, timeout=DAEMON_RESPONSE_TIMEOUT
        )
        if response:
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            if nsb_resp.manifest.op == nsb_pb2.nsbm.Manifest.Operation.INIT:
                if nsb_resp.manifest.code != nsb_pb2.nsbm.Manifest.OpCode.SUCCESS:
                    raise RuntimeError("INIT: Initialization failed.")
                if nsb_resp.HasField("config"):
                    self.cfg = Config(nsb_resp)
                    self.logger.info(f"{self.cfg}")
                    if self.cfg.use_db:
                        self.db = RedisConnector(
                            client_id, self.cfg.db_address, self.cfg.db_port
                        )
                        if not self.db.is_connected():
                            logging.error("Ensure that the Redis server is running.")
                            raise RuntimeError("Failed to connect to Redis database.")
                    return
        raise RuntimeError("Failed to initialize NSB client.")

    def ping(self, timeout: int = DAEMON_RESPONSE_TIMEOUT):
        """
        @brief Pings the server.
        """
        nsb_msg = nsb_pb2.nsbm()
        nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.PING
        nsb_msg.manifest.og = self.og_indicator
        nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.SUCCESS
        self.comms._send_msg(Comms.Channels.CTRL, nsb_msg.SerializeToString())
        self.logger.info("PING: Pinged server.")
        response = self.comms._recv_msg(Comms.Channels.CTRL, timeout=timeout)
        if response:
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            if nsb_resp.manifest.op == nsb_pb2.nsbm.Manifest.Operation.PING:
                if nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.SUCCESS:
                    self.logger.info("PING: Server is reachable and healthy.")
                    return True
                else:
                    self.logger.info("PING: Server has some issue, but is reachable.")
                    return False
        return False

    def exit(self):
        """
        @brief Instructs server and self to exit and shutdown.
        """
        nsb_msg = nsb_pb2.nsbm()
        nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.EXIT
        nsb_msg.manifest.og = self.og_indicator
        nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.SUCCESS
        self.comms._send_msg(Comms.Channels.CTRL, nsb_msg.SerializeToString())
        self.logger.info("EXIT: Sent command to server.")
        del self


### NSB APPLICATION CLIENT ###


class NSBAppClient(NSBClient):
    """
    @brief NSB Application Client interface with backend selection.
    """

    def __init__(
        self,
        identifier: str,
        server_address: str,
        server_port: int,
        backend: str = "socket",
    ):
        """
        @brief Constructs the NSB Application Client interface.

        @param identifier The identifier for this NSB application client.
        @param server_address The address of the NSB daemon/broker.
        @param server_port The port of the NSB daemon/broker.
        @param backend Communication backend: 'socket' or 'rabbitmq' (default: 'socket').
        """
        self._id = identifier
        self.logger = logging.getLogger(f"{self._id} (app)")
        super().__init__(server_address, server_port, backend, client_id=identifier)
        self.og_indicator = nsb_pb2.nsbm.Manifest.Originator.APP_CLIENT
        self.initialize()

    def send(self, dest_id: str, payload: bytes):
        """
        @brief Sends a payload to the specified destination via NSB.
        """
        nsb_msg = nsb_pb2.nsbm()
        nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.SEND
        nsb_msg.manifest.og = self.og_indicator
        nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.MESSAGE
        nsb_msg.metadata.src_id = self._id
        nsb_msg.metadata.dest_id = dest_id
        nsb_msg.metadata.payload_size = len(payload)

        key = None
        if self.cfg.use_db:
            key = self.db.store(payload)
            nsb_msg.msg_key = key
        else:
            nsb_msg.payload = payload

        self.comms._send_msg(
            Comms.Channels.SEND, nsb_msg.SerializeToString(), dest_id=dest_id
        )
        self.logger.info("SEND: Sent message + payload to server.")
        return key

    def receive(self, dest_id: str | None = None, timeout: int | None = None):
        """
        @brief Receives a payload via NSB.
        """
        if self.backend == "socket" and self.cfg.system_mode == Config.SystemMode.PULL:
            nsb_msg = nsb_pb2.nsbm()
            nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.RECEIVE
            nsb_msg.manifest.og = self.og_indicator
            nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.SUCCESS
            if dest_id:
                nsb_msg.metadata.dest_id = dest_id
            else:
                nsb_msg.metadata.dest_id = self._id
            self.comms._send_msg(Comms.Channels.RECV, nsb_msg.SerializeToString())
            self.logger.debug("RECEIVE: Polling the server.")
        elif (
            self.backend == "socket" and self.cfg.system_mode == Config.SystemMode.PUSH
        ):
            if timeout is not None and timeout != 0:
                self.logger.debug(
                    "RECEIVE: System is in PUSH mode, timeout will be overwritten to 0."
                )
            timeout = 0

        response = self.comms._recv_msg(Comms.Channels.RECV, timeout=timeout)
        if response:
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            if nsb_resp.manifest.op in [
                nsb_pb2.nsbm.Manifest.Operation.SEND,
                nsb_pb2.nsbm.Manifest.Operation.RECEIVE,
                nsb_pb2.nsbm.Manifest.Operation.FORWARD,
                nsb_pb2.nsbm.Manifest.Operation.POST,
            ]:
                if nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.MESSAGE:
                    if self.cfg.use_db:
                        payload = self.db.check_out(nsb_resp.msg_key)
                    else:
                        payload = nsb_resp.payload
                    self.logger.info(
                        f"RECEIVE: Received {nsb_resp.metadata.payload_size} bytes "
                        + f"from {nsb_resp.metadata.src_id} to {nsb_resp.metadata.dest_id}"
                    )
                    return MessageEntry(
                        src_id=nsb_resp.metadata.src_id,
                        dest_id=nsb_resp.metadata.dest_id,
                        payload=payload,
                    )
                elif nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.NO_MESSAGE:
                    self.logger.debug("RECEIVE: No message available.")
                    return None
        return None

    async def listen(self):
        """
        @brief Asynchronously listens for a payload via NSB.
        """
        if hasattr(self.comms, "_listen_msg"):
            response = await self.comms._listen_msg(Comms.Channels.RECV)
        else:
            loop = asyncio.get_event_loop()
            response = await loop.run_in_executor(
                None, self.comms._recv_msg, Comms.Channels.RECV, None
            )

        if response:
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            if nsb_resp.manifest.op in [
                nsb_pb2.nsbm.Manifest.Operation.RECEIVE,
                nsb_pb2.nsbm.Manifest.Operation.FORWARD,
            ]:
                if nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.MESSAGE:
                    if self.cfg.use_db:
                        payload = self.db.check_out(nsb_resp.msg_key)
                    else:
                        payload = nsb_resp.payload
                    self.logger.info(
                        f"LISTEN: Received {nsb_resp.metadata.payload_size} bytes"
                    )
                    return MessageEntry(
                        src_id=nsb_resp.metadata.src_id,
                        dest_id=nsb_resp.metadata.dest_id,
                        payload=payload,
                    )
        return None


### NSB SIMULATOR CLIENT ###


class NSBSimClient(NSBClient):
    """
    @brief NSB Simulator Client interface with backend selection.
    """

    def __init__(
        self,
        identifier: str,
        server_address: str,
        server_port: int,
        backend: str = "socket",
    ):
        """
        @brief Constructs the NSB Simulator Client interface.

        @param identifier The identifier for this NSB simulator client.
        @param server_address The address of the NSB daemon/broker.
        @param server_port The port of the NSB daemon/broker.
        @param backend Communication backend: 'socket' or 'rabbitmq' (default: 'socket').
        """
        self._id = identifier
        self.logger = logging.getLogger(f"{self._id} (sim)")
        super().__init__(server_address, server_port, backend, client_id=identifier)
        self.og_indicator = nsb_pb2.nsbm.Manifest.Originator.SIM_CLIENT
        self.initialize()

    def fetch(self, src_id: str | None = None, timeout: int | None = None):
        """
        @brief Fetches a payload that needs to be sent over the simulated network.
        """
        if self.backend == "socket" and self.cfg.system_mode == Config.SystemMode.PULL:
            nsb_msg = nsb_pb2.nsbm()
            nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.FETCH
            nsb_msg.manifest.og = self.og_indicator
            nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.SUCCESS
            if src_id:
                nsb_msg.metadata.src_id = src_id
            else:
                if self.cfg.simulator_mode == Config.SimulatorMode.PER_NODE:
                    nsb_msg.metadata.src_id = self._id
                else:
                    nsb_msg.metadata.src_id = ""
            self.comms._send_msg(Comms.Channels.RECV, nsb_msg.SerializeToString())
            self.logger.info("FETCH: Sent fetch request to server.")
        elif (
            self.backend == "socket" and self.cfg.system_mode == Config.SystemMode.PUSH
        ):
            if timeout is not None and timeout != 0:
                self.logger.warning(
                    "FETCH: System is in PUSH mode, timeout will be overwritten to 0."
                )
            timeout = 0

        response = self.comms._recv_msg(Comms.Channels.RECV, timeout=timeout)
        if response:
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            if nsb_resp.manifest.op in [
                nsb_pb2.nsbm.Manifest.Operation.SEND,
                nsb_pb2.nsbm.Manifest.Operation.FETCH,
                nsb_pb2.nsbm.Manifest.Operation.FORWARD,
            ]:
                if nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.MESSAGE:
                    if self.cfg.use_db:
                        payload = self.db.check_out(nsb_resp.msg_key)
                    else:
                        payload = nsb_resp.payload
                    self.logger.info(
                        f"FETCH: Fetched {nsb_resp.metadata.payload_size} bytes "
                        + f"from {nsb_resp.metadata.src_id} to {nsb_resp.metadata.dest_id}"
                    )
                    return MessageEntry(
                        src_id=nsb_resp.metadata.src_id,
                        dest_id=nsb_resp.metadata.dest_id,
                        payload=payload,
                    )
                elif nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.NO_MESSAGE:
                    self.logger.debug("FETCH: No message available.")
                    return None
        return None

    async def listen(self):
        """
        @brief Asynchronously listens for a payload to fetch.
        """
        if hasattr(self.comms, "_listen_msg"):
            response = await self.comms._listen_msg(Comms.Channels.RECV)
        else:
            loop = asyncio.get_event_loop()
            response = await loop.run_in_executor(
                None, self.comms._recv_msg, Comms.Channels.RECV, None
            )

        if response:
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            if nsb_resp.manifest.op in [
                nsb_pb2.nsbm.Manifest.Operation.FETCH,
                nsb_pb2.nsbm.Manifest.Operation.FORWARD,
            ]:
                if nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.MESSAGE:
                    if self.cfg.use_db:
                        payload = self.db.check_out(nsb_resp.msg_key)
                    else:
                        payload = nsb_resp.payload
                    self.logger.info(
                        f"LISTEN: Fetched {nsb_resp.metadata.payload_size} bytes"
                    )
                    return MessageEntry(
                        src_id=nsb_resp.metadata.src_id,
                        dest_id=nsb_resp.metadata.dest_id,
                        payload=payload,
                    )
        return None

    def post(self, src_id: str, dest_id: str, payload: bytes):
        """
        @brief Posts a payload that has been transmitted over the simulated network.
        """
        nsb_msg = nsb_pb2.nsbm()
        nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.POST
        nsb_msg.manifest.og = self.og_indicator
        nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.MESSAGE
        nsb_msg.metadata.src_id = src_id
        nsb_msg.metadata.dest_id = dest_id
        nsb_msg.metadata.payload_size = len(payload)

        key = None
        if self.cfg.use_db:
            key = self.db.store(payload)
            nsb_msg.msg_key = key
        else:
            nsb_msg.payload = payload

        self.comms._send_msg(
            Comms.Channels.SEND, nsb_msg.SerializeToString(), dest_id=dest_id
        )
        self.logger.info("POST: Posted message + payload to server.")
        return key
