cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
project(Deft)

# disable boost warning
add_definitions(-DBOOST_COROUTINES_NO_DEPRECATION_WARNING)

# Compiler options
set(CMAKE_CXX_STANDARD 20)
add_compile_options(-Wall -Wno-deprecated-declarations -Wsign-compare)

set(INCLUDE_BASE ${PROJECT_SOURCE_DIR}/include)
set(INCLUDE_CXL ${PROJECT_SOURCE_DIR}/include/cxl)
set(COMMON_SRC ${PROJECT_SOURCE_DIR}/src)
set(TEST_SRC_DIR ${PROJECT_SOURCE_DIR}/test)

set(RDMA_LINK_FLAGS
    -lcityhash
    -lboost_system
    -lboost_coroutine
    -lpthread
    -libverbs
    -lmemcached
    -lgflags)

set(CXL_LINK_FLAGS
    -lcityhash
    -lboost_system
    -lboost_coroutine
    -lpthread
    -lnuma
    -lgflags)

# ===============================
# RDMA library / executables
# ===============================
file(GLOB_RECURSE RDMA_COMMON_FILE ${COMMON_SRC}/*.cpp)
list(FILTER RDMA_COMMON_FILE EXCLUDE REGEX ".*/cxl/.*")

add_library(deft STATIC ${RDMA_COMMON_FILE})
target_include_directories(deft PUBLIC ${INCLUDE_BASE})

file(GLOB TEST_SRC ${TEST_SRC_DIR}/*.cpp)
list(REMOVE_ITEM TEST_SRC ${TEST_SRC_DIR}/deft_cxl.cpp)

foreach(TEST ${TEST_SRC})
    get_filename_component(TEST_NAME ${TEST} NAME_WE)
    add_executable(${TEST_NAME} ${TEST})
    target_link_libraries(${TEST_NAME} deft ${RDMA_LINK_FLAGS})
endforeach()

# ===============================
# CXL library / executable
# ===============================
set(CXL_SOURCES
    ${PROJECT_SOURCE_DIR}/src/cxl/dsm_client_cxl.cpp
)

set(CXL_SHARED_SOURCES
    ${PROJECT_SOURCE_DIR}/src/Tree.cpp
    ${PROJECT_SOURCE_DIR}/src/Common.cpp
    ${PROJECT_SOURCE_DIR}/src/Cache.cpp
    ${PROJECT_SOURCE_DIR}/src/Debug.cpp
)

add_library(deft_cxl_lib STATIC ${CXL_SOURCES} ${CXL_SHARED_SOURCES})
target_include_directories(deft_cxl_lib PUBLIC ${INCLUDE_CXL} ${INCLUDE_BASE})
target_compile_definitions(deft_cxl_lib PUBLIC DEFT_CXL)

add_executable(deft_cxl ${TEST_SRC_DIR}/deft_cxl.cpp)
target_include_directories(deft_cxl PRIVATE ${INCLUDE_CXL} ${INCLUDE_BASE})
target_compile_definitions(deft_cxl PRIVATE DEFT_CXL)
target_link_libraries(deft_cxl deft_cxl_lib ${CXL_LINK_FLAGS})
