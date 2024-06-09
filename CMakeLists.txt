cmake_minimum_required(VERSION 3.16)
project(FiredancerPktGen C)

enable_language(C)
set(CMAKE_C_STANDARD 17)

set(FIREDANCER_BUILD "/opt/firedancer/build/native/gcc" CACHE FILEPATH "Path to Firedancer build dir")

# Package fdgen_xdp_ports.o into a C library

add_custom_command(OUTPUT fdgen_xdp_ports.o
    COMMAND cd ${CMAKE_CURRENT_SOURCE_DIR}/src/xdp && ld -r -b binary -o ${CMAKE_CURRENT_BINARY_DIR}/fdgen_xdp_ports.o fdgen_xdp_ports.o
    COMMAND objcopy --rename-section .data=.rodata,alloc,load,readonly,data,contents ${CMAKE_CURRENT_BINARY_DIR}/fdgen_xdp_ports.o ${CMAKE_CURRENT_BINARY_DIR}/fdgen_xdp_ports.o)

add_library(fdgen_xdp_ports STATIC fdgen_xdp_ports.o)

SET_SOURCE_FILES_PROPERTIES(
    fdgen_xdp_ports.o
    PROPERTIES
    EXTERNAL_OBJECT true
    GENERATED true)

SET_TARGET_PROPERTIES(
    fdgen_xdp_ports
    PROPERTIES
    LINKER_LANGUAGE C)

add_executable(fdgen
    src/app/fdgen_main.c
    src/cfg/fdgen_cfg_net.c
    src/cfg/fdgen_cfg_net_socket.c
    src/cfg/fdgen_cfg_net_xdp.c
    src/tile/net_xsk/fdgen_tile_net_xsk_rx.c)

target_compile_options(fdgen PRIVATE
    -Wall
    -march=native
    -mtune=native
    -DFD_HAS_ATOMIC=1
    -DFD_HAS_THREADS=1
    -DFD_HAS_X86=1
    -DFD_HAS_SSE=1)

target_include_directories(fdgen SYSTEM PUBLIC
    ${FIREDANCER_BUILD}/include)

target_link_libraries(fdgen
    -pthread
    -lstdc++
    fdgen_xdp_ports
    ${FIREDANCER_BUILD}/lib/libfd_quic.a
    ${FIREDANCER_BUILD}/lib/libfd_waltz.a
    ${FIREDANCER_BUILD}/lib/libfd_tango.a
    ${FIREDANCER_BUILD}/lib/libfd_util.a)
