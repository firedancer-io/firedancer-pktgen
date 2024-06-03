cmake_minimum_required(VERSION 3.16)
project(FiredancerPktGen C)

enable_language(C)
set(CMAKE_C_STANDARD 17)

set(FIREDANCER_BUILD "/opt/firedancer/build/native/gcc" CACHE FILEPATH "Path to Firedancer build dir")

add_executable(fdgen src/app/fdgen_main.c)

target_include_directories(fdgen SYSTEM PUBLIC ${FIREDANCER_BUILD}/include)
target_link_libraries(fdgen -pthread -lstdc++ ${FIREDANCER_BUILD}/lib/libfd_util.a)
