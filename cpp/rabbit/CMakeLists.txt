cmake_minimum_required(VERSION 3.16)
project(nsb_rabbitmq_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# macOS: ensure we use libc++ and proper SDK
if(APPLE)
  execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE MACOS_SDK_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  set(CMAKE_OSX_SYSROOT "${MACOS_SDK_PATH}")
  # Explicitly add C++ standard library include path before other includes
  include_directories(BEFORE SYSTEM "${MACOS_SDK_PATH}/usr/include/c++/v1")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

# Determine paths relative to this CMakeLists
get_filename_component(RABBIT_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
get_filename_component(CPP_DIR ${RABBIT_DIR}/.. ABSOLUTE)
set(PROTO_DIR ${CPP_DIR}/proto)

# Protobuf (use existing generated files under cpp/proto)
find_package(Protobuf REQUIRED)
include_directories(${Protobuf_INCLUDE_DIRS})
include_directories(${PROTO_DIR})

# Abseil (required by newer protobuf versions)
find_package(absl REQUIRED)

# SimpleAmqpClient + rabbitmq-c
# Try pkg-config first
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(RABBITMQ_C QUIET librabbitmq)
  pkg_check_modules(SIMPLEAMQPCLIENT QUIET SimpleAmqpClient)
endif()

# Fallback: find_path/find_library
if(NOT RABBITMQ_C_FOUND)
  find_path(RABBITMQ_C_INCLUDE_DIRS amqp.h)
  find_library(RABBITMQ_C_LIBRARIES NAMES rabbitmq librabbitmq)
endif()
if(NOT SIMPLEAMQPCLIENT_FOUND)
  find_path(SIMPLEAMQPCLIENT_INCLUDE_DIRS SimpleAmqpClient/SimpleAmqpClient.h)
  find_library(SIMPLEAMQPCLIENT_LIBRARIES NAMES SimpleAmqpClient)
endif()

if(NOT RABBITMQ_C_INCLUDE_DIRS OR NOT RABBITMQ_C_LIBRARIES)
  message(FATAL_ERROR "rabbitmq-c not found. Install via brew install rabbitmq-c or provide RABBITMQ_C_INCLUDE_DIRS/RABBITMQ_C_LIBRARIES")
endif()
if(NOT SIMPLEAMQPCLIENT_INCLUDE_DIRS OR NOT SIMPLEAMQPCLIENT_LIBRARIES)
  message(FATAL_ERROR "SimpleAmqpClient not found. Install via brew or vcpkg, or provide SIMPLEAMQPCLIENT_INCLUDE_DIRS/SIMPLEAMQPCLIENT_LIBRARIES")
endif()

include_directories(${SIMPLEAMQPCLIENT_INCLUDE_DIRS})
include_directories(${RABBITMQ_C_INCLUDE_DIRS})

# Add library directories for pkg-config results
if(RABBITMQ_C_LIBRARY_DIRS)
  link_directories(${RABBITMQ_C_LIBRARY_DIRS})
endif()
if(SIMPLEAMQPCLIENT_LIBRARY_DIRS)
  link_directories(${SIMPLEAMQPCLIENT_LIBRARY_DIRS})
endif()

# hiredis (for Redis support in unified client)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(HIREDIS QUIET hiredis)
endif()
if(NOT HIREDIS_FOUND)
  find_path(HIREDIS_INCLUDE_DIRS hiredis/hiredis.h)
  find_library(HIREDIS_LIBRARIES NAMES hiredis)
endif()
if(NOT HIREDIS_INCLUDE_DIRS OR NOT HIREDIS_LIBRARIES)
  message(WARNING "hiredis not found — unified client will build without Redis support. Install via: brew install hiredis")
  set(HIREDIS_INCLUDE_DIRS "")
  set(HIREDIS_LIBRARIES "")
endif()
if(HIREDIS_INCLUDE_DIRS)
  include_directories(${HIREDIS_INCLUDE_DIRS})
endif()
if(HIREDIS_LIBRARY_DIRS)
  link_directories(${HIREDIS_LIBRARY_DIRS})
endif()

# Our headers
include_directories(${RABBIT_DIR}/include)

# ── RabbitMQ-only library (existing) ──
add_library(nsb_rabbitmq_cpp
  ${PROTO_DIR}/nsb.pb.cc
  ${RABBIT_DIR}/src/RabbitMQInterface.cpp
  ${RABBIT_DIR}/src/NSBClientRMQ.cpp
  ${RABBIT_DIR}/src/NSBAppClientRMQ.cpp
  ${RABBIT_DIR}/src/NSBSimClientRMQ.cpp
  ${RABBIT_DIR}/src/NSBDaemonRMQ.cpp
)

target_link_libraries(nsb_rabbitmq_cpp
  ${SIMPLEAMQPCLIENT_LIBRARIES}
  ${RABBITMQ_C_LIBRARIES}
  ${Protobuf_LIBRARIES}
  absl::log_internal_check_op
  absl::log_internal_message
)

# ── Unified library (socket + RabbitMQ backends) ──
add_library(nsb_unified_cpp
  ${PROTO_DIR}/nsb.pb.cc
  ${RABBIT_DIR}/src/NSBUnified.cpp
)

target_link_libraries(nsb_unified_cpp
  ${SIMPLEAMQPCLIENT_LIBRARIES}
  ${RABBITMQ_C_LIBRARIES}
  ${HIREDIS_LIBRARIES}
  ${Protobuf_LIBRARIES}
  absl::log_internal_check_op
  absl::log_internal_message
)

# Examples
add_executable(example_send_recv ${RABBIT_DIR}/examples/example_send_recv.cpp)
target_link_libraries(example_send_recv nsb_rabbitmq_cpp)

add_executable(example_daemon ${RABBIT_DIR}/examples/example_daemon.cpp)
target_link_libraries(example_daemon nsb_rabbitmq_cpp)

add_executable(example_unified ${RABBIT_DIR}/examples/example_unified.cpp)
target_link_libraries(example_unified nsb_unified_cpp)

message(STATUS "Using SimpleAmqpClient includes: ${SIMPLEAMQPCLIENT_INCLUDE_DIRS}")
message(STATUS "Using rabbitmq-c includes: ${RABBITMQ_C_INCLUDE_DIRS}")
message(STATUS "Using hiredis includes: ${HIREDIS_INCLUDE_DIRS}")
