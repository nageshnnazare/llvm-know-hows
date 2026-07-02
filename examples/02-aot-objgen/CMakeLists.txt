cmake_minimum_required(VERSION 3.20)
project(aot_objgen CXX)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

add_executable(aot aot.cpp)
target_compile_features(aot PRIVATE cxx_std_17)

# 'native' expands to the host backend (e.g. X86CodeGen + X86AsmPrinter + ...).
llvm_map_components_to_libnames(llvm_libs core support passes native)
target_link_libraries(aot PRIVATE ${llvm_libs})
