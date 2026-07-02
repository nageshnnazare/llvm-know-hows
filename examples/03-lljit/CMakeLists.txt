cmake_minimum_required(VERSION 3.20)
project(lljit_demo CXX)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

add_executable(jit jit.cpp)
target_compile_features(jit PRIVATE cxx_std_17)

# orcjit pulls in the ORC framework; native = host backend for codegen.
llvm_map_components_to_libnames(llvm_libs core support orcjit native)
target_link_libraries(jit PRIVATE ${llvm_libs})
