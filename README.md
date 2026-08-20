# MP4 Header Analysis

`mp4_header_analysis` 是一个用于检查 MP4 文件结构和媒体质量的 C++ 工具。它既能递归解析 MP4 的 Box 层级与常见字段，也能基于 FFmpeg 读取音视频包时间戳，定位帧率异常、视频丢帧、超快帧、音频丢包及码率波动等问题。

适用于排查录像文件、转封装文件或设备生成 MP4 文件的结构与时序异常。

## 功能

- 递归解析 MP4 Box 树，输出 Box 类型、文件偏移、大小和层级关系。
- 解析常见 Box 的可读字段，包括 `ftyp`、`mvhd`、`tkhd`、`mdhd`、`hdlr`、`stsd`、`stts`、`stsc`、`stsz`、`stco`、`elst` 和 `mdat` 等。
- 识别并解析 SeeVision UUID Box 中的 SXMD 私有元数据，包括普通录像、慢动作、静止延时、运动延时、轨迹延时、无音频和时间戳修改标志。
- 获取视频编码格式、分辨率、时长、容器标称帧率、实测帧率和视频帧数。
- 根据相邻视频帧 PTS 间隔检测丢帧和超快帧，并估算丢失帧数。
- 检查实测帧率是否落在目标帧率的 ±5% 范围内。
- 统计视频和音频每秒码率、平均码率、最小/最大码率及标准差。
- 分析音频编码、采样率、声道数和音频包 PTS，检测疑似音频丢包。
- 生成适合人工查看的 HTML 报告、适合 AI/文本处理的 Markdown 报告，或 Box 结构 TXT 文件。

## 依赖

- Linux
- CMake 3.10 或更高版本
- 支持 C++11 的编译器
- FFmpeg 开发产物：`libavformat`、`libavcodec`、`libavutil`、`libswresample` 及对应头文件
- `liblzma`、zlib、pthread

项目默认从以下位置查找 FFmpeg：

```text
$HOME/work/mycode/ffmpeg-snapshot-git/my_build
```

该目录应包含 `include/` 与 `lib/`。如实际位置不同，可在 CMake 配置时通过 `FFMPEG_ROOT` 覆盖。

## 构建

```bash
cmake -S . -B build -DFFMPEG_ROOT=/path/to/ffmpeg-install
cmake --build build -j
```

构建完成后，可执行文件为：

```text
build/analysis_mp4
```

## 使用方法

```bash
./analysis_mp4 <mp4_file> [options]
```

示例：

```bash
# 默认：执行 Box + 质量分析，并同时生成 HTML 和 Markdown 报告
./analysis_mp4 /data/sample.mp4

# 指定预期帧率，用于帧率、丢帧和超快帧判断
./analysis_mp4 /data/sample.mp4 -fps 30

# 只解析 Box 结构，不执行音视频质量分析
./analysis_mp4 /data/sample.mp4 -no-qc

# 只生成 Markdown 报告
./analysis_mp4 /data/sample.mp4 -md

# 只生成 Box 结构 TXT 文件
./analysis_mp4 /data/sample.mp4 -txt
```

### 命令行参数

| 参数 | 说明 |
| --- | --- |
| `-fps <fps>` | 指定预期帧率。未指定时，自动使用容器提供的帧率。 |
| `-no-qc` | 跳过质量分析，仅解析 MP4 Box 结构。 |
| `-txt` | 输出 Box 结构 TXT 文件。 |
| `-md` | 输出 Markdown 综合报告。 |

默认不带 `-txt` 或 `-md` 时，程序输出 HTML 和 Markdown 两份综合报告。

## 输出文件

输出文件与源 MP4 位于同一目录，名称由输入文件名生成：

| 输出模式 | 文件名 |
| --- | --- |
| 默认 | `<name>_report.html` 和 `<name>_report.md` |
| `-md` | `<name>_report.md` |
| `-txt` | `<name>_header.txt` |

例如分析 `/data/demo.mp4` 时，默认生成：

```text
/data/demo_report.html
/data/demo_report.md
```

程序也会在终端输出 Box 树、质量分析摘要和已识别的 SXMD 私有元数据。

## 检测规则

质量分析直接读取封装包信息，不解码音视频内容。视频帧按 PTS 排序后进行统计，以降低 B 帧导致时间戳非单调的影响。

| 项目 | 判定方式 |
| --- | --- |
| 帧率 | 实测帧率为首尾 PTS 时间跨度内的帧间隔平均值；与目标帧率误差不超过 ±5% 为通过。 |
| 视频丢帧 | 相邻视频帧 PTS 间隔大于预期间隔的 1.5 倍。 |
| 超快帧 | 相邻视频帧 PTS 间隔小于预期间隔的 0.5 倍。 |
| 视频码率 | 按秒聚合视频包大小，统计平均值、极值和标准差。 |
| 音频丢包 | 相邻音频包 PTS 间隔大于预期包时长的 1.5 倍。预期时长优先由 `frame_size / sample_rate` 计算；无法获取时尝试用包间隔中位数估计。 |
| 音频码率 | 按秒聚合音频包大小，统计平均值、极值和标准差。 |

未指定 `-fps` 时，目标帧率取自 FFmpeg 识别到的容器帧率；若容器未提供有效帧率，则需要手动指定 `-fps`。

## 报告内容

HTML 报告面向浏览器查看，包含 MP4 基本信息、质量分析摘要、异常事件、码率信息、Box 树以及常见 Box 字段。Markdown 报告包含相同的核心分析结果，便于提交给 AI agent 或纳入问题单。TXT 模式仅输出 Box 结构和字段信息。

## 项目结构

```text
.
├── main.cpp                 # 命令行入口、参数解析和报告生成
├── CMakeLists.txt           # 构建配置及 FFmpeg 链接配置
├── include/
│   └── t_log.h              # 日志接口
└── xilong/
    ├── mp4_parser.*         # MP4 Box 解析、字段导出与 SXMD 识别
    ├── mp4_analyzer.*       # 基于 FFmpeg 的音视频质量分析
    └── html_report.*        # HTML / Markdown 报告生成
```

## 注意事项

- 检测结果依据封装层的包大小和 PTS，不代表视频画面内容或音频波形质量。
- 丢帧、超快帧和音频丢包均为基于时间戳间隔的疑似异常，需要结合编码参数、变帧率场景和业务录制逻辑进一步确认。
- 对于变帧率视频，建议显式传入符合业务预期的 `-fps`，以避免将正常的时间戳变化误判为异常。
- 当前工具以 MP4 文件为主要输入目标；异常或损坏文件的解析结果应结合原始文件和其他媒体工具交叉验证。
