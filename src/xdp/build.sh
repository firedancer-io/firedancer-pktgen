#!/bin/bash

# Script to rebuild XDP program.
#
# Clang is the only supported eBPF compiler for now.
# Some versions of Clang attempt to build with stack protection
# which is not supported for the eBPF target -- the kernel verifier
# provides such safety features.

clang                           \
  -std=c17                      \
  -target bpf                   \
  -O2                           \
  -fno-stack-protector          \
  -c -o fd_xdp_ports_redirect.o \
  fd_xdp_ports_redirect.c

