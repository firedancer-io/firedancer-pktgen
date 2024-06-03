cmake_minimum_required(VERSION 3.16)
project(FiredancerPktGen C)

enable_language(C)
set(CMAKE_C_STANDARD 17)

set(FIREDANCER_BUILD "/opt/firedancer/build/native/gcc" CACHE FILEPATH "Path to Firedancer build dir")

add_executable(fd_pktgen src/app/fd_pktgen_main.c)
