#ifndef __MP4_PARSER_H
#define __MP4_PARSER_H

#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <memory>
#include <fstream>

// 最大 indentation 层级
#define MAX_DEPTH 32

// MP4 中 container box 的类型（这些 box 含有子 box）
// 注意: stco/co64 不是 container，它们是 leaf box
static const char* container_boxes[] = {
    "moov", "trak", "mdia", "minf", "stbl", "edts",
    "moof", "traf", "meco", "dinf", "mvex", "udta",
    "skip", "ipmc", "strk", "stsd", "stti", "sinf",
    "schi", "saio", "saiz", "pdin", "meta", "fiin",
    "paen", "mere", "maen", "leva", "subs", "trex",
    nullptr
};

static bool is_container(const char type[4]) {
    for (int i = 0; container_boxes[i] != nullptr; ++i) {
        if (type[0] == container_boxes[i][0] &&
            type[1] == container_boxes[i][1] &&
            type[2] == container_boxes[i][2] &&
            type[3] == container_boxes[i][3]) {
            return true;
        }
    }
    // stsd, minf 下的某些 box 也可能包含子 box
    // 只要不是 mdat 等纯数据 box，我们都尝试解析子 box
    return false;
}

// ============================================================
// MP4 Box 节点定义
// ============================================================
struct Mp4Box {
    char            type[5];       // 4-byte box type + NUL
    uint8_t         uuid[16];      // UUID (仅当 type == "uuid")
    uint64_t        box_size;      // 整个 box 的字节数 (包含 header)
    uint64_t        header_size;   // header 的字节数 (8 或 16 或 24)
    uint64_t        data_offset;   // data 在文件中的偏移 (box_start + header_size)
    uint64_t        data_size;     // data 的字节数 (box_size - header_size)
    uint64_t        file_offset;   // box 在文件中的起始偏移
    std::vector<uint8_t> raw_data; // 原始数据 (整个 box 的字节, 不含子 box)

    // 子 box
    std::vector<std::shared_ptr<Mp4Box>> children;

    // 等级（用于 tree 打印时的缩进）
    int level;

    // 是否为 container box
    bool is_container;
};

// ============================================================
// MP4 Parser 类
// ============================================================
class Mp4Parser {
public:
    Mp4Parser();
    ~Mp4Parser();

    // 打开并解析文件
    bool parse(const std::string& filepath);

    // 获取解析结果
    const std::vector<std::shared_ptr<Mp4Box>>& get_top_boxes() const { return m_top_boxes; }

    // 输出完整头信息到 txt 文件
    bool dump_header_to_txt(const std::string& output_filepath);

    // 打印 tree 结构到 stdout
    void print_tree();

private:
    std::string m_filepath;
    std::ifstream m_file;
    size_t m_file_size;

    // 顶层 box 列表
    std::vector<std::shared_ptr<Mp4Box>> m_top_boxes;

    // 递归解析 box
    std::shared_ptr<Mp4Box> parse_box(uint64_t offset, int level);

    // 读取 4 字节 big-endian uint32
    uint32_t read_uint32_be(uint64_t offset);

    // 读取 8 字节 big-endian uint64
    uint64_t read_uint64_be(uint64_t offset);

    // 读取 n 字节到 buffer
    bool read_bytes(uint64_t offset, size_t size, std::vector<uint8_t>& buffer);

    // 从 buffer 中读取 big-endian 值
    static uint32_t buf_read_uint32_be(const uint8_t* buf, size_t offset);
    static uint64_t buf_read_uint64_be(const uint8_t* buf, size_t offset);
    static uint16_t buf_read_uint16_be(const uint8_t* buf, size_t offset);
    static uint8_t  buf_read_uint8(const uint8_t* buf, size_t offset);

    // 写入带缩进的 tree 信息
    void dump_box_to_file(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);

    // 解析知名 box 的字段并输出可读信息
    void parse_box_fields(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);

    // 写缩进
    void write_indent(FILE* f, int depth);
    void write_indent_level(FILE* f, int depth);

    // 输出 hex dump
    void dump_hex(FILE* f, const uint8_t* data, size_t size, int depth);

    // 打印某一层 box 的 fields
    void dump_ftyp(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_mvhd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_tkhd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_mdhd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_hdlr(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_vmhd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_smhd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_stsd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_stts(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_stsc(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_stsz(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_stco(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_elst(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
    void dump_mdat(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth);
};

// 辅助：4 字节类型转为可读字符串（含转义非打印字符）
static std::string box_type_to_string(const char type[4]) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        if (type[i] >= 32 && type[i] < 127) {
            s += type[i];
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\x%02x", (uint8_t)type[i]);
            s += buf;
        }
    }
    return s;
}

static std::string indent_string(int level) {
    std::string s;
    for (int i = 0; i < level; ++i) s += "  ";
    return s;
}

// 四字符代码类型名
static std::string fourcc(const char type[4]) {
    return std::string(type, 4);
}

#endif