cmake_minimum_required(VERSION 3.16)
project(FiredancerPktGen C)

enable_language(C)
set(CMAKE_C_STANDARD 17)

set(FIREDANCER_BUILD "/opt/firedancer/build/native/gcc" CACHE FILEPATH "Path to Firedancer build dir")

set(FDGEN_CFLAGS
    -Wall
    -march=native
    -mtune=native
    -g
    -DFD_HAS_ALLOCA=1
    -DFD_HAS_ATOMIC=1
    -DFD_HAS_THREADS=1
    -DFD_HAS_X86=1
    -DFD_HAS_SSE=1
    -DFD_HAS_DOUBLE=1
    -DFD_HAS_INT128=1)

# Package fdgen_xdp_ports.o into a C library

add_compile_options(${FDGEN_CFLAGS})

add_custom_command(OUTPUT fdgen_xdp_ports.o
    COMMAND cd ${CMAKE_CURRENT_SOURCE_DIR}/src/xdp && ld -r -b binary -o ${CMAKE_CURRENT_BINARY_DIR}/fdgen_xdp_ports.o fdgen_xdp_ports.o
    COMMAND objcopy --rename-section .data=.rodata,alloc,load,readonly,data,contents ${CMAKE_CURRENT_BINARY_DIR}/fdgen_xdp_ports.o ${CMAKE_CURRENT_BINARY_DIR}/fdgen_xdp_ports.o
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/xdp/fdgen_xdp_ports.o)

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

add_library(fdgen_lib STATIC
    src/cfg/fdgen_cfg_net.c
    src/cfg/fdgen_cfg_net_socket.c
    src/cfg/fdgen_cfg_net_xdp.c
    src/cfg/fdgen_netlink.c
    src/tile/net_xsk/fdgen_tile_net_xsk_poll.c
    src/tile/net_xsk/fdgen_tile_net_xsk_rx.c
    src/tile/udp_flood/fdgen_tile_udp_flood.c)

include_directories(AFTER SYSTEM
    ${FIREDANCER_BUILD}/include)

add_executable(fdgen_rxdrop src/app/fdgen_rxdrop.c)
target_link_libraries(fdgen_rxdrop
    fdgen_lib
    fdgen_xdp_ports
    ${FIREDANCER_BUILD}/lib/libfd_quic.a
    ${FIREDANCER_BUILD}/lib/libfd_waltz.a
    ${FIREDANCER_BUILD}/lib/libfd_tango.a
    ${FIREDANCER_BUILD}/lib/libfd_util.a
    -pthread
    -lstdc++)

add_executable(fdgen_iavf src/app/fdgen_iavf.c)
target_link_libraries(fdgen_iavf
    fdgen_lib
    fdgen_xdp_ports
    ${FIREDANCER_BUILD}/lib/libfd_waltz.a
    ${FIREDANCER_BUILD}/lib/libfd_tango.a
    ${FIREDANCER_BUILD}/lib/libfd_util.a
    -pthread
    -lstdc++)

add_executable(test_tile_net_xsk_rx src/tile/net_xsk/test_tile_net_xsk_rx.c)
target_link_libraries(test_tile_net_xsk_rx
    fdgen_lib
    fdgen_xdp_ports
    ${FIREDANCER_BUILD}/lib/libfd_waltz.a
    ${FIREDANCER_BUILD}/lib/libfd_tango.a
    ${FIREDANCER_BUILD}/lib/libfd_util.a
    -pthread
    -lstdc++)
