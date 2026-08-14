#include <iostream>
#include <string>
#include "t_log.h"
#include "mp4_parser.h"
#include "mp4_analyzer.h"
#include "html_report.h"

int main(int argc, char* argv[])
{
    if (argc < 2) {
        printf("Usage: %s <mp4_file> [options]\n", argv[0]);
        printf("Options:\n");
        printf("  -fps <fps>     预设帧率 (默认自动检测)\n");
        printf("  -no-qc         跳过质量分析 (仅分析 Box 结构)\n");
        printf("  -txt           输出 TXT 格式 (默认输出 HTML)\n");
        printf("  -md            输出 Markdown 格式 (面向 AI agent 阅读)\n");
        return 1;
    }

    const char* filePath = argv[1];
    enum { FMT_HTML, FMT_TXT, FMT_MD } output_fmt = FMT_HTML;
    bool skip_qc = false;
    double preset_fps = 0.0;

    // 解析参数
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "-txt") {
            output_fmt = FMT_TXT;
        } else if (std::string(argv[i]) == "-md") {
            output_fmt = FMT_MD;
        } else if (std::string(argv[i]) == "-fps" && i + 1 < argc) {
            preset_fps = atof(argv[++i]);
        } else if (std::string(argv[i]) == "-no-qc") {
            skip_qc = true;
        }
    }

    std::string inputPath(filePath);
    
    // 从输入文件路径中提取文件名（不含目录）
    size_t lastSlash = inputPath.find_last_of("/\\");
    std::string fileName = (lastSlash == std::string::npos) ? inputPath : inputPath.substr(lastSlash + 1);
    
    // 移除扩展名
    size_t lastDot = fileName.find_last_of('.');
    std::string baseName = (lastDot == std::string::npos) ? fileName : fileName.substr(0, lastDot);

    LOGD("Input file: %s", filePath);
    LOGD("Output format: %s", output_fmt == FMT_HTML ? "HTML" : (output_fmt == FMT_MD ? "Markdown" : "TXT"));

    // ============================================
    // 1. Box 结构分析
    // ============================================
    Mp4Parser parser;
    if (!parser.parse(filePath)) {
        LOGD("Failed to parse MP4 file: %s", filePath);
        return 1;
    }

    // 打印 tree 到控制台
    parser.print_tree();

    // ============================================
    // 2. 质量分析 (可选)
    // ============================================
    sunxilong::AnalyzeResult qc_result;
    sunxilong::AnalyzeOptions qc_opt;
    qc_opt.preset_fps = preset_fps;
    bool has_qc = false;

    if (!skip_qc) {
        printf("\n=== 质量分析 ===\n");
        int ret = sunxilong::analyze_mp4(filePath, qc_opt, qc_result);
        if (ret == 0 && qc_result.valid) {
            has_qc = true;
            printf("编码: %s, 分辨率: %dx%d\n", qc_result.codec_name.c_str(), qc_result.width, qc_result.height);
            printf("时长: %.2f s, 帧数: %zu\n", qc_result.duration_sec, qc_result.frames.size());
            printf("容器帧率: %.3f fps, 实测帧率: %.3f fps\n", qc_result.container_fps, qc_result.measured_fps);
            printf("帧率检查: %s\n", qc_result.fps_ok ? "PASS" : "FAIL");
            printf("丢帧事件: %zu 处, 估计丢失帧: %d\n", qc_result.drop_events.size(), qc_result.total_dropped_frames);
            printf("超快帧事件: %zu 处\n", qc_result.fast_events.size());
            printf("平均码率: %.0f kbps, 波动: %.0f kbps\n", qc_result.vbr.avg_kbps, qc_result.vbr.stddev_kbps);
        } else {
            printf("质量分析失败: %s\n", qc_result.error.c_str());
        }
    }

    // ============================================
    // 3. 输出报告
    // ============================================
    // 获取文件大小
    FILE* f = fopen(filePath, "rb");
    uint64_t file_size = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        file_size = ftell(f);
        fclose(f);
    }

    if (output_fmt == FMT_HTML) {
        std::string outputFile = baseName + "_report.html";
        LOGD("Output file: %s", outputFile.c_str());
        if (!write_mp4_html_report(parser, filePath, file_size, outputFile,
                                   has_qc ? &qc_result : nullptr,
                                   has_qc ? &qc_opt : nullptr)) {
            LOGD("Failed to write HTML report to: %s", outputFile.c_str());
            return 1;
        }
        LOGD("HTML report written to: %s", outputFile.c_str());
    } else if (output_fmt == FMT_MD) {
        std::string outputFile = baseName + "_report.md";
        LOGD("Output file: %s", outputFile.c_str());
        if (!write_mp4_md_report(parser, filePath, file_size, outputFile,
                                 has_qc ? &qc_result : nullptr,
                                 has_qc ? &qc_opt : nullptr)) {
            LOGD("Failed to write Markdown report to: %s", outputFile.c_str());
            return 1;
        }
        LOGD("Markdown report written to: %s", outputFile.c_str());
    } else {
        std::string outputFile = baseName + "_header.txt";
        LOGD("Output file: %s", outputFile.c_str());
        if (!parser.dump_header_to_txt(outputFile)) {
            LOGD("Failed to write header analysis to: %s", outputFile.c_str());
            return 1;
        }
    }

    // 打印 SXMD 信息（如果有）
    std::vector<SxmdInfo> sxmd_list;
    parser.collect_sxmd(sxmd_list);
    if (!sxmd_list.empty()) {
        printf("\n=== SXMD 私有元数据 ===\n");
        for (size_t i = 0; i < sxmd_list.size(); ++i) {
            const SxmdInfo& s = sxmd_list[i];
            printf("[%zu] Record Type: %u (%s)\n", i, s.record_type, s.record_type_str().c_str());
            printf("    Flags: 0x%02x", s.flags);
            if (s.flags & 0x01) printf(" [no_audio]");
            if (s.flags & 0x02) printf(" [timestamp_modified]");
            printf("\n");
            if (s.is_slow_motion()) {
                printf("    慢动作倍数: x%.1f\n", s.slow_motion_multiplier_x100 / 100.0);
            }
            if (s.is_timelapse()) {
                printf("    延时倍数: x%.1f\n", s.timelapse_multiplier_x100 / 100.0);
                printf("    延时间隔帧: %u\n", s.timelapse_interval_frame);
                printf("    目标 FPS: %u\n", s.target_fps);
            }
        }
    }

    LOGD("Analysis completed successfully!");
    return 0;
}
