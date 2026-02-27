cmake_minimum_required(VERSION 3.10)
project(convertserver VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 设置 MACA 环境变量
if(NOT DEFINED ENV{MACA_PATH})
    set(ENV{MACA_PATH} "/opt/maca")
endif()
if(NOT DEFINED ENV{CUDA_PATH})
    set(ENV{CUDA_PATH} "/usr/local/cuda")
endif()

# 检测编译器
if(DEFINED ENV{MMSEQS_CUDA_COMPILER})
    set(CUDA_COMPILER $ENV{MMSEQS_CUDA_COMPILER})
else()
    set(CUDA_COMPILER "mxcc")
endif()

# 源文件
set(CONVERTSERVER_SRC
    src/convertserver.cpp
    src/convertalis_fast.cpp
)

# 头文件路径 - 引用 MMseqs2 源码
set(MMSEQS2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../jin/MMseqs2")
include_directories(
    ${MMSEQS2_SRC}/src
    ${MMSEQS2_SRC}/lib/mmseqs/lib
    ${MMSEQS2_SRC}/lib/ksw2/src
    ${MMSEQS2_SRC}/lib/zstd/lib
    ${MMSEQS2_SRC}/lib/cdzlib
    ${MMSEQS2_SRC}/lib/tinyexpr
    ${MMSEQS2_SRC}/lib/ffindex/src
    ${MMSEQS2_SRC}/lib/cacode/src
)

# 编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -fPIC -Wall -Wno-unused-function")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
endif()

# OpenMP
find_package(OpenMP)
if(OPENMP_FOUND)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
endif()

# 链接库
set(COMMON_LIBS pthread z m dl)

# 可执行文件
add_executable(convertserver src/convertserver.cpp)
target_link_libraries(convertserver ${COMMON_LIBS})

add_executable(convertalis-fast src/convertalis_fast.cpp)
target_link_libraries(convertalis-fast ${COMMON_LIBS})

# 如果需要链接 MMseqs2 库
add_subdirectory(${MMSEQS2_SRC}/lib/mmseqs/lib mmseqs-lib)

target_link_libraries(convertserver mmseqs)
target_link_libraries(convertalis-fast mmseqs)

# 安装
install(TARGETS convertserver convertalis-fast DESTINATION bin)

# 打印配置信息
message(STATUS "=== convertserver Build Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "CXX compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "MMseqs2 source: ${MMSEQS2_SRC}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
