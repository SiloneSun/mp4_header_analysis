#include <iostream>
#include <string>
#include "t_log.h"
#include "mp4_parser.h"

int main(int argc, char* argv[])
{
    if (argc < 2) {
        printf("Usage: %s <mp4_file>\n", argv[0]);
        return 1;
    }

    const char* filePath = argv[1];

    // 构造输出文件名: input_mp4_header.txt
    std::string inputPath(filePath);
    std::string outputFile;
    
    // 从输入文件路径中提取文件名（不含目录）
    size_t lastSlash = inputPath.find_last_of("/\\");
    std::string fileName = (lastSlash == std::string::npos) ? inputPath : inputPath.substr(lastSlash + 1);
    
    // 移除扩展名，追加 _header.txt
    size_t lastDot = fileName.find_last_of('.');
    std::string baseName = (lastDot == std::string::npos) ? fileName : fileName.substr(0, lastDot);
    outputFile = baseName + "_header.txt";

    LOGD("Input file: %s", filePath);
    LOGD("Output file: %s", outputFile.c_str());

    Mp4Parser parser;
    if (!parser.parse(filePath)) {
        LOGD("Failed to parse MP4 file: %s", filePath);
        return 1;
    }

    // 打印 tree 到控制台
    parser.print_tree();

    // 输出完整头信息到 txt 文件
    if (!parser.dump_header_to_txt(outputFile)) {
        LOGD("Failed to write header analysis to: %s", outputFile.c_str());
        return 1;
    }

    LOGD("Analysis completed successfully!");
    return 0;
}