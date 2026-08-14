#include "mp4_analyzer.h"
#include "t_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

namespace sunxilong {

static std::string averr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

int analyze_mp4(const std::string& path, const AnalyzeOptions& opt, AnalyzeResult& res)
{
    res.file_path = path;

    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        res.error = "打开文件失败: " + averr(ret);
        return ret;
    }
    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        res.error = "读取流信息失败: " + averr(ret);
        avformat_close_input(&fmt_ctx);
        return ret;
    }

    int vidx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0) {
        res.error = "未找到视频流";
        avformat_close_input(&fmt_ctx);
        return vidx;
    }
    AVStream* vst = fmt_ctx->streams[vidx];

    const AVCodecDescriptor* desc = avcodec_descriptor_get(vst->codecpar->codec_id);
    res.codec_name = desc ? desc->name : "unknown";
    res.width = vst->codecpar->width;
    res.height = vst->codecpar->height;

    // 容器帧率: 优先 av_guess_frame_rate, 回退 avg_frame_rate
    AVRational fr = av_guess_frame_rate(fmt_ctx, vst, nullptr);
    if (fr.num <= 0 || fr.den <= 0) fr = vst->avg_frame_rate;
    res.container_fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;
    res.preset_fps = (opt.preset_fps > 0) ? opt.preset_fps : res.container_fps;
    if (res.preset_fps <= 0) {
        res.error = "无法确定帧率: 容器头无帧率信息, 请通过 --fps 指定预设帧率";
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    // 遍历所有包, 收集视频帧时间戳与大小(不解码, 仅用包信息)
    std::vector<FrameInfo> frames;
    if (vst->nb_frames > 0) frames.reserve((size_t)vst->nb_frames);
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == vidx) {
            int64_t ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
            if (ts != AV_NOPTS_VALUE) {
                FrameInfo f;
                f.raw_pts = ts;
                f.pts = ts * av_q2d(vst->time_base);
                f.size = pkt->size;
                frames.push_back(f);
                res.video_bytes += pkt->size;
            }
        } else {
            res.audio_bytes += pkt->size;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);

    if (frames.size() < 2) {
        res.error = "视频帧数不足(少于2帧), 无法分析";
        return -1;
    }

    // MP4 含 B 帧时 pts 非单调, 按 pts 排序后再分析间隔
    std::sort(frames.begin(), frames.end(),
              [](const FrameInfo& a, const FrameInfo& b) { return a.pts < b.pts; });
    res.frames = frames;
    const size_t n = frames.size();

    const double span = frames.back().pts - frames.front().pts;
    res.duration_sec = (span > 0) ? span : 0.0;

    // ---- 帧率检查 ----
    res.measured_fps = (span > 0) ? (double)(n - 1) / span : 0.0;
    res.fps_ok = std::fabs(res.measured_fps - res.preset_fps) / res.preset_fps <= opt.fps_tolerance;

    // ---- 帧间隔 / 丢帧 / 超快帧检测 ----
    const double expected_ms = 1000.0 / res.preset_fps;
    const double drop_th_ms = expected_ms * opt.drop_ratio;
    const double fast_th_ms = expected_ms * opt.fast_ratio;
    res.intervals_ms.reserve(n - 1);
    for (size_t i = 1; i < n; ++i) {
        double gap_ms = (frames[i].pts - frames[i - 1].pts) * 1000.0;
        res.intervals_ms.push_back(gap_ms);
        if (gap_ms <= 0) {
            res.bad_pts_count++;
            continue;
        }
        if (gap_ms > drop_th_ms) {
            GapEvent ev;
            ev.frame_index = (int)i;
            ev.t = frames[i].pts;
            ev.gap_ms = gap_ms;
            ev.expected_ms = expected_ms;
            ev.dropped_count = (int)std::llround(gap_ms / expected_ms) - 1;
            if (ev.dropped_count < 1) ev.dropped_count = 1;
            res.drop_events.push_back(ev);
            res.total_dropped_frames += ev.dropped_count;
        } else if (gap_ms < fast_th_ms) {
            GapEvent ev;
            ev.frame_index = (int)i;
            ev.t = frames[i].pts;
            ev.gap_ms = gap_ms;
            ev.expected_ms = expected_ms;
            ev.dropped_count = 0;
            res.fast_events.push_back(ev);
        }
    }

    // ---- 码率统计: 按秒聚合 ----
    double dur = (res.duration_sec > 0) ? res.duration_sec : span;
    int nbuckets = (int)std::ceil(dur);
    if (nbuckets < 1) nbuckets = 1;
    std::vector<double> kbits(nbuckets, 0.0);
    const double t0 = frames.front().pts;
    for (size_t i = 0; i < n; ++i) {
        int b = (int)(frames[i].pts - t0);
        if (b < 0) b = 0;
        if (b >= nbuckets) b = nbuckets - 1;
        kbits[b] += frames[i].size * 8.0 / 1000.0; // 1秒内的 kbit 数即 kbps
    }
    res.vbr.per_second_kbps = kbits;
    double sum = 0.0, sumsq = 0.0;
    double vmin = 1e18, vmax = -1.0;
    for (double v : kbits) {
        sum += v;
        sumsq += v * v;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    const double cnt = (double)kbits.size();
    res.vbr.avg_kbps = sum / cnt;
    res.vbr.min_kbps = vmin;
    res.vbr.max_kbps = vmax;
    const double var = sumsq / cnt - res.vbr.avg_kbps * res.vbr.avg_kbps;
    res.vbr.stddev_kbps = (var > 0) ? std::sqrt(var) : 0.0;

    res.valid = true;
    return 0;
}

} // namespace sunxilong
