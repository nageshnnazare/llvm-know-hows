cmake_minimum_required(VERSION 3.20)
project(mul_to_add CXX)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

# Build a SHARED library (a pass plugin) loadable by `opt -load-pass-plugin`.
add_library(MulToAdd MODULE MulToAdd.cpp)
target_compile_features(MulToAdd PRIVATE cxx_std_17)

# Plugins resolve LLVM symbols from the opt process; do not link libLLVM here.
set_target_properties(MulToAdd PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  # macOS: allow undefined symbols (resolved by the host `opt`).
  LINK_FLAGS "$<$<PLATFORM_ID:Darwin>:-undefined dynamic_lookup>")
