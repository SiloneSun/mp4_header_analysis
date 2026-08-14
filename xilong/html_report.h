#ifndef __HTML_REPORT_H
#define __HTML_REPORT_H

#include "mp4_parser.h"
#include "mp4_analyzer.h"
#include <string>
#include <vector>

// 生成 MP4 综合分析报告 (Box 结构 + 质量分析)
bool write_mp4_html_report(const Mp4Parser& parser,
                           const std::string& source_filepath,
                           uint64_t file_size,
                           const std::string& output_path,
                           const sunxilong::AnalyzeResult* qc_result = nullptr,
                           const sunxilong::AnalyzeOptions* qc_opt = nullptr);

// 生成 Markdown 分析报告 (面向 AI agent 阅读理解)
bool write_mp4_md_report(const Mp4Parser& parser,
                         const std::string& source_filepath,
                         uint64_t file_size,
                         const std::string& output_path,
                         const sunxilong::AnalyzeResult* qc_result = nullptr,
                         const sunxilong::AnalyzeOptions* qc_opt = nullptr);

#endif
