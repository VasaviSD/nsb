"""
@file nsb_rabbitmq.py
@module nsb_rabbitmq
@brief RabbitMQ-based Application & Simulator Client Interfaces for NSB
@details This module provides RabbitMQ-backed implementations of NSB clients,
         maintaining the same semantics as the socket-based implementation while
         using RabbitMQ as the message broker. The architecture uses a separate
         daemon client for configuration management and direct client-to-client
         communication through the broker.
"""

import time
import logging
from enum import IntEnum
import threading
import asyncio

import pika
import pika.exceptions
import proto.nsb_pb2 as nsb_pb2
import redis

# Set up logging
logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s.%(msecs)03d\t%(name)s\t%(levelname)s\t%(message)s',
                    datefmt='%H:%M:%S',
                    handlers=[logging.StreamHandler(),])

## @var SERVER_CONNECTION_TIMEOUT
# Maximum time a client will wait to connect to the broker.
SERVER_CONNECTION_TIMEOUT = 10
## @var DAEMON_RESPONSE_TIMEOUT
# Maximum time a client will wait to get a response from the daemon.
DAEMON_RESPONSE_TIMEOUT = 600

### SYSTEM CONFIG ###

class Config:
    """
    @brief Class to maintain property codes.

    These property codes should be standardized across Python and C++ libraries, 
    including the NSB Daemon.
    """
    class SystemMode(IntEnum):
        """
        @brief Denotes whether the NSB system is in *PUSH* or *PULL* mode.

        Original (socket-based) behavior [outdated for RabbitMQ]:
        - In PULL mode, clients poll the daemon server to fetch/receive messages.
        - In PUSH mode, the daemon forwards messages to clients automatically.

        RabbitMQ implementation (current behavior):
        - The broker routes messages directly to client queues.
        - Clients consume from their own RECV queue in both modes.
        - PULL typically uses non-blocking polling with a timeout.
        - PUSH can be treated as immediate/streaming consumption (e.g., timeout=0 or async listen).
        - The daemon is used for configuration only, not message forwarding.
        
        @see NSBAppClient.receive()
        @see NSBSimClient.fetch()
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

        @param nsb_msg The NSB INIT response message from the daemon server 
        containing the configuration. The message should have a 'config' field.

        @see NSBClient.initialize()
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
    
    Provides a consistent interface for accessing message metadata and payload.
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

    As NSB is expected to support different communication paradigms and protocols --
    including sockets, RabbitMQ, and Websockets -- we plan to provide various 
    different communication interfaces with the same basic functions.
    """
    class Channels(IntEnum):
        """
        @brief Shared enumeration to designate different channels.
        """
        CTRL = 0
        SEND = 1
        RECV = 2


class RabbitMQInterface(Comms):
    """
    @brief RabbitMQ-based interface for client-server communication.

    This class implements RabbitMQ-based message routing to facilitate network 
    communication between NSB clients and the broker. It maintains the same 
    3-channel architecture as SocketInterface but uses RabbitMQ queues instead 
    of TCP sockets.
    
    Queue naming convention:
    - CTRL channel (client listens here): nsb.config.response.{client_id}
    - SEND channel: nsb.send.{client_id}
    - RECV channel: nsb.recv.{client_id}
    - Config request (client publishes here): nsb.config.request
    - Config response (client listens here): nsb.config.response.{client_id}
    """
    def __init__(self, server_address: str, server_port: int, client_id: str = None):
        """
        @brief Constructor for the RabbitMQInterface class.

        Sets the address and port of the RabbitMQ broker before 
        connecting to the broker.
        
        @param server_address The address of the RabbitMQ broker.
        @param server_port The port of the RabbitMQ broker.
        @param client_id Optional client identifier for queue naming.

        @see RabbitMQInterface._connect(timeout)
        """
        # Save connection information.
        self.server_addr = server_address 
        self.server_port = server_port
        self.client_id = client_id
        
        # Create logger.
        self.logger = logging.getLogger(f"RabbitMQ({self.server_addr}:{self.server_port})")
        
        # RabbitMQ connection-specific stuff
        self._connection = None
        self._channels = {}  # One channel per Comms.Channels member
        self._queues = {}    # Mapping: Comms.Channels -> queue name
        self._connect()

    def _connect(self, timeout: int = SERVER_CONNECTION_TIMEOUT):
        """
        @brief Connects to the RabbitMQ broker with the stored server address and port.

        This method establishes a connection to the broker and creates channels
        for each of the client's communication channels (CTRL, SEND, RECV).

        @param timeout Maximum time in seconds to wait to connect to the broker. 
        @exception TimeoutError Raised if the connection to the broker times out 
                                after the specified timeout period.
        """
        self.logger.info(f"Connecting to rabbitbroker@{self.server_addr}:{self.server_port}...")
        
        # Set target time for timing out.
        start_time = time.time()
        params = pika.ConnectionParameters(
            host=self.server_addr, 
            port=self.server_port,
            connection_attempts=3,
            retry_delay=1
        )

        # Establish a connection or timeout.
        while time.time() - start_time < timeout:
            try:
                self._connection = pika.BlockingConnection(params)
                
                # Create channels for each communication channel
                for ch in Comms.Channels:
                    channel = self._connection.channel()
                    self._channels[ch] = channel
                    
                    # Declare exchange once (on the first channel)
                    if ch == list(Comms.Channels)[0]:
                        channel.exchange_declare(exchange="nsb", exchange_type="direct", durable=True)
                    
                    # Create and bind queues per channel
                    if ch == Comms.Channels.CTRL:
                        # CTRL channel listens on config response queue
                        if not self.client_id:
                            raise RuntimeError("CTRL channel requires client_id for response queue")
                        qname = f"nsb.config.response.{self.client_id}"
                        channel.queue_declare(queue=qname, durable=True)
                        channel.queue_bind(exchange="nsb", queue=qname, routing_key=qname)
                        self._queues[ch] = qname
                    else:
                        if self.client_id:
                            qname = f"nsb.{ch.name.lower()}.{self.client_id}"
                        else:
                            qname = f"nsb.{ch.name.lower()}"
                        channel.queue_declare(queue=qname, durable=True)
                        channel.queue_bind(exchange="nsb", queue=qname, routing_key=qname)
                        self._queues[ch] = qname

                # Declare and bind config request queue (clients publish here for INIT/PING)
                config_channel = self._channels[Comms.Channels.CTRL]
                config_channel.queue_declare(queue="nsb.config.request", durable=True)
                config_channel.queue_bind(exchange="nsb", queue="nsb.config.request", routing_key="nsb.config.request")
                
                self.logger.info("Connected and channels established.")
                return
            except pika.exceptions.AMQPConnectionError as e:
                self.logger.debug(f"Connection failed: {e}. Retrying...")
                time.sleep(1)

        raise TimeoutError(f"RabbitMQ connection timed out after {timeout} seconds.")

    def _close(self):
        """
        @brief Healthily closes the RabbitMQ connection.

        Attempts to shutdown the channels, then closes connection if open.
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
        
        This method publishes the message to the exchange, routing it to the 
        appropriate queue based on the channel and optional destination.

        @param channel The channel to send the message on (CTRL, SEND, or RECV).
        @param message The message to send to the broker.
        @param dest_id Optional destination client ID for routing (used for SEND operations).
        @exception RuntimeError Raised if the publish operation fails.
        """
        try:
            ch = self._channels[channel]
            
            # Determine routing key
            if channel == Comms.Channels.CTRL:
                # CTRL publications go to the shared config request queue
                routing_key = "nsb.config.request"
            elif dest_id and channel == Comms.Channels.SEND:
                # For SEND operations, route to destination's RECV queue
                routing_key = f"nsb.recv.{dest_id}"
            else:
                # For other operations, use the channel's queue
                routing_key = self._queues[channel]
            
            # Publish message
            # Delivery mode 1: Transient (not written to disk)
            # Delivery mode 2: Persistent (written to disk) (lower performance)
            ch.basic_publish(
                exchange="nsb",
                routing_key=routing_key,
                body=message,
                properties=pika.BasicProperties(delivery_mode=1)
            )
            self.logger.debug(f"Published to {routing_key}")
        except Exception as e:
            self.logger.error(f"Send failed on {channel.name}: {e}")
            raise RuntimeError(f"RabbitMQ send failed: {e}")

    def _recv_msg(self, channel: Comms.Channels, timeout: int | None = None):
        """
        @brief Receives a message from the broker.
        
        This method consumes a message from the queue, waiting up to timeout seconds
        before timing out.

        @param channel The channel to receive the message on (CTRL, SEND, or RECV).
        @param timeout Maximum time in seconds to wait for a response from the
                       broker. If None, it will wait indefinitely.
        @return The message body as bytes, or None if timeout occurs.
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
                if timeout and (time.time() - start_time) > timeout:
                    self.logger.debug(f"Timeout on recv from {channel.name}")
                    return None
                time.sleep(check_interval)
        except Exception as e:
            self.logger.error(f"Receive failed on {channel.name}: {e}")
            return None
    
    async def _listen_msg(self, channel: Comms.Channels):
        """
        @brief Asynchronously listens for a message.
        
        This method uses asynchronous message consumption to wait for a message 
        to come in over the target channel without blocking other coroutines.

        @param channel The channel to listen on (CTRL, SEND, or RECV).
        @return The message body as bytes, or None if an error occurs.
        """
        check_interval = 0.05
        
        qname = self._queues.get(channel)
        if not qname:
            self.logger.error(f"No queue for channel {channel.name}")
            return None

        try:
            # Use asyncio with a loop to periodically check for messages
            while True:
                ch = self._channels.get(channel)
                if ch:
                    method_frame, _, body = ch.basic_get(queue=qname, auto_ack=True)
                    if method_frame:
                        self.logger.debug(f"Async received from {qname}")
                        return body
                await asyncio.sleep(check_interval)
        except Exception as e:
            self.logger.error(f"Async receive failed on {channel.name}: {e}")
            return None
    
    def __del__(self):
        """
        @brief Closes connection to the broker.

        @see _close()
        """
        self._close()


### DATABASE CONNECTORS ###

class DBConnector:
    """
    @brief Base class for database connectors.

    Database connectors enable NSB to use a database to store and retrieve 
    larger messages being sent over the network.
    """
    def __init__(self, client_id: str):
        """
        @brief Base class constructor.
        
        Stores the client ID for the database connector and initializes a 
        payload counter and lock method call to be used in the payload ID 
        generator.

        @param client_id The identifier for the client object using this 
                         connector.

        @see generate_payload_id()
        """
        self.client_id = client_id
        self.plctr = 0
        self.lock = threading.Lock()
        
    def generate_payload_id(self):
        """
        @brief Payload ID generator.
        
        This simple message key generator method uses the client ID, the current 
        time, and an incrementing counter to create unique message IDs.

        @returns str The newly generated unique ID for a payload.
        """
        with self.lock:
            self.plctr = (self.plctr + 1) & 0xFFFFF
            tms = int(time.time() * 1000) & 0x1FFFFFFFFFF
            payload_id = f"{tms}-{self.client_id}-{self.plctr}"
            return payload_id


class RedisConnector(DBConnector):
    """
    @brief Connector for Redis Database.

    This connector enables NSB to store and retrieve payload data using Redis's 
    in-memory key-value store.
    """
    def __init__(self, client_id: str, address: str, port: int):
        """
        @brief Constructor for RedisConnector.

        This constructor passes the client ID to the base class constructor and
        uses the address and port to connect to the Redis instance. The Redis 
        server must be started from outside this program.

        @param client_id The identifier of the client object that is using this
                         connector.
        @param address The address of the Redis server.
        @param port The port to be used to access the Redis server.
        """
        super().__init__(client_id)
        self.address = address
        self.port = port
        self.r = redis.Redis(host=self.address, port=self.port)

    def is_connected(self):
        """
        @brief Checks connection to the Redis server.

        @returns bool Whether or not the Redis server is reachable.
        """
        try:
            return self.r.ping()
        except redis.ConnectionError:
            return False
        
    def store(self, value: bytes):
        """
        @brief Stores a payload.

        This method creates a new unique payload key and then stores the
        payload under that key.

        @param value The value (payload) to be stored.
        @return The generated key the message was stored with.
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

        This method uses the passed in key to check out a payload, deleting the
        payload after it has been retrieved.

        @param key The key to retrieve the payload from the Redis server.
        @return The retrieved payload.
        """
        try:
            value = self.r.getdel(key)
            if isinstance(value, str):
                value = value.encode('latin1')
            return value
        except ConnectionError as e:
            logging.error("Check that the Redis server is online.")
            raise e
    
    def peek(self, key: str):
        """
        @brief Peeks at the payload at the given key.

        This method uses the passed in key to retrieve a payload, similar to 
        the check_out() method, but without deleting the stored value.

        @param key The key to retrieve the payload from the Redis server.
        @return The retrieved payload.
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


### NSB Base Client Class ###

class NSBClient:
    """
    @brief NSB client base class for RabbitMQ backend.
    
    This class serves as the base for the implemented clients (NSBAppClient 
    and NSBSimClient) and provides basic methods and shared operation methods
    using RabbitMQ for communication.
    """

    def __init__(self, server_address: str, server_port: int):
        """
        @brief Base constructor for NSB Clients.

        Sets the communications module to RabbitMQInterface to connect to 
        the RabbitMQ broker.

        @param server_address The address of the RabbitMQ broker.
        @param server_port The port of the RabbitMQ broker.
        
        Note: server_address and server_port will be set by subclass and 
        RabbitMQInterface constructor.

        @see RabbitMQInterface
        """
        
        self.comms = None  # Will be set in subclass after _id is set
        self.og_indicator = None  # Must be set in subclass
        self.cfg = None
        self.db = None

    def initialize(self):
        """
        @brief Initializes configuration with the daemon.

        This method sends an INIT message to the daemon client requesting
        configuration and gets a response containing configuration parameters.
        """
        if not hasattr(self, '_id'):
            raise RuntimeError("Client identifier (_id) not set.")
        
        client_id = self._id
        self.logger.info(f"Initializing {client_id} with broker at {self.comms.server_addr}:{self.comms.server_port}...")

        # Create and populate an INIT message.
        nsb_msg = nsb_pb2.nsbm()
        nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.INIT
        nsb_msg.manifest.og = self.og_indicator
        nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.SUCCESS
        nsb_msg.intro.identifier = client_id
        nsb_msg.intro.address = self.comms.server_addr
        nsb_msg.intro.ch_CTRL = 0
        nsb_msg.intro.ch_SEND = 0
        nsb_msg.intro.ch_RECV = 0

        self.logger.debug(f"INIT message: {nsb_msg.intro}")

        # Send INIT to config request queue
        self.comms._send_msg(Comms.Channels.CTRL, nsb_msg.SerializeToString())
        self.logger.debug("Sent INIT. Waiting for response...")

        # Wait for response on config response queue
        response = self.comms._recv_msg(Comms.Channels.CTRL, timeout=DAEMON_RESPONSE_TIMEOUT)
        if response:
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            if nsb_resp.manifest.op == nsb_pb2.nsbm.Manifest.Operation.INIT and nsb_resp.HasField("config"):
                self.cfg = Config(nsb_resp)
                self.logger.info(f"Configuration received:\n{self.cfg}")
                if self.cfg.use_db:
                    self.db = RedisConnector(client_id, self.cfg.db_address, self.cfg.db_port)
                    if not self.db.is_connected():
                        logging.error("Check that the Redis server is online.")
                        raise RuntimeError("Failed to connect to Redis database.")
                return
        raise RuntimeError("Failed to initialize NSB client: invalid or missing response.")

    def ping(self, timeout: int = DAEMON_RESPONSE_TIMEOUT):
        """
        @brief Pings the daemon to check connectivity.

        @param timeout Maximum time to wait for a response from the daemon.
        @returns bool True if successful, False otherwise.
        """
        nsb_msg = nsb_pb2.nsbm()
        nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.PING
        nsb_msg.manifest.og = self.og_indicator
        nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.SUCCESS

        self.comms._send_msg(Comms.Channels.CTRL, nsb_msg.SerializeToString())
        self.logger.info("PING: Sent to daemon.")

        response = self.comms._recv_msg(Comms.Channels.CTRL, timeout=timeout)
        if response:
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            if nsb_resp.manifest.op == nsb_pb2.nsbm.Manifest.Operation.PING:
                code = nsb_resp.manifest.code
                if code == nsb_pb2.nsbm.Manifest.OpCode.SUCCESS:
                    self.logger.info("PING: Daemon responded OK.")
                    return True
                elif code == nsb_pb2.nsbm.Manifest.OpCode.FAILURE:
                    self.logger.info("PING: Daemon is up, but returned FAILURE.")
                    return False
        self.logger.warning("PING: No response or unexpected response.")
        return False

    def exit(self):
        """
        @brief Sends an EXIT message to the daemon and closes the connection.
        """
        nsb_msg = nsb_pb2.nsbm()
        nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.EXIT
        nsb_msg.manifest.og = self.og_indicator
        nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.SUCCESS

        self.comms._send_msg(Comms.Channels.CTRL, nsb_msg.SerializeToString())
        self.logger.info("EXIT: Sent shutdown command to daemon.")
        self.comms._close()

    ### MACROS ###
    def msg_get_payload_obj(self, msg):
        return msg.msg_key if self.cfg.use_db else msg.payload

    def msg_set_payload_obj(self, payload_obj, msg):
        if self.cfg.use_db:
            msg.msg_key = payload_obj
        else:
            msg.payload = payload_obj


### NSB APPLICATION CLIENT ###

class NSBAppClient(NSBClient):
    """
    @brief NSB Application Client interface for RabbitMQ backend.
    
    This client provides the high-level NSB interface to send and receive 
    messages via NSB by communicating with the RabbitMQ broker.
    """
    def __init__(self, identifier: str, server_address: str, server_port: int):
        """
        @brief Constructs the NSB Application Client interface.

        This method initializes a RabbitMQ interface to connect and communicate 
        with the RabbitMQ broker. It also sets an identifier that should 
        correspond to the identifier used in the NSB system.

        @param identifier The identifier for this NSB application client.
        @param server_address The address of the RabbitMQ broker.
        @param server_port The port of the RabbitMQ broker.
        """
        self._id = identifier
        self.logger = logging.getLogger(f"{self._id} (app-rmq)")
        super().__init__(server_address, server_port)
        self.comms = RabbitMQInterface(server_address, server_port, self._id)
        self.og_indicator = nsb_pb2.nsbm.Manifest.Originator.APP_CLIENT
        self.initialize()

    def send(self, dest_id: str, payload: bytes):
        """
        @brief Sends a payload to the specified destination via RabbitMQ.
        
        This method creates an NSB SEND message with the appropriate information 
        and payload and sends it through the broker. It does not expect a response 
        from the simulator client nor broker.

        @param dest_id The identifier of the destination NSB client.
        @param payload The payload to send to the destination.
        """
        # Create and populate a SEND message.
        nsb_msg = nsb_pb2.nsbm()
        # Manifest.
        nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.SEND
        nsb_msg.manifest.og = self.og_indicator
        nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.MESSAGE
        # Metadata.
        nsb_msg.metadata.src_id = self._id
        nsb_msg.metadata.dest_id = dest_id
        nsb_msg.metadata.payload_size = len(payload)
        # Database stuff
        if self.cfg.use_db:
            nsb_msg.msg_key = self.db.store(payload)
        else:
            nsb_msg.payload = payload
        
        # Send to destination's RECV queue
        self.comms._send_msg(Comms.Channels.SEND, nsb_msg.SerializeToString(), dest_id=dest_id)
        self.logger.info("SEND: Sent message + payload to broker.")

    def receive(self, dest_id: str | None = None, timeout: int | None = None):
        """
        @brief Receives a payload via NSB.

        This method directly consumes messages from the client's RECV queue in 
        RabbitMQ, eliminating the need for daemon intermediation. Messages are 
        routed directly to the client's queue by the broker.

        @param dest_id Deprecated parameter (kept for API compatibility). 
                       Not used in RabbitMQ implementation.
        @param timeout The amount of time in seconds to wait to receive data. 
                       None denotes waiting indefinitely while 0 denotes polling
                       behavior.

        @returns MessageEntry|None The MessageEntry containing the received 
                                   payload and metadata if a message is found, 
                                   otherwise None.

        @see Config.SystemMode
        @see RabbitMQInterface._recv_msg()
        """
        # In RabbitMQ, we consume directly from the RECV queue
        # The broker routes messages directly to nsb.recv.{client_id}
        response = self.comms._recv_msg(Comms.Channels.RECV, timeout=timeout)
        if response:
            # Parse in message.
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            # Check to see that message is of expected operation.
            if nsb_resp.manifest.op == nsb_pb2.nsbm.Manifest.Operation.SEND or \
               nsb_resp.manifest.op == nsb_pb2.nsbm.Manifest.Operation.FORWARD:
                # Check to see if there is a message at all.
                if nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.MESSAGE:
                    if self.cfg.use_db:
                        # If using a database, retrieve the payload.
                        payload = self.db.check_out(nsb_resp.msg_key)
                    else:
                        payload = nsb_resp.payload
                    self.logger.info(f"RECEIVE: Received {nsb_resp.metadata.payload_size} " + \
                                        f"bytes from {nsb_resp.metadata.src_id} to " + \
                                        f"{nsb_resp.metadata.dest_id}: " + \
                                        f"({len(payload)}B)")
                    # Pack the payload into a MessageEntry.
                    return MessageEntry(src_id=nsb_resp.metadata.src_id,
                                        dest_id=nsb_resp.metadata.dest_id,
                                        payload=payload)
                elif nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.NO_MESSAGE:
                    self.logger.debug("RECEIVE: No message available.")
                    return None
        # If no response to receive, return None.
        return None

    async def listen(self):
        """
        @brief Asynchronously listens for a payload via NSB.

        This method returns a coroutine to be used in asynchronous calls. Its 
        implementation is based on the receive() method, but leverages the 
        asynchronous _listen_msg() instead.

        @returns MessageEntry|None The MessageEntry containing the received 
                                   payload and metadata if a message is found, 
                                   otherwise None.

        @see NSBAppClient.receive()
        @see RabbitMQInterface._listen_msg()
        """
        # Get response from request or just wait for message to come in.
        response = await self.comms._listen_msg(Comms.Channels.RECV)
        if response:
            # Parse in message.
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            # Check to see that message is of expected operation.
            if nsb_resp.manifest.op == nsb_pb2.nsbm.Manifest.Operation.RECEIVE or nsb_pb2.nsbm.Manifest.Operation.FORWARD:
                # Check to see if there is a message at all.
                if nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.MESSAGE:
                    if self.cfg.use_db:
                        # If using a database, retrieve the payload.
                        payload = self.db.check_out(nsb_resp.msg_key)
                    else:
                        payload = nsb_resp.payload
                    self.logger.info(f"LISTEN: Received {nsb_resp.metadata.payload_size} " + \
                                        f"bytes from {nsb_resp.metadata.src_id} to " + \
                                        f"{nsb_resp.metadata.dest_id}: " + \
                                        f"({len(payload)}B)")
                    # Pack the payload into a MessageEntry.
                    return MessageEntry(src_id=nsb_resp.metadata.src_id,
                                        dest_id=nsb_resp.metadata.dest_id,
                                        payload=payload)
                elif nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.NO_MESSAGE:
                    self.logger.info("LISTEN: No message available.")
                    return None
        # If nothing, return None.
        return None


### NSB SIMULATOR CLIENT ###

class NSBSimClient(NSBClient):
    """
    @brief NSB Simulator Client interface for RabbitMQ backend.
    
    This client provides the NSB simulator interface for fetching and posting
    messages via RabbitMQ by communicating with the broker.
    """

    def __init__(self, identifier: str, server_address: str, server_port: int):
        """
        @brief Constructs the NSB Simulator Client interface.

        This method initializes a RabbitMQ interface to connect and communicate 
        with the RabbitMQ broker.

        @param identifier The identifier for this NSB simulator client.
        @param server_address The address of the RabbitMQ broker.
        @param server_port The port of the RabbitMQ broker.
        """
        self._id = identifier
        self.logger = logging.getLogger(f"{self._id} (sim-rmq)")
        super().__init__(server_address, server_port)
        self.comms = RabbitMQInterface(server_address, server_port, self._id)
        self.og_indicator = nsb_pb2.nsbm.Manifest.Originator.SIM_CLIENT
        self.initialize()

    def fetch(self, src_id: str | None = None, timeout: int | None = None):
        """
        @brief Fetches a payload that needs to be sent over the simulated 
               network.
        
        This method directly consumes messages from the simulator's RECV queue 
        in RabbitMQ. In SYSTEM_WIDE mode, the simulator receives all messages. 
        In PER_NODE mode, the simulator only receives messages sent to its own 
        identifier.

        @param src_id Deprecated parameter (kept for API compatibility). 
                      Not used in RabbitMQ implementation.
        @param timeout The amount of time in seconds to wait to receive data. 
               None denotes waiting indefinitely while 0 denotes polling 
               behavior.

        @returns MessageEntry|None The MessageEntry containing the fetched 
                                   payload and metadata if a message is found, 
                                   otherwise None.
        """
        # In RabbitMQ, we consume directly from the RECV queue
        # The broker routes messages directly to nsb.recv.{simulator_id}
        response = self.comms._recv_msg(Comms.Channels.RECV, timeout=timeout)
        if response:
            # Parse in message.
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            if nsb_resp.manifest.op == nsb_pb2.nsbm.Manifest.Operation.SEND or \
                nsb_resp.manifest.op == nsb_pb2.nsbm.Manifest.Operation.FORWARD:
                if nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.MESSAGE:
                    if self.cfg.use_db:
                        # If using a database, retrieve the payload.
                        payload = self.db.check_out(nsb_resp.msg_key)
                    else:
                        payload = nsb_resp.payload
                    self.logger.info(f"FETCH: Got {nsb_resp.metadata.payload_size} " + \
                                        f"bytes from {nsb_resp.metadata.src_id} to " + \
                                        f"{nsb_resp.metadata.dest_id}: " + \
                                        f"({len(payload)}B)")
                    # Pack the payload into a MessageEntry.
                    return MessageEntry(src_id=nsb_resp.metadata.src_id,
                                        dest_id=nsb_resp.metadata.dest_id,
                                        payload=payload)
                elif nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.NO_MESSAGE:
                    self.logger.info("FETCH: No message available.")
                    return None
        return None

    async def listen(self):
        """
        @brief Asynchronously listens for a payload that needs to be sent over 
               the simulated network.
        
        This method returns a coroutine to be used in asynchronous calls. Its 
        implementation is based on the fetch() method, but leverages the 
        asynchronous _listen_msg() instead.

        @returns MessageEntry|None The MessageEntry containing the fetched 
                                   payload and metadata if a message is found, 
                                   otherwise None.
        
        @see NSBSimClient.fetch()
        @see RabbitMQInterface._listen_msg()
        """
        # Get response from request or await forwarded message.
        response = await self.comms._listen_msg(Comms.Channels.RECV)
        if response:
            # Parse in message.
            nsb_resp = nsb_pb2.nsbm()
            nsb_resp.ParseFromString(response)
            if nsb_resp.manifest.op == nsb_pb2.nsbm.Manifest.Operation.FETCH or \
                nsb_resp.manifest.op == nsb_pb2.nsbm.Manifest.Operation.FORWARD:
                if nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.MESSAGE:
                    if self.cfg.use_db:
                        # If using a database, retrieve the payload.
                        payload = self.db.check_out(nsb_resp.msg_key)
                    else:
                        payload = nsb_resp.payload
                    self.logger.info(f"LISTEN: Got {nsb_resp.metadata.payload_size} " + \
                                        f"bytes from {nsb_resp.metadata.src_id} to " + \
                                        f"{nsb_resp.metadata.dest_id}: " + \
                                        f"({len(payload)}B)")
                    # Pack the payload into a MessageEntry.
                    return MessageEntry(src_id=nsb_resp.metadata.src_id,
                                        dest_id=nsb_resp.metadata.dest_id,
                                        payload=payload)
                elif nsb_resp.manifest.code == nsb_pb2.nsbm.Manifest.OpCode.NO_MESSAGE:
                    self.logger.info("LISTEN: No message available.")
                    return None
            else:
                return None
        else:
            return None

    def post(self, src_id: str, dest_id: str, payload: bytes):
        """
        @brief Posts a payload to the specified destination via NSB.
        
        This is intended to be used when a payload is finished being processed 
        (either successfully delivered or dropped) and the simulator client 
        needs to hand it off back to NSB. This method creates an NSB POST 
        message with the appropriate information and payload and sends it to the 
        daemon.

        @param src_id The identifier of the source NSB client.
        @param dest_id The identifier of the destination NSB client.
        @param payload The payload to post to the destination.
        """
        # Create and populate a POST message.
        nsb_msg = nsb_pb2.nsbm()
        # Manifest.
        nsb_msg.manifest.op = nsb_pb2.nsbm.Manifest.Operation.POST
        nsb_msg.manifest.og = self.og_indicator
        nsb_msg.manifest.code = nsb_pb2.nsbm.Manifest.OpCode.MESSAGE
        # Metadata.
        nsb_msg.metadata.src_id = src_id
        nsb_msg.metadata.dest_id = dest_id
        nsb_msg.metadata.payload_size = len(payload)
        self.msg_set_payload_obj(payload, nsb_msg)
        # Send the NSB message to destination's RECV queue
        self.comms._send_msg(Comms.Channels.SEND, nsb_msg.SerializeToString(), dest_id=dest_id)
        self.logger.info("POST: Posted message + payload to broker.")
