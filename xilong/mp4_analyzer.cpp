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

// 音频流基本信息(在关闭 fmt_ctx 前提取, 避免 use-after-free)
struct AudioStreamInfo {
    std::string codec_name;
    int sample_rate = 0;
    int channels = 0;
    int frame_size = 0;
};

// 音频流分析: 基于相邻包 PTS 间隔检测丢包
static void analyze_audio(const AudioStreamInfo& info, const std::vector<FrameInfo>& pkts,
                          const AnalyzeOptions& opt, AudioResult& ar)
{
    if (pkts.size() < 2) {
        ar.note = "音频包数不足, 无法分析";
        return;
    }

    ar.codec_name = info.codec_name;
    ar.sample_rate = info.sample_rate;
    ar.channels = info.channels;
    ar.frame_size = info.frame_size;
    ar.packet_count = (int)pkts.size();
    for (const auto& p : pkts) ar.total_bytes += p.size;

    // 按 pts 排序(MP4 音频通常单调, 保险起见)
    std::vector<FrameInfo> sorted = pkts;
    std::sort(sorted.begin(), sorted.end(),
              [](const FrameInfo& a, const FrameInfo& b) { return a.pts < b.pts; });

    const double span = sorted.back().pts - sorted.front().pts;
    ar.duration_sec = (span > 0) ? span : 0.0;

    // 预期包时长: frame_size / sample_rate (AAC=1024, MP3=1152)
    // frame_size 无法确定时(部分容器未提供), 从相邻间隔众数估计
    double exp_ms = 0.0;
    if (ar.frame_size > 0 && ar.sample_rate > 0) {
        exp_ms = 1000.0 * ar.frame_size / ar.sample_rate;
    } else {
        // 估计: 取间隔中位数(丢包是少数, 中位数更鲁棒)
        std::vector<double> gaps;
        gaps.reserve(sorted.size() - 1);
        for (size_t i = 1; i < sorted.size(); ++i) {
            double g = (sorted[i].pts - sorted[i - 1].pts) * 1000.0;
            if (g > 0.5) gaps.push_back(g);
        }
        if (gaps.size() >= 10) {
            std::sort(gaps.begin(), gaps.end());
            exp_ms = gaps[gaps.size() / 2];
            ar.note = "包时长由间隔中位数估计(" + std::to_string((int)std::llround(exp_ms)) + "ms)";
        } else {
            ar.note = "包时长未知且样本不足, 未检测丢包";
        }
    }
    ar.expected_pkt_ms = exp_ms;

    // 丢包检测: 间隔 > 预期 * drop_ratio
    if (exp_ms > 0) {
        const double drop_th_ms = exp_ms * opt.drop_ratio;
        for (size_t i = 1; i < sorted.size(); ++i) {
            double gap_ms = (sorted[i].pts - sorted[i - 1].pts) * 1000.0;
            if (gap_ms <= 0) continue;
            if (gap_ms > drop_th_ms) {
                GapEvent ev;
                ev.frame_index = (int)i;
                ev.t = sorted[i].pts;
                ev.gap_ms = gap_ms;
                ev.expected_ms = exp_ms;
                ev.dropped_count = (int)std::llround(gap_ms / exp_ms) - 1;
                if (ev.dropped_count < 1) ev.dropped_count = 1;
                ar.drop_events.push_back(ev);
                ar.total_dropped_packets += ev.dropped_count;
            }
        }
    }

    // 音频码率统计: 按秒聚合
    double dur = (ar.duration_sec > 0) ? ar.duration_sec : span;
    int nbuckets = (int)std::ceil(dur);
    if (nbuckets < 1) nbuckets = 1;
    std::vector<double> kbits(nbuckets, 0.0);
    const double t0 = sorted.front().pts;
    for (size_t i = 0; i < sorted.size(); ++i) {
        int b = (int)(sorted[i].pts - t0);
        if (b < 0) b = 0;
        if (b >= nbuckets) b = nbuckets - 1;
        kbits[b] += sorted[i].size * 8.0 / 1000.0;
    }
    ar.vbr.per_second_kbps = kbits;
    double sum = 0.0, sumsq = 0.0;
    double vmin = 1e18, vmax = -1.0;
    for (double v : kbits) {
        sum += v;
        sumsq += v * v;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    const double cnt = (double)kbits.size();
    ar.vbr.avg_kbps = sum / cnt;
    ar.vbr.min_kbps = vmin;
    ar.vbr.max_kbps = vmax;
    const double var = sumsq / cnt - ar.vbr.avg_kbps * ar.vbr.avg_kbps;
    ar.vbr.stddev_kbps = (var > 0) ? std::sqrt(var) : 0.0;

    ar.valid = true;
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

    // 查找音频流(可选)
    int aidx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    AVStream* ast = (aidx >= 0) ? fmt_ctx->streams[aidx] : nullptr;

    // 提前提取音频流信息(fmt_ctx 关闭后不可再访问)
    AudioStreamInfo ainfo;
    if (ast) {
        const AVCodecParameters* cp = ast->codecpar;
        const AVCodecDescriptor* adesc = avcodec_descriptor_get(cp->codec_id);
        ainfo.codec_name = adesc ? adesc->name : "unknown";
        ainfo.sample_rate = cp->sample_rate;
        ainfo.channels = cp->ch_layout.nb_channels;
        ainfo.frame_size = cp->frame_size;
    }

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

    // 遍历所有包, 收集视频帧与音频包的时间戳与大小(不解码, 仅用包信息)
    std::vector<FrameInfo> frames;
    std::vector<FrameInfo> apackets;  // 音频包
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
        } else if (ast && pkt->stream_index == aidx) {
            int64_t ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
            if (ts != AV_NOPTS_VALUE) {
                FrameInfo f;
                f.raw_pts = ts;
                f.pts = ts * av_q2d(ast->time_base);
                f.size = pkt->size;
                apackets.push_back(f);
            }
            res.audio_bytes += pkt->size;
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

    // ---- 音频分析: 丢包检测 + 码率统计 ----
    if (ast) {
        analyze_audio(ainfo, apackets, opt, res.audio);
    } else {
        res.audio.note = "无音频流";
    }

    res.valid = true;
    return 0;
}

} // namespace sunxilong
