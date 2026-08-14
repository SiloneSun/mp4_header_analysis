#include "mp4_parser.h"
#include "t_log.h"

#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <functional>

// SeeVision 私有 UUID: 53 58 4d 44 2d 53 45 45 56 49 53 49 4f 4e 01 01
static const uint8_t SEEVISION_SXMD_UUID[16] = {
    0x53, 0x58, 0x4d, 0x44, 0x2d, 0x53, 0x45, 0x45,
    0x56, 0x49, 0x53, 0x49, 0x4f, 0x4e, 0x01, 0x01
};

// ============================================================
// 构造函数 / 析构函数
// ============================================================
Mp4Parser::Mp4Parser()
    : m_file_size(0)
{
}

Mp4Parser::~Mp4Parser() {
    if (m_file.is_open()) {
        m_file.close();
    }
}

// ============================================================
// 读取文件二进制数据
// ============================================================
bool Mp4Parser::read_bytes(uint64_t offset, size_t size, std::vector<uint8_t>& buffer) {
    if (!m_file.is_open()) return false;
    buffer.resize(size);
    m_file.seekg(offset, std::ios::beg);
    m_file.read(reinterpret_cast<char*>(buffer.data()), size);
    return (size_t)m_file.gcount() == size;
}

uint32_t Mp4Parser::read_uint32_be(uint64_t offset) {
    std::vector<uint8_t> buf;
    if (!read_bytes(offset, 4, buf)) return 0;
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  | ((uint32_t)buf[3]);
}

uint64_t Mp4Parser::read_uint64_be(uint64_t offset) {
    std::vector<uint8_t> buf;
    if (!read_bytes(offset, 8, buf)) return 0;
    return ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) |
           ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
           ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) |
           ((uint64_t)buf[6] << 8)  | ((uint64_t)buf[7]);
}

// ============================================================
// 从 buffer 读取 big-endian 值
// ============================================================
uint32_t Mp4Parser::buf_read_uint32_be(const uint8_t* buf, size_t offset) {
    return ((uint32_t)buf[offset] << 24) |
           ((uint32_t)buf[offset+1] << 16) |
           ((uint32_t)buf[offset+2] << 8) |
           ((uint32_t)buf[offset+3]);
}

uint64_t Mp4Parser::buf_read_uint64_be(const uint8_t* buf, size_t offset) {
    return ((uint64_t)buf[offset] << 56) |
           ((uint64_t)buf[offset+1] << 48) |
           ((uint64_t)buf[offset+2] << 40) |
           ((uint64_t)buf[offset+3] << 32) |
           ((uint64_t)buf[offset+4] << 24) |
           ((uint64_t)buf[offset+5] << 16) |
           ((uint64_t)buf[offset+6] << 8) |
           ((uint64_t)buf[offset+7]);
}

uint16_t Mp4Parser::buf_read_uint16_be(const uint8_t* buf, size_t offset) {
    return ((uint16_t)buf[offset] << 8) | ((uint16_t)buf[offset+1]);
}

uint8_t Mp4Parser::buf_read_uint8(const uint8_t* buf, size_t offset) {
    return buf[offset];
}

// ============================================================
// 递归解析 Box
// ============================================================
std::shared_ptr<Mp4Box> Mp4Parser::parse_box(uint64_t offset, int level) {
    auto box = std::make_shared<Mp4Box>();
    box->level = level;
    box->file_offset = offset;

    if (offset + 8 > m_file_size) {
        return nullptr;
    }

    // 读取 box size (4 bytes) + type (4 bytes)
    std::vector<uint8_t> header;
    if (!read_bytes(offset, 8, header)) {
        return nullptr;
    }

    uint32_t size32 = buf_read_uint32_be(header.data(), 0);
    memcpy(box->type, header.data() + 4, 4);
    box->type[4] = '\0';

    // 判断 header 大小
    if (size32 == 0) {
        // box 延伸到文件末尾
        box->box_size = m_file_size - offset;
        box->header_size = 8;
    } else if (size32 == 1) {
        // 64-bit extended size
        box->header_size = 16;
        if (offset + 16 > m_file_size) {
            return nullptr;
        }
        std::vector<uint8_t> ext_header;
        if (!read_bytes(offset, 16, ext_header)) {
            return nullptr;
        }
        box->box_size = buf_read_uint64_be(ext_header.data(), 8);
    } else {
        box->box_size = size32;
        box->header_size = 8;
    }

    // 处理 uuid 类型
    if (memcmp(box->type, "uuid", 4) == 0) {
        box->header_size += 16; // 额外 16 字节 UUID
        if (offset + box->header_size > m_file_size) {
            return nullptr;
        }
        std::vector<uint8_t> uuid_data;
        if (!read_bytes(offset + 8, 16, uuid_data)) {
            return nullptr;
        }
        memcpy(box->uuid, uuid_data.data(), 16);
    }

    box->data_offset = offset + box->header_size;
    box->data_size = box->box_size - box->header_size;
    box->is_container = is_container(box->type);

    // 检查 box 大小是否合法
    if (box->box_size == 0 || offset + box->box_size > m_file_size) {
        LOGD("WARNING: box %s at offset 0x%lx has invalid size %lu (file size %lu)",
             box->type, offset, (unsigned long)box->box_size, (unsigned long)m_file_size);
        return nullptr;
    }

    // 如果 data_size 合理，读取整个 box 数据
    // 注意：对于极大数据 box（如 mdat），我们不读入内存以避免 OOM
    // 仅读前 256 字节用于 hex dump 展示
    // 但对于非大数据类型的 box，全部读取
    bool is_large_data = (memcmp(box->type, "mdat", 4) == 0) ||
                         (memcmp(box->type, "free", 4) == 0) ||
                         (memcmp(box->type, "skip", 4) == 0);
    if (box->data_size > 0 && !is_large_data) {
        size_t read_size = (box->data_size > 1024 * 1024) ? (1024 * 1024) : (size_t)box->data_size;
        if (!read_bytes(box->data_offset, read_size, box->raw_data)) {
            LOGD("WARNING: failed to read data for box %s at 0x%lx (size %lu)",
                 box->type, (unsigned long)box->data_offset, (unsigned long)read_size);
        }
    } else if (is_large_data && box->data_size > 0) {
        // 对于大 box，只读前 256 字节用于展示
        size_t read_size = (box->data_size > 256) ? 256 : (size_t)box->data_size;
        if (!read_bytes(box->data_offset, read_size, box->raw_data)) {
            LOGD("WARNING: failed to read data for box %s at 0x%lx (size %lu)",
                 box->type, (unsigned long)box->data_offset, (unsigned long)read_size);
        }
    }

    // 如果是 container box 并且有足够的数据，递归解析子 box
    if (box->is_container && box->data_size >= 8) {
        uint64_t child_offset = box->data_offset;
        // 部分 FullBox 类型的 container box 需要跳过 version+flags 或更多
        // - stsd: 前 4 字节 version+flags, 接着 4 字节 entry_count
        // - meta: 前 4 字节 version+flags
        // - dref: 前 4 字节 version+flags, 接着 4 字节 entry_count
        if (memcmp(box->type, "stsd", 4) == 0 ||
            memcmp(box->type, "dref", 4) == 0) {
            child_offset += 8; // version(1)+flags(3)+entry_count(4)
        } else if (memcmp(box->type, "meta", 4) == 0) {
            child_offset += 4; // version(1)+flags(3)
        }
        uint64_t data_end = offset + box->box_size;
        while (child_offset + 8 <= data_end) {
            // 检查是否为有效的子 box（至少能读出 size 和 type）
            std::vector<uint8_t> child_header;
            if (!read_bytes(child_offset, 8, child_header)) {
                break;
            }
            uint32_t child_size32 = buf_read_uint32_be(child_header.data(), 0);
            uint64_t child_size;
            if (child_size32 == 0) {
                child_size = data_end - child_offset;
            } else if (child_size32 == 1) {
                child_size = 8; // 暂读不到，下面 parse_box 会处理
            } else {
                child_size = child_size32;
            }
            if (child_size < 8) {
                // 无效的 box 大小，跳过
                child_offset += 1;
                continue;
            }
            auto child = parse_box(child_offset, level + 1);
            if (child) {
                box->children.push_back(child);
                // 实际上 child->box_size 应该由 parse_box 填充
                child_offset += child->box_size;
            } else {
                child_offset += 1;
            }
            // 防止无限循环
            if (child_offset >= data_end) break;
        }
    }

    return box;
}

// ============================================================
// 主解析入口
// ============================================================
bool Mp4Parser::parse(const std::string& filepath) {
    m_filepath = filepath;

    // 打开文件
    m_file.open(filepath, std::ios::binary);
    if (!m_file.is_open()) {
        LOGD("Failed to open file: %s", filepath.c_str());
        return false;
    }

    // 获取文件大小
    m_file.seekg(0, std::ios::end);
    m_file_size = (size_t)m_file.tellg();
    m_file.seekg(0, std::ios::beg);

    LOGD("File size: %lu bytes (0x%lx)", (unsigned long)m_file_size, (unsigned long)m_file_size);

    // 解析顶层 box
    uint64_t offset = 0;
    while (offset + 8 <= m_file_size) {
        // 检查剩余数据是否足够构成一个最小 box
        auto box = parse_box(offset, 0);
        if (!box) {
            LOGD("Failed to parse box at offset 0x%lx", (unsigned long)offset);
            break;
        }
        m_top_boxes.push_back(box);
        offset += box->box_size;
        if (offset >= m_file_size) break;
    }

    LOGD("Parsed %zu top-level boxes", m_top_boxes.size());
    return true;
}

// ============================================================
// 缩进格式
// ============================================================
void Mp4Parser::write_indent(FILE* f, int depth) {
    for (int i = 0; i < depth; ++i) {
        fprintf(f, "    ");
    }
}

void Mp4Parser::write_indent_level(FILE* f, int depth) {
    for (int i = 0; i < depth; ++i) {
        fprintf(f, "|   ");
    }
}

// ============================================================
// Hex dump
// ============================================================
void Mp4Parser::dump_hex(FILE* f, const uint8_t* data, size_t size, int depth) {
    if (size == 0) {
        write_indent(f, depth);
        fprintf(f, "(empty)\n");
        return;
    }

    // 每行 16 字节
    size_t offset = 0;
    while (offset < size) {
        write_indent(f, depth);
        // 偏移
        fprintf(f, "%08zx  ", offset);

        // hex 部分
        for (size_t i = 0; i < 16; ++i) {
            if (offset + i < size) {
                fprintf(f, "%02x ", data[offset + i]);
            } else {
                fprintf(f, "   ");
            }
            if (i == 7) fprintf(f, " ");
        }

        // ASCII 部分
        fprintf(f, " |");
        for (size_t i = 0; i < 16 && offset + i < size; ++i) {
            uint8_t c = data[offset + i];
            if (c >= 32 && c < 127) {
                fprintf(f, "%c", c);
            } else {
                fprintf(f, ".");
            }
        }
        fprintf(f, "|\n");

        offset += 16;
    }
}

// ============================================================
// 知名 box 字段解析
// ============================================================

// ---------- ftyp ----------
void Mp4Parser::dump_ftyp(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();
    size_t sz = box->raw_data.size();

    write_indent(f, depth + 1);
    fprintf(f, "Major Brand: %c%c%c%c (0x%02x%02x%02x%02x)\n",
            d[0], d[1], d[2], d[3], d[0], d[1], d[2], d[3]);

    write_indent(f, depth + 1);
    uint32_t minor = buf_read_uint32_be(d, 4);
    fprintf(f, "Minor Version: %u (0x%08x)\n", minor, minor);

    size_t compat_count = (sz - 8) / 4;
    write_indent(f, depth + 1);
    fprintf(f, "Compatible Brands (%zu):\n", compat_count);
    for (size_t i = 0; i < compat_count; ++i) {
        write_indent(f, depth + 2);
        uint32_t brand = buf_read_uint32_be(d, 8 + i * 4);
        fprintf(f, "[%zu] %c%c%c%c (0x%08x)\n", i,
                (brand >> 24) & 0xFF, (brand >> 16) & 0xFF,
                (brand >> 8) & 0xFF, brand & 0xFF, brand);
    }
}

// ---------- mvhd ----------
void Mp4Parser::dump_mvhd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 100) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];

    write_indent(f, depth + 1);
    fprintf(f, "Version: %u\n", version);

    // flags (3 bytes)
    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    write_indent(f, depth + 1);
    fprintf(f, "Flags: 0x%06x\n", flags);

    if (version == 0) {
        // 32-bit times
        uint32_t creation_time = buf_read_uint32_be(d, 4);
        uint32_t modification_time = buf_read_uint32_be(d, 8);
        uint32_t timescale = buf_read_uint32_be(d, 12);
        uint32_t duration = buf_read_uint32_be(d, 16);

        write_indent(f, depth + 1);
        fprintf(f, "Creation Time (32-bit): %u (0x%08x)\n", creation_time, creation_time);
        write_indent(f, depth + 1);
        fprintf(f, "Modification Time (32-bit): %u (0x%08x)\n", modification_time, modification_time);
        write_indent(f, depth + 1);
        fprintf(f, "Timescale: %u (0x%08x)\n", timescale, timescale);
        write_indent(f, depth + 1);
        fprintf(f, "Duration: %u (0x%08x)\n", duration, duration);

        // rate, volume 等字段在偏移 20
        int32_t rate = (int16_t)buf_read_uint16_be(d, 20) + (int16_t)buf_read_uint16_be(d, 22) / 65536.0 * 100;
        write_indent(f, depth + 1);
        uint16_t rate_int = buf_read_uint16_be(d, 20);
        uint16_t rate_frac = buf_read_uint16_be(d, 22);
        fprintf(f, "Rate: %u.%u (0x%04x%04x)\n", rate_int, rate_frac, rate_int, rate_frac);

        int16_t volume = (int16_t)buf_read_uint16_be(d, 24);
        write_indent(f, depth + 1);
        fprintf(f, "Volume: %d (0x%04x)\n", volume, (uint16_t)volume);
    } else {
        // 64-bit times
        uint64_t creation_time = buf_read_uint64_be(d, 4);
        uint64_t modification_time = buf_read_uint64_be(d, 12);
        uint32_t timescale = buf_read_uint32_be(d, 20);
        uint64_t duration = buf_read_uint64_be(d, 24);

        write_indent(f, depth + 1);
        fprintf(f, "Creation Time (64-bit): %lu (0x%016lx)\n",
                (unsigned long)creation_time, (unsigned long)creation_time);
        write_indent(f, depth + 1);
        fprintf(f, "Modification Time (64-bit): %lu (0x%016lx)\n",
                (unsigned long)modification_time, (unsigned long)modification_time);
        write_indent(f, depth + 1);
        fprintf(f, "Timescale: %u (0x%08x)\n", timescale, timescale);
        write_indent(f, depth + 1);
        fprintf(f, "Duration (64-bit): %lu (0x%016lx)\n",
                (unsigned long)duration, (unsigned long)duration);

        uint16_t rate_int = buf_read_uint16_be(d, 36);
        uint16_t rate_frac = buf_read_uint16_be(d, 38);
        write_indent(f, depth + 1);
        fprintf(f, "Rate: %u.%u (0x%04x%04x)\n", rate_int, rate_frac, rate_int, rate_frac);

        int16_t volume = (int16_t)buf_read_uint16_be(d, 40);
        write_indent(f, depth + 1);
        fprintf(f, "Volume: %d (0x%04x)\n", volume, (uint16_t)volume);
    }

    // Matrix (36 bytes at offset 32 or 48)
    size_t matrix_off = (version == 0) ? 32 : 48;
    write_indent(f, depth + 1);
    fprintf(f, "Matrix (9 fixed-point 16.16 values):\n");
    for (int i = 0; i < 9; ++i) {
        write_indent(f, depth + 2);
        uint32_t val = buf_read_uint32_be(d, matrix_off + i * 4);
        fprintf(f, "[%d] %u (0x%08x)\n", i, val, val);
    }

    // Pre-defined times, etc.
    size_t predef_off = (version == 0) ? 68 : 84;
    write_indent(f, depth + 1);
    fprintf(f, "Pre-defined (6 x 32-bit zeros):\n");
    for (int i = 0; i < 6; ++i) {
        write_indent(f, depth + 2);
        uint32_t val = buf_read_uint32_be(d, predef_off + i * 4);
        fprintf(f, "[%d] %u (0x%08x)\n", i, val, val);
    }

    // Next track id
    size_t ntid_off = (version == 0) ? 92 : 108;
    uint32_t next_track_id = buf_read_uint32_be(d, ntid_off);
    write_indent(f, depth + 1);
    fprintf(f, "Next Track ID: %u (0x%08x)\n", next_track_id, next_track_id);
}

// ---------- tkhd ----------
void Mp4Parser::dump_tkhd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 84) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];
    write_indent(f, depth + 1);
    fprintf(f, "Version: %u\n", version);

    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    write_indent(f, depth + 1);
    fprintf(f, "Flags: 0x%06x", flags);
    if (flags & 0x01) fprintf(f, " (Track Enabled)");
    if (flags & 0x02) fprintf(f, " (Track In Movie)");
    if (flags & 0x04) fprintf(f, " (Track In Preview)");
    fprintf(f, "\n");

    size_t off = 4;
    if (version == 0) {
        uint32_t creation_time = buf_read_uint32_be(d, off); off += 4;
        uint32_t modification_time = buf_read_uint32_be(d, off); off += 4;
        uint32_t track_id = buf_read_uint32_be(d, off); off += 4;
        uint32_t reserved = buf_read_uint32_be(d, off); off += 4;
        uint32_t duration = buf_read_uint32_be(d, off); off += 4;

        write_indent(f, depth + 1);
        fprintf(f, "Creation Time (32-bit): %u (0x%08x)\n", creation_time, creation_time);
        write_indent(f, depth + 1);
        fprintf(f, "Modification Time (32-bit): %u (0x%08x)\n", modification_time, modification_time);
        write_indent(f, depth + 1);
        fprintf(f, "Track ID: %u (0x%08x)\n", track_id, track_id);
        write_indent(f, depth + 1);
        fprintf(f, "Reserved: %u (0x%08x)\n", reserved, reserved);
        write_indent(f, depth + 1);
        fprintf(f, "Duration (32-bit): %u (0x%08x)\n", duration, duration);
    } else {
        uint64_t creation_time = buf_read_uint64_be(d, off); off += 8;
        uint64_t modification_time = buf_read_uint64_be(d, off); off += 8;
        uint32_t track_id = buf_read_uint32_be(d, off); off += 4;
        uint32_t reserved = buf_read_uint32_be(d, off); off += 4;
        uint64_t duration = buf_read_uint64_be(d, off); off += 8;

        write_indent(f, depth + 1);
        fprintf(f, "Creation Time (64-bit): %lu (0x%016lx)\n",
                (unsigned long)creation_time, (unsigned long)creation_time);
        write_indent(f, depth + 1);
        fprintf(f, "Modification Time (64-bit): %lu (0x%016lx)\n",
                (unsigned long)modification_time, (unsigned long)modification_time);
        write_indent(f, depth + 1);
        fprintf(f, "Track ID: %u (0x%08x)\n", track_id, track_id);
        write_indent(f, depth + 1);
        fprintf(f, "Reserved: %u (0x%08x)\n", reserved, reserved);
        write_indent(f, depth + 1);
        fprintf(f, "Duration (64-bit): %lu (0x%016lx)\n",
                (unsigned long)duration, (unsigned long)duration);
    }

    // Reserved (8 bytes)
    off += 8;
    write_indent(f, depth + 1);
    fprintf(f, "Layer: %d (0x%04x)\n", (int16_t)buf_read_uint16_be(d, off), buf_read_uint16_be(d, off)); off += 2;
    write_indent(f, depth + 1);
    fprintf(f, "Alternate Group: %d (0x%04x)\n", (int16_t)buf_read_uint16_be(d, off), buf_read_uint16_be(d, off)); off += 2;
    write_indent(f, depth + 1);
    fprintf(f, "Volume: %d (0x%04x)\n", (int16_t)buf_read_uint16_be(d, off), buf_read_uint16_be(d, off)); off += 2;
    off += 2; // reserved

    // Matrix (36 bytes)
    write_indent(f, depth + 1);
    fprintf(f, "Matrix (9 fixed-point 16.16 values):\n");
    for (int i = 0; i < 9; ++i) {
        write_indent(f, depth + 2);
        uint32_t val = buf_read_uint32_be(d, off); off += 4;
        fprintf(f, "[%d] %u (0x%08x)\n", i, val, val);
    }

    // Width and Height
    uint32_t width = buf_read_uint32_be(d, off); off += 4;
    uint32_t height = buf_read_uint32_be(d, off); off += 4;
    write_indent(f, depth + 1);
    fprintf(f, "Width (fixed-point 16.16): %d.%d (%u)\n",
            width >> 16, (width & 0xFFFF) * 100 / 65536, width);
    write_indent(f, depth + 1);
    fprintf(f, "Height (fixed-point 16.16): %d.%d (%u)\n",
            height >> 16, (height & 0xFFFF) * 100 / 65536, height);
}

// ---------- mdhd ----------
void Mp4Parser::dump_mdhd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 24) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];
    write_indent(f, depth + 1);
    fprintf(f, "Version: %u\n", version);

    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    write_indent(f, depth + 1);
    fprintf(f, "Flags: 0x%06x\n", flags);

    if (version == 0) {
        uint32_t creation_time = buf_read_uint32_be(d, 4);
        uint32_t modification_time = buf_read_uint32_be(d, 8);
        uint32_t timescale = buf_read_uint32_be(d, 12);
        uint32_t duration = buf_read_uint32_be(d, 16);

        write_indent(f, depth + 1);
        fprintf(f, "Creation Time (32-bit): %u (0x%08x)\n", creation_time, creation_time);
        write_indent(f, depth + 1);
        fprintf(f, "Modification Time (32-bit): %u (0x%08x)\n", modification_time, modification_time);
        write_indent(f, depth + 1);
        fprintf(f, "Timescale: %u (0x%08x)\n", timescale, timescale);
        write_indent(f, depth + 1);
        fprintf(f, "Duration (32-bit): %u (0x%08x)\n", duration, duration);

        // Pad bit (1 bit), language (15 bits)
        uint16_t lang_pad = buf_read_uint16_be(d, 20);
        uint8_t lang[4];
        lang[0] = ((lang_pad >> 10) & 0x1F) + 0x60;
        lang[1] = ((lang_pad >> 5) & 0x1F) + 0x60;
        lang[2] = (lang_pad & 0x1F) + 0x60;
        lang[3] = '\0';
        write_indent(f, depth + 1);
        fprintf(f, "Language: %s (0x%04x)\n", lang, lang_pad);

        uint16_t quality = buf_read_uint16_be(d, 22);
        write_indent(f, depth + 1);
        fprintf(f, "Quality: %u (0x%04x)\n", quality, quality);
    } else {
        uint64_t creation_time = buf_read_uint64_be(d, 4);
        uint64_t modification_time = buf_read_uint64_be(d, 12);
        uint32_t timescale = buf_read_uint32_be(d, 20);
        uint64_t duration = buf_read_uint64_be(d, 24);

        write_indent(f, depth + 1);
        fprintf(f, "Creation Time (64-bit): %lu (0x%016lx)\n",
                (unsigned long)creation_time, (unsigned long)creation_time);
        write_indent(f, depth + 1);
        fprintf(f, "Modification Time (64-bit): %lu (0x%016lx)\n",
                (unsigned long)modification_time, (unsigned long)modification_time);
        write_indent(f, depth + 1);
        fprintf(f, "Timescale: %u (0x%08x)\n", timescale, timescale);
        write_indent(f, depth + 1);
        fprintf(f, "Duration (64-bit): %lu (0x%016lx)\n",
                (unsigned long)duration, (unsigned long)duration);

        uint16_t lang_pad = buf_read_uint16_be(d, 32);
        uint8_t lang[4];
        lang[0] = ((lang_pad >> 10) & 0x1F) + 0x60;
        lang[1] = ((lang_pad >> 5) & 0x1F) + 0x60;
        lang[2] = (lang_pad & 0x1F) + 0x60;
        lang[3] = '\0';
        write_indent(f, depth + 1);
        fprintf(f, "Language: %s (0x%04x)\n", lang, lang_pad);

        uint16_t quality = buf_read_uint16_be(d, 34);
        write_indent(f, depth + 1);
        fprintf(f, "Quality: %u (0x%04x)\n", quality, quality);
    }
}

// ---------- hdlr ----------
void Mp4Parser::dump_hdlr(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 24) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];
    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];

    write_indent(f, depth + 1);
    fprintf(f, "Version: %u, Flags: 0x%06x\n", version, flags);

    uint32_t pre_defined = buf_read_uint32_be(d, 4);
    write_indent(f, depth + 1);
    fprintf(f, "Pre-defined: %u (0x%08x)\n", pre_defined, pre_defined);

    uint32_t handler_type = buf_read_uint32_be(d, 8);
    char handler_str[5];
    handler_str[0] = (handler_type >> 24) & 0xFF;
    handler_str[1] = (handler_type >> 16) & 0xFF;
    handler_str[2] = (handler_type >> 8) & 0xFF;
    handler_str[3] = handler_type & 0xFF;
    handler_str[4] = '\0';
    write_indent(f, depth + 1);
    fprintf(f, "Handler Type: %s (0x%08x)\n", handler_str, handler_type);

    // Reserved (12 bytes)
    write_indent(f, depth + 1);
    fprintf(f, "Reserved (3 x 32-bit zeros):\n");
    for (int i = 0; i < 3; ++i) {
        write_indent(f, depth + 2);
        uint32_t val = buf_read_uint32_be(d, 12 + i * 4);
        fprintf(f, "[%d] %u (0x%08x)\n", i, val, val);
    }

    // Name (null-terminated string)
    if (box->raw_data.size() > 24) {
        write_indent(f, depth + 1);
        fprintf(f, "Name: %s\n", (const char*)(d + 24));
    }
}

// ---------- vmhd ----------
void Mp4Parser::dump_vmhd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 12) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];
    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];

    write_indent(f, depth + 1);
    fprintf(f, "Version: %u, Flags: 0x%06x\n", version, flags);

    uint16_t graphics_mode = buf_read_uint16_be(d, 4);
    write_indent(f, depth + 1);
    fprintf(f, "Graphics Mode: %u (0x%04x)\n", graphics_mode, graphics_mode);

    write_indent(f, depth + 1);
    fprintf(f, "Op Color (3 x uint16):\n");
    for (int i = 0; i < 3; ++i) {
        write_indent(f, depth + 2);
        uint16_t val = buf_read_uint16_be(d, 6 + i * 2);
        fprintf(f, "[%d] %u (0x%04x)\n", i, val, val);
    }
}

// ---------- smhd ----------
void Mp4Parser::dump_smhd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];
    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];

    write_indent(f, depth + 1);
    fprintf(f, "Version: %u, Flags: 0x%06x\n", version, flags);

    uint16_t balance = buf_read_uint16_be(d, 4);
    write_indent(f, depth + 1);
    fprintf(f, "Balance (fixed-point 8.8): %d (0x%04x)\n", balance, balance);

    uint16_t reserved = buf_read_uint16_be(d, 6);
    write_indent(f, depth + 1);
    fprintf(f, "Reserved: %u (0x%04x)\n", reserved, reserved);
}

// ---------- stsd (Sample Description Box) ----------
void Mp4Parser::dump_stsd(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];
    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];

    write_indent(f, depth + 1);
    fprintf(f, "Version: %u, Flags: 0x%06x\n", version, flags);

    uint32_t entry_count = buf_read_uint32_be(d, 4);
    write_indent(f, depth + 1);
    fprintf(f, "Entry Count: %u (0x%08x)\n", entry_count, entry_count);

    if (box->children.size() > 0) {
        write_indent(f, depth + 1);
        fprintf(f, "Sample Descriptions (%zu):\n", box->children.size());
        for (size_t i = 0; i < box->children.size(); ++i) {
            auto& child = box->children[i];
            if (child->raw_data.size() < 8) continue;

            write_indent(f, depth + 2);
            fprintf(f, "[%zu] Type: %s, Data Size: %lu bytes\n",
                    i, child->type, (unsigned long)child->data_size);

            // stsd 子 box 的前 8 字节是 sample entry 的公共部分
            const uint8_t* cd = child->raw_data.data();
            // 对于视频或音频 codec，打印前几个关键字段

            // 解析 sample entry 部分
            if (child->raw_data.size() >= 8) {
                uint16_t data_ref_index = buf_read_uint16_be(cd, 6);
                write_indent(f, depth + 3);
                fprintf(f, "Data Reference Index: %u (0x%04x)\n", data_ref_index, data_ref_index);
            }

            // 如果是视频 (avc1, hvc1, etc.)
            if (memcmp(child->type, "avc1", 4) == 0 ||
                memcmp(child->type, "hvc1", 4) == 0 ||
                memcmp(child->type, "hev1", 4) == 0 ||
                memcmp(child->type, "mp4v", 4) == 0) {
                if (child->raw_data.size() >= 78) {
                    uint16_t width = buf_read_uint16_be(cd, 24);
                    uint16_t height = buf_read_uint16_be(cd, 26);
                    uint32_t horiz_res = buf_read_uint32_be(cd, 36);
                    uint32_t vert_res = buf_read_uint32_be(cd, 40);
                    uint16_t frame_count = buf_read_uint16_be(cd, 46);
                    uint16_t depth = buf_read_uint16_be(cd, 74);

                    write_indent(f, depth + 3);
                    fprintf(f, "Width: %u\n", width);
                    write_indent(f, depth + 3);
                    fprintf(f, "Height: %u\n", height);
                    write_indent(f, depth + 3);
                    fprintf(f, "Horizontal Resolution: %u.%03u\n",
                            horiz_res >> 16, (horiz_res & 0xFFFF) * 1000 / 65536);
                    write_indent(f, depth + 3);
                    fprintf(f, "Vertical Resolution: %u.%03u\n",
                            vert_res >> 16, (vert_res & 0xFFFF) * 1000 / 65536);
                    write_indent(f, depth + 3);
                    fprintf(f, "Frame Count: %u\n", frame_count);
                    write_indent(f, depth + 3);
                    fprintf(f, "Depth: %u\n", depth);
                }
            }
            // 如果是音频 (mp4a, opus, etc.)
            if (memcmp(child->type, "mp4a", 4) == 0 ||
                memcmp(child->type, "opus", 4) == 0 ||
                memcmp(child->type, "twos", 4) == 0 ||
                memcmp(child->type, "sowt", 4) == 0) {
                if (child->raw_data.size() >= 28) {
                    uint16_t channels = buf_read_uint16_be(cd, 16);
                    uint16_t sample_size = buf_read_uint16_be(cd, 18);
                    uint32_t sample_rate = buf_read_uint32_be(cd, 24);

                    write_indent(f, depth + 3);
                    fprintf(f, "Channels: %u\n", channels);
                    write_indent(f, depth + 3);
                    fprintf(f, "Sample Size: %u bits\n", sample_size);
                    write_indent(f, depth + 3);
                    fprintf(f, "Sample Rate: %u Hz\n", sample_rate >> 16);
                }
            }
        }
    }
}

// ---------- stts ----------
void Mp4Parser::dump_stts(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];
    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];

    write_indent(f, depth + 1);
    fprintf(f, "Version: %u, Flags: 0x%06x\n", version, flags);

    uint32_t entry_count = buf_read_uint32_be(d, 4);
    write_indent(f, depth + 1);
    fprintf(f, "Entry Count: %u (0x%08x)\n", entry_count, entry_count);

    // 只显示前 20 个条目，避免输出过大
    uint32_t max_show = (entry_count > 20) ? 20 : entry_count;
    for (uint32_t i = 0; i < max_show; ++i) {
        if (8 + i * 8 + 8 > box->raw_data.size()) break;
        uint32_t sample_count = buf_read_uint32_be(d, 8 + i * 8);
        uint32_t sample_delta = buf_read_uint32_be(d, 12 + i * 8);
        write_indent(f, depth + 2);
        fprintf(f, "[%u] Sample Count: %u, Sample Delta: %u\n", i, sample_count, sample_delta);
    }
    if (entry_count > 20) {
        write_indent(f, depth + 2);
        fprintf(f, "... (%u more entries)\n", entry_count - 20);
    }
}

// ---------- stsc ----------
void Mp4Parser::dump_stsc(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];
    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];

    write_indent(f, depth + 1);
    fprintf(f, "Version: %u, Flags: 0x%06x\n", version, flags);

    uint32_t entry_count = buf_read_uint32_be(d, 4);
    write_indent(f, depth + 1);
    fprintf(f, "Entry Count: %u (0x%08x)\n", entry_count, entry_count);

    uint32_t max_show = (entry_count > 20) ? 20 : entry_count;
    for (uint32_t i = 0; i < max_show; ++i) {
        if (8 + i * 12 + 12 > box->raw_data.size()) break;
        uint32_t first_chunk = buf_read_uint32_be(d, 8 + i * 12);
        uint32_t samples_per_chunk = buf_read_uint32_be(d, 12 + i * 12);
        uint32_t sample_desc_index = buf_read_uint32_be(d, 16 + i * 12);
        write_indent(f, depth + 2);
        fprintf(f, "[%u] First Chunk: %u, Samples/Chunk: %u, Sample Desc Index: %u\n",
                i, first_chunk, samples_per_chunk, sample_desc_index);
    }
    if (entry_count > 20) {
        write_indent(f, depth + 2);
        fprintf(f, "... (%u more entries)\n", entry_count - 20);
    }
}

// ---------- stsz ----------
void Mp4Parser::dump_stsz(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 12) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];
    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];

    write_indent(f, depth + 1);
    fprintf(f, "Version: %u, Flags: 0x%06x\n", version, flags);

    uint32_t sample_size = buf_read_uint32_be(d, 4);
    uint32_t sample_count = buf_read_uint32_be(d, 8);

    write_indent(f, depth + 1);
    fprintf(f, "Sample Size (constant): %u (0x%08x)\n", sample_size, sample_size);
    write_indent(f, depth + 1);
    fprintf(f, "Sample Count: %u (0x%08x)\n", sample_count, sample_count);

    if (sample_size == 0) {
        // Variable sample sizes
        uint32_t max_show = (sample_count > 20) ? 20 : sample_count;
        for (uint32_t i = 0; i < max_show; ++i) {
            if (12 + i * 4 + 4 > box->raw_data.size()) break;
            uint32_t sz = buf_read_uint32_be(d, 12 + i * 4);
            write_indent(f, depth + 2);
            fprintf(f, "[%u] Size: %u bytes\n", i, sz);
        }
        if (sample_count > 20) {
            write_indent(f, depth + 2);
            fprintf(f, "... (%u more entries)\n", sample_count - 20);
        }
    }
}

// ---------- stco ----------
void Mp4Parser::dump_stco(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];
    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];

    write_indent(f, depth + 1);
    fprintf(f, "Version: %u, Flags: 0x%06x\n", version, flags);

    uint32_t entry_count = buf_read_uint32_be(d, 4);
    write_indent(f, depth + 1);
    fprintf(f, "Entry Count: %u (0x%08x)\n", entry_count, entry_count);

    // Check if it's co64 (64-bit) or stco (32-bit)
    bool is_co64 = (memcmp(box->type, "co64", 4) == 0);

    uint32_t max_show = (entry_count > 20) ? 20 : entry_count;
    for (uint32_t i = 0; i < max_show; ++i) {
        write_indent(f, depth + 2);
        if (is_co64) {
            if (8 + i * 8 + 8 > box->raw_data.size()) break;
            uint64_t chunk_offset = buf_read_uint64_be(d, 8 + i * 8);
            fprintf(f, "[%u] Chunk Offset: 0x%016lx (%lu)\n",
                    i, (unsigned long)chunk_offset, (unsigned long)chunk_offset);
        } else {
            if (8 + i * 4 + 4 > box->raw_data.size()) break;
            uint32_t chunk_offset = buf_read_uint32_be(d, 8 + i * 4);
            fprintf(f, "[%u] Chunk Offset: 0x%08x (%u)\n", i, chunk_offset, chunk_offset);
        }
    }
    if (entry_count > 20) {
        write_indent(f, depth + 2);
        fprintf(f, "... (%u more entries)\n", entry_count - 20);
    }
}

// ---------- elst ----------
void Mp4Parser::dump_elst(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();

    uint8_t version = d[0];
    write_indent(f, depth + 1);
    fprintf(f, "Version: %u\n", version);

    uint32_t entry_count = buf_read_uint32_be(d, 4);
    write_indent(f, depth + 1);
    fprintf(f, "Entry Count: %u (0x%08x)\n", entry_count, entry_count);

    for (uint32_t i = 0; i < entry_count; ++i) {
        write_indent(f, depth + 2);
        if (version == 0) {
            if (8 + i * 12 + 12 > box->raw_data.size()) break;
            uint32_t seg_duration = buf_read_uint32_be(d, 8 + i * 12);
            int32_t media_time = (int32_t)buf_read_uint32_be(d, 12 + i * 12);
            int16_t media_rate_int = (int16_t)buf_read_uint16_be(d, 16 + i * 12);
            int16_t media_rate_frac = (int16_t)buf_read_uint16_be(d, 18 + i * 12);
            fprintf(f, "[%u] Segment Duration: %u, Media Time: %d, Media Rate: %d.%d\n",
                    i, seg_duration, media_time, media_rate_int, media_rate_frac);
        } else {
            if (8 + i * 20 + 20 > box->raw_data.size()) break;
            uint64_t seg_duration = buf_read_uint64_be(d, 8 + i * 20);
            int64_t media_time = (int64_t)buf_read_uint64_be(d, 16 + i * 20);
            int16_t media_rate_int = (int16_t)buf_read_uint16_be(d, 24 + i * 20);
            int16_t media_rate_frac = (int16_t)buf_read_uint16_be(d, 26 + i * 20);
            fprintf(f, "[%u] Segment Duration: %lu, Media Time: %ld, Media Rate: %d.%d\n",
                    i, (unsigned long)seg_duration, (long)media_time,
                    media_rate_int, media_rate_frac);
        }
    }
}

// ---------- mdat ----------
void Mp4Parser::dump_mdat(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    write_indent(f, depth + 1);
    fprintf(f, "Media Data Size: %lu bytes (0x%lx)\n",
            (unsigned long)box->data_size, (unsigned long)box->data_size);

    // 只显示前 64 字节
    if (box->raw_data.size() > 0) {
        size_t show_size = (box->raw_data.size() > 64) ? 64 : box->raw_data.size();
        write_indent(f, depth + 1);
        fprintf(f, "First %zu bytes of media data:\n", show_size);
        dump_hex(f, box->raw_data.data(), show_size, depth + 2);
        if (box->raw_data.size() > 64) {
            write_indent(f, depth + 1);
            fprintf(f, "... (truncated, total data size %lu bytes)\n",
                    (unsigned long)box->data_size);
        }
    }
}

// ============================================================
// 自动识别并解析知名 box 的字段
// ============================================================
void Mp4Parser::parse_box_fields(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (memcmp(box->type, "ftyp", 4) == 0) {
        dump_ftyp(f, box, depth);
    } else if (memcmp(box->type, "mvhd", 4) == 0) {
        dump_mvhd(f, box, depth);
    } else if (memcmp(box->type, "tkhd", 4) == 0) {
        dump_tkhd(f, box, depth);
    } else if (memcmp(box->type, "mdhd", 4) == 0) {
        dump_mdhd(f, box, depth);
    } else if (memcmp(box->type, "hdlr", 4) == 0) {
        dump_hdlr(f, box, depth);
    } else if (memcmp(box->type, "vmhd", 4) == 0) {
        dump_vmhd(f, box, depth);
    } else if (memcmp(box->type, "smhd", 4) == 0) {
        dump_smhd(f, box, depth);
    } else if (memcmp(box->type, "stsd", 4) == 0) {
        dump_stsd(f, box, depth);
    } else if (memcmp(box->type, "stts", 4) == 0) {
        dump_stts(f, box, depth);
    } else if (memcmp(box->type, "stsc", 4) == 0) {
        dump_stsc(f, box, depth);
    } else if (memcmp(box->type, "stsz", 4) == 0) {
        dump_stsz(f, box, depth);
    } else if (memcmp(box->type, "stco", 4) == 0) {
        dump_stco(f, box, depth);
    } else if (memcmp(box->type, "co64", 4) == 0) {
        dump_stco(f, box, depth);
    } else if (memcmp(box->type, "elst", 4) == 0) {
        dump_elst(f, box, depth);
    } else if (memcmp(box->type, "uuid", 4) == 0) {
        dump_uuid(f, box, depth);
    } else if (memcmp(box->type, "mdat", 4) == 0) {
        dump_mdat(f, box, depth);
    }
}

// ---------- uuid (含 SXMD 检测) ----------
void Mp4Parser::dump_uuid(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    write_indent(f, depth + 1);
    fprintf(f, "UUID: ");
    for (int i = 0; i < 16; ++i) {
        fprintf(f, "%02x", box->uuid[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) fprintf(f, "-");
    }
    fprintf(f, "\n");

    // 检查是否为 SeeVision SXMD
    SxmdInfo sxmd;
    if (try_parse_sxmd(box, sxmd)) {
        write_indent(f, depth + 1);
        fprintf(f, "=== SeeVision SXMD (私有元数据) ===\n");
        write_indent(f, depth + 1);
        fprintf(f, "Magic: SXMD\n");
        write_indent(f, depth + 1);
        fprintf(f, "Version: %u\n", sxmd.version);
        write_indent(f, depth + 1);
        fprintf(f, "Record Type: %u (%s)\n", sxmd.record_type, sxmd.record_type_str().c_str());
        write_indent(f, depth + 1);
        fprintf(f, "Flags: 0x%02x", sxmd.flags);
        if (sxmd.flags & 0x01) fprintf(f, " [no_audio]");
        if (sxmd.flags & 0x02) fprintf(f, " [timestamp_modified]");
        fprintf(f, "\n");
        write_indent(f, depth + 1);
        fprintf(f, "Slow Motion Multiplier: %u (x%.2f)\n",
                sxmd.slow_motion_multiplier_x100,
                sxmd.slow_motion_multiplier_x100 / 100.0);
        write_indent(f, depth + 1);
        fprintf(f, "Timelapse Multiplier: %u (x%.2f)\n",
                sxmd.timelapse_multiplier_x100,
                sxmd.timelapse_multiplier_x100 / 100.0);
        write_indent(f, depth + 1);
        fprintf(f, "Timelapse Interval Frame: %u\n", sxmd.timelapse_interval_frame);
        write_indent(f, depth + 1);
        fprintf(f, "Target FPS: %u\n", sxmd.target_fps);

        // 业务含义
        write_indent(f, depth + 1);
        if (sxmd.is_slow_motion()) {
            fprintf(f, "\u2192 \u8fd9\u662f\u4e00\u4e2a\u6162\u52a8\u4f5c\u5f55\u50cf (x%.1f \u6162\u653e)\n",
                    sxmd.slow_motion_multiplier_x100 / 100.0);
        } else if (sxmd.record_type == 2) {
            fprintf(f, "\u2192 \u8fd9\u662f\u4e00\u4e2a\u9759\u6b62\u5ef6\u65f6\u5f55\u50cf (x%.1f \u52a0\u901f)\n",
                    sxmd.timelapse_multiplier_x100 / 100.0);
        } else if (sxmd.record_type == 3) {
            fprintf(f, "\u2192 \u8fd9\u662f\u4e00\u4e2a\u8fd0\u52a8\u5ef6\u65f6\u5f55\u50cf (x%.1f \u52a0\u901f)\n",
                    sxmd.timelapse_multiplier_x100 / 100.0);
        } else if (sxmd.record_type == 4) {
            fprintf(f, "\u2192 \u8fd9\u662f\u4e00\u4e2a\u8f68\u8ff9\u5ef6\u65f6\u5f55\u50cf (x%.1f \u52a0\u901f)\n",
                    sxmd.timelapse_multiplier_x100 / 100.0);
        }
    } else {
        write_indent(f, depth + 1);
        fprintf(f, "(\u672a\u77e5 UUID \u7c7b\u578b)\n");
    }
}

bool Mp4Parser::try_parse_sxmd(const std::shared_ptr<Mp4Box>& box, SxmdInfo& info) {
    // 检查是否为 uuid box
    if (memcmp(box->type, "uuid", 4) != 0) return false;

    // 检查 UUID 是否匹配 SeeVision
    if (memcmp(box->uuid, SEEVISION_SXMD_UUID, 16) != 0) return false;

    // 检查 payload 大小: magic(4) + version(1) + record_type(1) + flags(1) + reserved(1)
    //   + slow_motion(4) + timelapse(4) + interval(4) + target_fps(4) = 24 bytes
    if (box->raw_data.size() < 24) return false;

    const uint8_t* d = box->raw_data.data();

    // 检查 magic
    if (d[0] != 'S' || d[1] != 'X' || d[2] != 'M' || d[3] != 'D') return false;

    info.valid = true;
    info.version = d[4];

    // 只支持 version 1
    if (info.version != 1) return false;

    info.record_type = d[5];
    info.flags = d[6];
    // d[7] = reserved0

    info.slow_motion_multiplier_x100 = buf_read_uint32_be(d, 8);
    info.timelapse_multiplier_x100 = buf_read_uint32_be(d, 12);
    info.timelapse_interval_frame = buf_read_uint32_be(d, 16);
    info.target_fps = buf_read_uint32_be(d, 20);

    // 基本合法性检查
    if (info.record_type > 4) return false;
    if (info.slow_motion_multiplier_x100 < 100) return false;
    if (info.timelapse_multiplier_x100 < 100) return false;

    return true;
}

// ============================================================
// 收集 SXMD 信息
// ============================================================
void Mp4Parser::collect_sxmd(std::vector<SxmdInfo>& result) const {
    for (const auto& box : m_top_boxes) {
        collect_sxmd_recursive(box, result);
    }
}

void Mp4Parser::collect_sxmd_recursive(const std::shared_ptr<Mp4Box>& box, std::vector<SxmdInfo>& result) {
    if (!box) return;
    SxmdInfo info;
    if (try_parse_sxmd(box, info)) {
        result.push_back(info);
    }
    for (const auto& child : box->children) {
        collect_sxmd_recursive(child, result);
    }
}

// ============================================================
// 递归输出一个 box 到文件
// ============================================================
void Mp4Parser::dump_box_to_file(FILE* f, const std::shared_ptr<Mp4Box>& box, int depth) {
    if (!box) return;

    // 缩进 + tree 结构标识
    write_indent_level(f, depth);
    if (depth > 0) {
        fprintf(f, "+-- ");
    }

    bool is_uuid = (memcmp(box->type, "uuid", 4) == 0);
    if (is_uuid) {
        fprintf(f, "Box: uuid [UUID: ");
        for (int i = 0; i < 16; ++i) {
            fprintf(f, "%02x", box->uuid[i]);
            if (i == 3 || i == 5 || i == 7 || i == 9) fprintf(f, "-");
        }
        fprintf(f, "]\n");
    } else {
        fprintf(f, "Box: %s\n", box->type);
    }

    // 基本信息
    write_indent_level(f, depth + 1);
    fprintf(f, "  File Offset: 0x%08lx (%lu)\n",
            (unsigned long)box->file_offset, (unsigned long)box->file_offset);
    write_indent_level(f, depth + 1);
    fprintf(f, "  Box Size: %lu bytes (0x%lx)\n",
            (unsigned long)box->box_size, (unsigned long)box->box_size);
    write_indent_level(f, depth + 1);
    fprintf(f, "  Header Size: %lu bytes\n", (unsigned long)box->header_size);
    write_indent_level(f, depth + 1);
    fprintf(f, "  Data Offset: 0x%08lx (%lu)\n",
            (unsigned long)box->data_offset, (unsigned long)box->data_offset);
    write_indent_level(f, depth + 1);
    fprintf(f, "  Data Size: %lu bytes (0x%lx)\n",
            (unsigned long)box->data_size, (unsigned long)box->data_size);
    write_indent_level(f, depth + 1);
    fprintf(f, "  Is Container: %s\n", box->is_container ? "Yes" : "No");

    // 解析已知 box 的字段
    parse_box_fields(f, box, depth + 1);

    // hex dump (如果 data_size > 0)
    if (box->data_size > 0 && box->raw_data.size() > 0) {
        write_indent_level(f, depth + 1);
        if (box->raw_data.size() < box->data_size) {
            fprintf(f, "  Raw Data (first %zu of %lu bytes):\n",
                    box->raw_data.size(), (unsigned long)box->data_size);
        } else {
            fprintf(f, "  Raw Data (%zu bytes):\n", box->raw_data.size());
        }
        dump_hex(f, box->raw_data.data(), box->raw_data.size(), depth + 2);
        if (box->raw_data.size() < box->data_size) {
            write_indent_level(f, depth + 1);
            fprintf(f, "  (remaining %lu bytes not loaded)\n",
                    (unsigned long)(box->data_size - box->raw_data.size()));
        }
    }

    // 递归输出子 box
    for (auto& child : box->children) {
        dump_box_to_file(f, child, depth + 1);
    }
}

// ============================================================
// 输出完整头信息到文件
// ============================================================
bool Mp4Parser::dump_header_to_txt(const std::string& output_filepath) {
    FILE* f = fopen(output_filepath.c_str(), "w");
    if (!f) {
        LOGD("Failed to open output file: %s", output_filepath.c_str());
        return false;
    }

    fprintf(f, "============================================================\n");
    fprintf(f, "  MP4 Header Analysis Report\n");
    fprintf(f, "============================================================\n");
    fprintf(f, "  Source File: %s\n", m_filepath.c_str());
    fprintf(f, "  File Size: %lu bytes (0x%lx)\n",
            (unsigned long)m_file_size, (unsigned long)m_file_size);
    fprintf(f, "  Total Top-Level Boxes: %zu\n", m_top_boxes.size());
    fprintf(f, "============================================================\n\n");

    for (auto& box : m_top_boxes) {
        dump_box_to_file(f, box, 0);
        fprintf(f, "\n");
    }

    fprintf(f, "\n");
    fprintf(f, "============================================================\n");
    fprintf(f, "  End of Report\n");
    fprintf(f, "============================================================\n");

    fclose(f);
    LOGD("Header analysis written to: %s", output_filepath.c_str());
    return true;
}

// ============================================================
// 打印 tree 结构到 stdout
// ============================================================
void Mp4Parser::print_tree() {
    printf("\n=== MP4 Box Tree ===\n\n");

    std::function<void(const std::shared_ptr<Mp4Box>&, int)> print_node;
    print_node = [&](const std::shared_ptr<Mp4Box>& box, int depth) {
        for (int i = 0; i < depth; ++i) printf("    ");
        bool is_uuid = (memcmp(box->type, "uuid", 4) == 0);
        if (is_uuid) {
            printf("+-- uuid\n");
        } else {
            printf("+-- %s", box->type);
            printf("  [size=%lu, off=0x%lx]\n",
                   (unsigned long)box->box_size, (unsigned long)box->file_offset);
        }
        for (auto& child : box->children) {
            print_node(child, depth + 1);
        }
    };

    for (auto& box : m_top_boxes) {
        print_node(box, 0);
    }
    printf("\n");
}