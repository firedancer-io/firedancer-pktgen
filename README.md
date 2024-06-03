**firedancer-pktgen** is a network traffic generator using Firedancer components.

## Setup

This project depends on CMake.
It also requires a local installation of Firedancer.

**Configure**

```sh
cmake -B build -DFIREDANCER_BUILD=/opt/firedancer/build/native/gcc
# Adjust the `FIREDANCER_BUILD` CMake variable as needed.
```

**Build**

```sh
cmake --build build -j
```

**Run**

```sh
build/fdgen --help
```
