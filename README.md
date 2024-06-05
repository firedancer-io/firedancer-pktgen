**firedancer-pktgen** is a network traffic generator using Firedancer components.

## Setup

This project depends on CMake.
It also requires a local installation of Firedancer.

**Configure**

```sh
# Adjust the `FIREDANCER_BUILD` CMake variable as needed.
cmake -B build \
  -DFIREDANCER_BUILD=/opt/firedancer/build/native/gcc \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=1

# Optional: Set up compile_commands.json for clangd / VSCode.
ln -s build/compile_commands.json compile_commands.json
```

**Build**

```sh
cmake --build build -j
```

**Run**

```sh
build/fdgen --help
```
