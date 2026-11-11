cmake_minimum_required(VERSION 3.16)
project(mini_exchange CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# -fno-omit-frame-pointer keeps perf flame graphs readable
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

option(MX_PROFILE "Keep order book internals out-of-line so they appear in flame graphs" OFF)
if(MX_PROFILE)
  add_compile_definitions(MX_PROFILE)
endif()

option(MX_LINEAR_BEST "Use the original linear best-price walk (benchmark baseline)" OFF)
if(MX_LINEAR_BEST)
  add_compile_definitions(MX_LINEAR_BEST)
endif()

find_package(Threads REQUIRED)
add_library(mx_core INTERFACE)
target_include_directories(mx_core INTERFACE include)
target_link_libraries(mx_core INTERFACE Threads::Threads)

function(mx_binary name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} mx_core)
endfunction()

mx_binary(mx_exchange    src/exchange_main.cpp)   # the venue: gateway + engine + market data
mx_binary(mx_loadgen     src/loadgen.cpp)         # throughput / latency probe
mx_binary(mx_md_listener src/md_listener.cpp)     # UDP feed subscriber
mx_binary(mx_mm          src/mm_client.cpp)       # live market maker
mx_binary(mx_flow        src/flow_client.cpp)     # simulated participants
mx_binary(mx_backtest    src/backtest.cpp)        # in-process strategy research
mx_binary(mx_replay      src/replay.cpp)          # replay LOBSTER market data through the engine

mx_binary(bench_order_book bench/bench_order_book.cpp)

# Optional: binary vs protobuf vs gRPC comparison. Needs
#   apt install protobuf-compiler libprotobuf-dev libgrpc++-dev protobuf-compiler-grpc
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GRPCPP QUIET IMPORTED_TARGET grpc++ protobuf)
endif()
find_program(PROTOC protoc)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
if(GRPCPP_FOUND AND PROTOC AND GRPC_CPP_PLUGIN)
  set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
  file(MAKE_DIRECTORY ${GEN})
  add_custom_command(
    OUTPUT ${GEN}/mx.pb.cc ${GEN}/mx.pb.h ${GEN}/mx.grpc.pb.cc ${GEN}/mx.grpc.pb.h
    COMMAND ${PROTOC} -I ${CMAKE_CURRENT_SOURCE_DIR}/proto --cpp_out=${GEN} --grpc_out=${GEN}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN} ${CMAKE_CURRENT_SOURCE_DIR}/proto/mx.proto
    DEPENDS proto/mx.proto)
  add_executable(bench_wire bench/bench_wire.cpp ${GEN}/mx.pb.cc ${GEN}/mx.grpc.pb.cc)
  target_include_directories(bench_wire PRIVATE ${GEN})
  target_link_libraries(bench_wire mx_core PkgConfig::GRPCPP)
  set_source_files_properties(${GEN}/mx.pb.cc ${GEN}/mx.grpc.pb.cc PROPERTIES COMPILE_FLAGS -w)
else()
  message(STATUS "protobuf/gRPC not found: skipping bench_wire")
endif()

enable_testing()
mx_binary(test_order_book tests/test_order_book.cpp)
mx_binary(test_engine     tests/test_engine.cpp)
add_test(NAME order_book COMMAND test_order_book)
add_test(NAME engine     COMMAND test_engine)
