cmake_minimum_required(VERSION 3.20)
project(hello_ir CXX)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

add_executable(hello_ir hello_ir.cpp)
target_compile_features(hello_ir PRIVATE cxx_std_17)

llvm_map_components_to_libnames(llvm_libs core support)
target_link_libraries(hello_ir PRIVATE ${llvm_libs})
