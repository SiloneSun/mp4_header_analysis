cmake_minimum_required(VERSION 3.10)

# 项目名称和使用的语言（CXX 表示 C++）
project(analysis_mp4 CXX)

# 设置 C++ 语言标准为 C++11
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================
# ffmpeg 路径（源码编译产物，可通过 -DFFMPEG_ROOT=xxx 覆盖）
# 源码位置: ~/work/mycode/ffmpeg-snapshot-git/ffmpeg
# ============================================
set(FFMPEG_ROOT "$ENV{HOME}/work/mycode/ffmpeg-snapshot-git/my_build"
    CACHE PATH "ffmpeg 编译输出目录（含 include/ 和 lib/）")

# ============================================
# 源文件
# ============================================
set(SOURCES
    main.cpp
    xilong/mp4_parser.cpp
    xilong/html_report.cpp
    xilong/mp4_analyzer.cpp
)

# ============================================
# 头文件路径
# ============================================
include_directories(
    include
    xilong
    ${FFMPEG_ROOT}/include
)

# ============================================
# 链接库目录（等价于 -L）
# ============================================
link_directories(
    ${FFMPEG_ROOT}/lib
    /usr/lib
    /usr/lib/x86_64-linux-gnu
)

# ============================================
# 生成可执行文件
# ============================================
add_executable(analysis_mp4 ${SOURCES})

# 静态库链接顺序有要求：依赖者在前，被依赖者在后
target_link_libraries(analysis_mp4 PRIVATE
    avformat            # -lavformat  （封装格式库）
    avcodec             # -lavcodec   （编解码库）
    swresample          # -lswresample（音频重采样）
    avutil              # -lavutil    （工具库）
    lzma                # -llzma
    z                   # -lz         （zlib 压缩库）
    m                   # -lm         （数学库）
    pthread             # -lpthread
)
