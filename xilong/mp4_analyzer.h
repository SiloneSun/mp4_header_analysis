#ifndef MP4_ANALYZER_H
#define MP4_ANALYZER_H

#include <string>
#include <vector>
#include <cstdint>

namespace sunxilong {

// 分析参数
struct AnalyzeOptions {
    double preset_fps = 0.0;      // 预设帧率, 0 表示自动采用容器头帧率
    double drop_ratio = 1.5;      // 丢帧判定: 帧间隔 > 预期间隔 * drop_ratio
    double fast_ratio = 0.5;      // 超快帧判定: 帧间隔 < 预期间隔 * fast_ratio
    double fps_tolerance = 0.05;  // 帧率符合判定容差(±5%)
};

// 单个视频帧信息
struct FrameInfo {
    double pts = 0.0;    // 时间戳(秒)
    int64_t size = 0;    // 包大小(字节)
    int64_t raw_pts = 0; // 原始 pts(时间基单位)
};

// 一次帧间隔异常事件
struct GapEvent {
    int frame_index = 0;     // 间隔末端帧序号(从0开始)
    double t = 0.0;          // 末端帧时间戳(秒)
    double gap_ms = 0.0;     // 实际帧间隔(ms)
    double expected_ms = 0.0;// 预期帧间隔(ms)
    int dropped_count = 0;   // 估计丢失帧数(仅丢帧事件有意义)
};

// 码率统计(按秒聚合)
struct BitrateStats {
    double avg_kbps = 0.0;
    double max_kbps = 0.0;
    double min_kbps = 0.0;
    double stddev_kbps = 0.0;
    std::vector<double> per_second_kbps; // 每秒码率(kbps)
};

// 分析结果
struct AnalyzeResult {
    // 文件/流信息
    std::string file_path;
    std::string codec_name;
    int width = 0;
    int height = 0;
    double container_fps = 0.0;   // 容器头标称帧率
    double duration_sec = 0.0;    // 视频时长(秒)
    int64_t video_bytes = 0;      // 视频流总字节数
    int64_t audio_bytes = 0;      // 其余流(音频等)总字节数

    // 帧率检查
    double preset_fps = 0.0;      // 实际采用的预设帧率
    double measured_fps = 0.0;    // 实测帧率
    bool fps_ok = false;          // 帧率是否符合预设

    // 帧与间隔
    std::vector<FrameInfo> frames;      // 按 pts 排序的帧
    std::vector<double> intervals_ms;   // 相邻帧间隔(ms), 与 frames[1..] 对齐
    std::vector<GapEvent> drop_events;  // 丢帧事件
    std::vector<GapEvent> fast_events;  // 超快帧事件
    int total_dropped_frames = 0;       // 估计丢失帧总数
    int bad_pts_count = 0;              // pts 不单调/重复的数量

    BitrateStats vbr;                   // 视频码率统计

    bool valid = false;
    std::string error;
};

// 分析入口, 成功返回0
int analyze_mp4(const std::string& path, const AnalyzeOptions& opt, AnalyzeResult& res);

} // namespace sunxilong

#endif // MP4_ANALYZER_H
