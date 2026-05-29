cmake_minimum_required(VERSION 3.16)
project(ascend_d2h_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ASCEND_HOME_PATH "$ENV{ASCEND_HOME_PATH}" CACHE PATH "Ascend toolkit path")
if(NOT ASCEND_HOME_PATH)
    set(ASCEND_HOME_PATH "/usr/local/Ascend/ascend-toolkit/latest")
endif()

find_path(ACL_INCLUDE_DIR
    NAMES acl/acl.h
    HINTS
        "${ASCEND_HOME_PATH}/include"
        "${ASCEND_HOME_PATH}/include/aclnn"
        "/usr/local/Ascend/ascend-toolkit/latest/include"
)

find_library(ASCENDCL_LIBRARY
    NAMES ascendcl
    HINTS
        "${ASCEND_HOME_PATH}/lib64"
        "${ASCEND_HOME_PATH}/lib"
        "/usr/local/Ascend/ascend-toolkit/latest/lib64"
)

if(NOT ACL_INCLUDE_DIR)
    message(FATAL_ERROR "Cannot find acl/acl.h. Set ASCEND_HOME_PATH or ACL_INCLUDE_DIR.")
endif()
if(NOT ASCENDCL_LIBRARY)
    message(FATAL_ERROR "Cannot find libascendcl. Set ASCEND_HOME_PATH or ASCENDCL_LIBRARY.")
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${ACL_INCLUDE_DIR}")
set(CMAKE_REQUIRED_LIBRARIES "${ASCENDCL_LIBRARY}")

check_cxx_source_compiles("
#include <acl/acl.h>
int main() {
    auto fn = &aclrtHostRegisterV2;
    (void)fn;
    return 0;
}
" HAVE_ACLRT_HOST_REGISTER_V2)

check_cxx_source_compiles("
#include <acl/acl.h>
int main() {
    aclrtMemcpyBatchAttr attr{};
    size_t fail = 0;
    (void)attr;
    (void)fail;
    return 0;
}
" HAVE_ACLRT_MEMCPY_BATCH)

add_executable(ascend_d2h_bench src/ascend_d2h_bench.cpp)
target_include_directories(ascend_d2h_bench PRIVATE "${ACL_INCLUDE_DIR}")
target_link_libraries(ascend_d2h_bench PRIVATE "${ASCENDCL_LIBRARY}" pthread)
target_compile_definitions(ascend_d2h_bench PRIVATE _GNU_SOURCE)

if(HAVE_ACLRT_HOST_REGISTER_V2)
    target_compile_definitions(ascend_d2h_bench PRIVATE HAVE_ACLRT_HOST_REGISTER_V2=1)
endif()
if(HAVE_ACLRT_MEMCPY_BATCH)
    target_compile_definitions(ascend_d2h_bench PRIVATE HAVE_ACLRT_MEMCPY_BATCH=1)
endif()
