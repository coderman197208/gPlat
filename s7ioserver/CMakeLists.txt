cmake_minimum_required(VERSION 3.12)
project(s7ioserver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置输出目录（Debug 配置）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/../x64/Debug")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/../x64/Debug")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/../x64/Debug")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/../x64/Release")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/../x64/Release")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/../x64/Release")

# 设置目标平台相关选项
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_definitions(-DLINUX)
endif()

# 设置调试和发布版本的编译选项
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/../include
)

# 源文件列表
set(SRC_FILES
    main.cpp
    s7config.cpp
    s7log.cpp
    threadReadPlc.cpp
    threadWritePlc.cpp
)

# 头文件列表
set(HEADER_FILES
    s7config.h
    s7log.h
)

# 添加库搜索路径
link_directories(
    ${CMAKE_SOURCE_DIR}/../x64/Debug
    /usr/local/lib
    /usr/lib
)

# 创建可执行文件
add_executable(s7ioserver ${SRC_FILES} ${HEADER_FILES})

# 链接库
target_link_libraries(s7ioserver PRIVATE
    pthread
    snap7
    higplat
)

# 安装规则 (可选)
install(TARGETS s7ioserver
    RUNTIME DESTINATION bin
)

# 多配置支持
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES Debug Release)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING
        "Reset the configurations to what we need" FORCE)
endif()
