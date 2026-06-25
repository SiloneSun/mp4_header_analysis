cmake_minimum_required(VERSION 3.10)

# 项目名称和使用的语言（CXX 表示 C++）
project(analysis_mp4 CXX)

# 设置 C++ 语言标准为 C++11
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================
# 源文件
# ============================================
set(SOURCES
    main.cpp
    xilong/mp4_parser.cpp
)

# ============================================
# 头文件路径
# ============================================
include_directories(
    include
    xilong
)

# ============================================
# 生成可执行文件
# ============================================
add_executable(analysis_mp4 ${SOURCES})