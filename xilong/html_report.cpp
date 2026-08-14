#include "html_report.h"

#include <fstream>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <functional>

// ============================================================
// 辅助函数
// ============================================================

static std::string fmt_num(double v, int ndigits = 1) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", ndigits, v);
    return buf;
}

static std::string fmt_time(double sec) {
    if (sec < 0) sec = 0;
    int64_t ms = (int64_t)std::llround(sec * 1000.0);
    int h = (int)(ms / 3600000); ms %= 3600000;
    int m = (int)(ms / 60000);   ms %= 60000;
    int s = (int)(ms / 1000);    ms %= 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", h, m, s, (int)ms);
    return buf;
}

static std::string fmt_axis_time(double sec) {
    if (sec < 0) sec = 0;
    int t = (int)std::llround(sec);
    int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

static std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

static std::string hex_str_32(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08x", v);
    return buf;
}

static std::string hex_str_64(uint64_t v) {
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%016lx", (unsigned long)v);
    return buf;
}

static std::string size_human(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1073741824ULL)
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
    else if (bytes >= 1048576ULL)
        snprintf(buf, sizeof(buf), "%.2f MB", bytes / 1048576.0);
    else if (bytes >= 1024ULL)
        snprintf(buf, sizeof(buf), "%.2f KB", bytes / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%lu B", (unsigned long)bytes);
    return buf;
}

// 生成 hex dump HTML
static std::string gen_hex_dump_html(const uint8_t* data, size_t size, size_t max_bytes = 256) {
    std::ostringstream o;
    bool truncated = (size > max_bytes);
    size_t show_size = truncated ? max_bytes : size;

    o << "<div class=\"hexdump\"><pre>";
    for (size_t off = 0; off < show_size; off += 16) {
        char line[128];
        snprintf(line, sizeof(line), "%08zx  ", off);
        o << line;

        for (size_t i = 0; i < 16; ++i) {
            if (off + i < show_size) {
                char h[4];
                snprintf(h, sizeof(h), "%02x ", data[off + i]);
                o << h;
            } else {
                o << "   ";
            }
            if (i == 7) o << " ";
        }

        o << " |";
        for (size_t i = 0; i < 16 && off + i < show_size; ++i) {
            uint8_t c = data[off + i];
            if (c >= 32 && c < 127)
                o << (char)c;
            else
                o << ".";
        }
        o << "|\n";
    }
    o << "</pre></div>";

    if (truncated) {
        o << "<p class=\"note\">... 仅显示前 " << max_bytes << " 字节，共 "
          << size << " 字节</p>";
    }
    return o.str();
}

// ============================================================
// 已知 box 字段解析为 HTML 表格行
// ============================================================

static void gen_ftyp_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();
    size_t sz = box->raw_data.size();

    o << "<tr><td>Major Brand</td><td>"
      << (char)d[0] << (char)d[1] << (char)d[2] << (char)d[3]
      << " (" << hex_str_32(Mp4Parser::buf_read_uint32_be(d, 0)) << ")</td></tr>";
    o << "<tr><td>Minor Version</td><td>" << Mp4Parser::buf_read_uint32_be(d, 4)
      << " (" << hex_str_32(Mp4Parser::buf_read_uint32_be(d, 4)) << ")</td></tr>";

    size_t compat_count = (sz - 8) / 4;
    for (size_t i = 0; i < compat_count; ++i) {
        uint32_t brand = Mp4Parser::buf_read_uint32_be(d, 8 + i * 4);
        char bstr[5] = {(char)((brand >> 24) & 0xFF), (char)((brand >> 16) & 0xFF),
                        (char)((brand >> 8) & 0xFF), (char)(brand & 0xFF), '\0'};
        o << "<tr><td>Compatible Brand[" << i << "]</td><td>" << bstr
          << " (" << hex_str_32(brand) << ")</td></tr>";
    }
}

static void gen_mvhd_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box) {
    if (box->raw_data.size() < 100) return;
    const uint8_t* d = box->raw_data.data();
    uint8_t ver = d[0];

    o << "<tr><td>Version</td><td>" << (int)ver << "</td></tr>";

    if (ver == 0) {
        o << "<tr><td>Timescale</td><td>" << Mp4Parser::buf_read_uint32_be(d, 12) << "</td></tr>";
        o << "<tr><td>Duration</td><td>" << Mp4Parser::buf_read_uint32_be(d, 16) << "</td></tr>";
        uint32_t ts = Mp4Parser::buf_read_uint32_be(d, 12);
        uint32_t dur = Mp4Parser::buf_read_uint32_be(d, 16);
        if (ts > 0) {
            double sec = (double)dur / ts;
            char buf[32];
            int h = (int)(sec / 3600); sec -= h * 3600;
            int m = (int)(sec / 60); sec -= m * 60;
            snprintf(buf, sizeof(buf), "%02d:%02d:%06.3f", h, m, sec);
            o << "<tr><td>Duration (时间)</td><td>" << buf << "</td></tr>";
        }
        o << "<tr><td>Rate</td><td>" << (Mp4Parser::buf_read_uint32_be(d, 20) / 65536.0) << "</td></tr>";
        uint16_t vol = Mp4Parser::buf_read_uint16_be(d, 24);
        o << "<tr><td>Volume</td><td>" << (vol / 256.0) << "</td></tr>";
        o << "<tr><td>Next Track ID</td><td>" << Mp4Parser::buf_read_uint32_be(d, 92) << "</td></tr>";
    } else {
        o << "<tr><td>Timescale</td><td>" << Mp4Parser::buf_read_uint32_be(d, 20) << "</td></tr>";
        uint64_t dur = Mp4Parser::buf_read_uint64_be(d, 24);
        o << "<tr><td>Duration</td><td>" << (unsigned long)dur << "</td></tr>";
        uint32_t ts = Mp4Parser::buf_read_uint32_be(d, 20);
        if (ts > 0) {
            double sec = (double)dur / ts;
            char buf[32];
            int h = (int)(sec / 3600); sec -= h * 3600;
            int m = (int)(sec / 60); sec -= m * 60;
            snprintf(buf, sizeof(buf), "%02d:%02d:%06.3f", h, m, sec);
            o << "<tr><td>Duration (时间)</td><td>" << buf << "</td></tr>";
        }
        o << "<tr><td>Next Track ID</td><td>" << Mp4Parser::buf_read_uint32_be(d, 108) << "</td></tr>";
    }
}

static void gen_tkhd_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box) {
    if (box->raw_data.size() < 84) return;
    const uint8_t* d = box->raw_data.data();
    uint8_t ver = d[0];
    uint32_t flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];

    o << "<tr><td>Version</td><td>" << (int)ver << "</td></tr>";
    o << "<tr><td>Flags</td><td>" << hex_str_32(flags);
    if (flags & 0x01) o << " (Enabled)";
    if (flags & 0x02) o << " (In Movie)";
    if (flags & 0x04) o << " (In Preview)";
    o << "</td></tr>";

    size_t off = 4;
    if (ver == 0) {
        o << "<tr><td>Track ID</td><td>" << Mp4Parser::buf_read_uint32_be(d, off + 8) << "</td></tr>";
        o << "<tr><td>Duration</td><td>" << Mp4Parser::buf_read_uint32_be(d, off + 16) << "</td></tr>";
        uint32_t w = Mp4Parser::buf_read_uint32_be(d, 76);
        uint32_t h = Mp4Parser::buf_read_uint32_be(d, 80);
        o << "<tr><td>Width</td><td>" << (w >> 16) << "." << ((w & 0xFFFF) * 100 / 65536) << "</td></tr>";
        o << "<tr><td>Height</td><td>" << (h >> 16) << "." << ((h & 0xFFFF) * 100 / 65536) << "</td></tr>";
    } else {
        o << "<tr><td>Track ID</td><td>" << Mp4Parser::buf_read_uint32_be(d, off + 16) << "</td></tr>";
        o << "<tr><td>Duration</td><td>" << (unsigned long)Mp4Parser::buf_read_uint64_be(d, off + 20) << "</td></tr>";
        uint32_t w = Mp4Parser::buf_read_uint32_be(d, 88);
        uint32_t h = Mp4Parser::buf_read_uint32_be(d, 92);
        o << "<tr><td>Width</td><td>" << (w >> 16) << "." << ((w & 0xFFFF) * 100 / 65536) << "</td></tr>";
        o << "<tr><td>Height</td><td>" << (h >> 16) << "." << ((h & 0xFFFF) * 100 / 65536) << "</td></tr>";
    }
}

static void gen_mdhd_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box) {
    if (box->raw_data.size() < 24) return;
    const uint8_t* d = box->raw_data.data();
    uint8_t ver = d[0];

    o << "<tr><td>Version</td><td>" << (int)ver << "</td></tr>";

    if (ver == 0) {
        o << "<tr><td>Timescale</td><td>" << Mp4Parser::buf_read_uint32_be(d, 12) << "</td></tr>";
        o << "<tr><td>Duration</td><td>" << Mp4Parser::buf_read_uint32_be(d, 16) << "</td></tr>";
        uint32_t ts = Mp4Parser::buf_read_uint32_be(d, 12);
        uint32_t dur = Mp4Parser::buf_read_uint32_be(d, 16);
        if (ts > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.3f s", (double)dur / ts);
            o << "<tr><td>Duration (时间)</td><td>" << buf << "</td></tr>";
        }
        uint16_t lp = Mp4Parser::buf_read_uint16_be(d, 20);
        char lang[4];
        lang[0] = ((lp >> 10) & 0x1F) + 0x60;
        lang[1] = ((lp >> 5) & 0x1F) + 0x60;
        lang[2] = (lp & 0x1F) + 0x60;
        lang[3] = '\0';
        o << "<tr><td>Language</td><td>" << lang << "</td></tr>";
    } else {
        o << "<tr><td>Timescale</td><td>" << Mp4Parser::buf_read_uint32_be(d, 20) << "</td></tr>";
        uint64_t dur = Mp4Parser::buf_read_uint64_be(d, 24);
        o << "<tr><td>Duration</td><td>" << (unsigned long)dur << "</td></tr>";
        uint32_t ts = Mp4Parser::buf_read_uint32_be(d, 20);
        if (ts > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.3f s", (double)dur / ts);
            o << "<tr><td>Duration (时间)</td><td>" << buf << "</td></tr>";
        }
    }
}

static void gen_hdlr_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box) {
    if (box->raw_data.size() < 24) return;
    const uint8_t* d = box->raw_data.data();

    uint32_t ht = Mp4Parser::buf_read_uint32_be(d, 8);
    char hs[5] = {(char)((ht >> 24) & 0xFF), (char)((ht >> 16) & 0xFF),
                  (char)((ht >> 8) & 0xFF), (char)(ht & 0xFF), '\0'};
    o << "<tr><td>Handler Type</td><td>" << hs << "</td></tr>";

    if (box->raw_data.size() > 24) {
        o << "<tr><td>Handler Name</td><td>" << html_escape(std::string((const char*)(d + 24))) << "</td></tr>";
    }
}

static void gen_stts_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();
    uint32_t count = Mp4Parser::buf_read_uint32_be(d, 4);
    o << "<tr><td>Entry Count</td><td>" << count << "</td></tr>";

    uint32_t show = std::min(count, (uint32_t)20);
    for (uint32_t i = 0; i < show; ++i) {
        if (8 + i * 8 + 8 > box->raw_data.size()) break;
        o << "<tr><td>Entry[" << i << "]</td><td>count="
          << Mp4Parser::buf_read_uint32_be(d, 8 + i * 8)
          << ", delta=" << Mp4Parser::buf_read_uint32_be(d, 12 + i * 8) << "</td></tr>";
    }
    if (count > 20) o << "<tr><td>...</td><td>(" << (count - 20) << " more entries)</td></tr>";
}

static void gen_stsz_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box) {
    if (box->raw_data.size() < 12) return;
    const uint8_t* d = box->raw_data.data();
    uint32_t ss = Mp4Parser::buf_read_uint32_be(d, 4);
    uint32_t sc = Mp4Parser::buf_read_uint32_be(d, 8);
    o << "<tr><td>Sample Size</td><td>" << ss << (ss == 0 ? " (variable)" : " (constant)") << "</td></tr>";
    o << "<tr><td>Sample Count</td><td>" << sc << "</td></tr>";
}

static void gen_stco_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();
    uint32_t count = Mp4Parser::buf_read_uint32_be(d, 4);
    bool is_co64 = (memcmp(box->type, "co64", 4) == 0);
    o << "<tr><td>Entry Count</td><td>" << count << "</td></tr>";
    o << "<tr><td>Type</td><td>" << (is_co64 ? "co64 (64-bit)" : "stco (32-bit)") << "</td></tr>";
}

static void gen_stsc_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();
    uint32_t count = Mp4Parser::buf_read_uint32_be(d, 4);
    o << "<tr><td>Entry Count</td><td>" << count << "</td></tr>";
}

static void gen_elst_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box) {
    if (box->raw_data.size() < 8) return;
    const uint8_t* d = box->raw_data.data();
    uint8_t ver = d[0];
    uint32_t count = Mp4Parser::buf_read_uint32_be(d, 4);
    o << "<tr><td>Version</td><td>" << (int)ver << "</td></tr>";
    o << "<tr><td>Entry Count</td><td>" << count << "</td></tr>";

    for (uint32_t i = 0; i < count; ++i) {
        if (ver == 0) {
            if (8 + i * 12 + 12 > box->raw_data.size()) break;
            o << "<tr><td>Entry[" << i << "]</td><td>duration="
              << Mp4Parser::buf_read_uint32_be(d, 8 + i * 12)
              << ", media_time=" << (int32_t)Mp4Parser::buf_read_uint32_be(d, 12 + i * 12)
              << "</td></tr>";
        } else {
            if (8 + i * 20 + 20 > box->raw_data.size()) break;
            o << "<tr><td>Entry[" << i << "]</td><td>duration="
              << (unsigned long)Mp4Parser::buf_read_uint64_be(d, 8 + i * 20)
              << ", media_time=" << (int64_t)Mp4Parser::buf_read_uint64_be(d, 16 + i * 20)
              << "</td></tr>";
        }
    }
}

// 为已知 box 类型生成解析后的 HTML 表格行
static void gen_known_fields_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box) {
    if (memcmp(box->type, "ftyp", 4) == 0) gen_ftyp_html(o, box);
    else if (memcmp(box->type, "mvhd", 4) == 0) gen_mvhd_html(o, box);
    else if (memcmp(box->type, "tkhd", 4) == 0) gen_tkhd_html(o, box);
    else if (memcmp(box->type, "mdhd", 4) == 0) gen_mdhd_html(o, box);
    else if (memcmp(box->type, "hdlr", 4) == 0) gen_hdlr_html(o, box);
    else if (memcmp(box->type, "stts", 4) == 0) gen_stts_html(o, box);
    else if (memcmp(box->type, "stsz", 4) == 0) gen_stsz_html(o, box);
    else if (memcmp(box->type, "stco", 4) == 0 || memcmp(box->type, "co64", 4) == 0) gen_stco_html(o, box);
    else if (memcmp(box->type, "stsc", 4) == 0) gen_stsc_html(o, box);
    else if (memcmp(box->type, "elst", 4) == 0) gen_elst_html(o, box);
}

// ============================================================
// SVG 图表生成 (用于质量分析)
// ============================================================

static const int ML = 60, MR = 20, MT = 16, MB = 36;

static void downsample_max(const std::vector<double>& xs, const std::vector<double>& ys,
                           size_t maxpts, std::vector<double>& ox, std::vector<double>& oy) {
    ox.clear(); oy.clear();
    if (xs.empty()) return;
    if (xs.size() <= maxpts) { ox = xs; oy = ys; return; }
    size_t n = xs.size();
    double per = (double)n / (double)maxpts;
    for (size_t k = 0; k < maxpts; ++k) {
        size_t b = (size_t)(k * per);
        size_t e = (size_t)((k + 1) * per);
        if (e > n) e = n;
        if (e <= b) e = b + 1;
        double mx = ys[b], mxx = xs[b];
        for (size_t i = b; i < e; ++i) {
            if (ys[i] > mx) { mx = ys[i]; mxx = xs[i]; }
        }
        ox.push_back(mxx);
        oy.push_back(mx);
    }
}

static std::string svg_line_chart(const std::vector<double>& xs, const std::vector<double>& ys,
                                  int w, int h, double ymin, double ymax,
                                  const char* color,
                                  const std::vector<std::pair<double, std::string>>& hlines) {
    std::ostringstream o;
    if (xs.size() < 2 || ymax <= ymin) {
        o << "<svg width=\"" << w << "\" height=\"" << h << "\">"
          << "<text x=\"10\" y=\"20\" font-size=\"12\" fill=\"#888\">数据不足</text></svg>";
        return o.str();
    }
    const double x0 = xs.front(), x1 = xs.back();
    const double xr = (x1 > x0) ? (x1 - x0) : 1.0;
    const double yr = ymax - ymin;
    const int pw = w - ML - MR, ph = h - MT - MB;

    auto X = [&](double x) { return ML + (x - x0) / xr * pw; };
    auto Y = [&](double y) {
        double v = (y - ymin) / yr;
        if (v < 0) v = 0; if (v > 1) v = 1;
        return MT + (1.0 - v) * ph;
    };

    o << "<svg width=\"" << w << "\" height=\"" << h
      << "\" style=\"background:#fff;border:1px solid #e2e8f0;border-radius:6px;\">";

    for (int k = 0; k <= 4; ++k) {
        double yv = ymin + yr * k / 4.0;
        double yy = Y(yv);
        o << "<line x1=\"" << ML << "\" y1=\"" << yy << "\" x2=\"" << (w - MR)
          << "\" y2=\"" << yy << "\" stroke=\"#edf2f7\" stroke-width=\"1\"/>";
        o << "<text x=\"" << (ML - 6) << "\" y=\"" << (yy + 4)
          << "\" font-size=\"11\" fill=\"#718096\" text-anchor=\"end\">"
          << fmt_num(yv, yr < 10 ? 1 : 0) << "</text>";
    }
    for (int k = 0; k <= 5; ++k) {
        double xv = x0 + xr * k / 5.0;
        double xx = X(xv);
        o << "<text x=\"" << xx << "\" y=\"" << (h - MB + 18)
          << "\" font-size=\"11\" fill=\"#718096\" text-anchor=\"middle\">"
          << fmt_axis_time(xv) << "</text>";
    }
    for (const auto& hl : hlines) {
        if (hl.first < ymin || hl.first > ymax) continue;
        double yy = Y(hl.first);
        o << "<line x1=\"" << ML << "\" y1=\"" << yy << "\" x2=\"" << (w - MR)
          << "\" y2=\"" << yy << "\" stroke=\"" << hl.second << "\" stroke-width=\"1\" "
          << "stroke-dasharray=\"6,4\"/>";
    }
    o << "<polyline fill=\"none\" stroke=\"" << color << "\" stroke-width=\"1.5\" points=\"";
    for (size_t i = 0; i < xs.size(); ++i) {
        o << fmt_num(X(xs[i]), 1) << "," << fmt_num(Y(ys[i]), 1) << " ";
    }
    o << "\"/>";
    o << "<rect x=\"" << ML << "\" y=\"" << MT << "\" width=\"" << pw << "\" height=\"" << ph
      << "\" fill=\"none\" stroke=\"#cbd5e0\" stroke-width=\"1\"/>";
    o << "</svg>";
    return o.str();
}

static std::string svg_bar_chart(const std::vector<double>& vals, int w, int h,
                                 const char* color, double avg_line) {
    std::ostringstream o;
    if (vals.empty()) {
        o << "<svg width=\"" << w << "\" height=\"" << h << "\">"
          << "<text x=\"10\" y=\"20\" font-size=\"12\" fill=\"#888\">数据不足</text></svg>";
        return o.str();
    }
    std::vector<double> v;
    const size_t maxbars = 2000;
    if (vals.size() <= maxbars) {
        v = vals;
    } else {
        double per = (double)vals.size() / (double)maxbars;
        for (size_t k = 0; k < maxbars; ++k) {
            size_t b = (size_t)(k * per), e = (size_t)((k + 1) * per);
            if (e > vals.size()) e = vals.size();
            if (e <= b) e = b + 1;
            double s = 0;
            for (size_t i = b; i < e; ++i) s += vals[i];
            v.push_back(s / (e - b));
        }
    }
    double vmax = 0;
    for (double x : v) if (x > vmax) vmax = x;
    if (avg_line > vmax) vmax = avg_line;
    vmax *= 1.1;
    if (vmax <= 0) vmax = 1;
    const int pw = w - ML - MR, ph = h - MT - MB;
    auto Y = [&](double y) { return MT + (1.0 - y / vmax) * ph; };

    o << "<svg width=\"" << w << "\" height=\"" << h
      << "\" style=\"background:#fff;border:1px solid #e2e8f0;border-radius:6px;\">";
    for (int k = 0; k <= 4; ++k) {
        double yv = vmax * k / 4.0;
        double yy = Y(yv);
        o << "<line x1=\"" << ML << "\" y1=\"" << yy << "\" x2=\"" << (w - MR)
          << "\" y2=\"" << yy << "\" stroke=\"#edf2f7\" stroke-width=\"1\"/>";
        o << "<text x=\"" << (ML - 6) << "\" y=\"" << (yy + 4)
          << "\" font-size=\"11\" fill=\"#718096\" text-anchor=\"end\">"
          << fmt_num(yv, 0) << "</text>";
    }
    double sec_per_bar = (double)vals.size() / (double)v.size();
    for (int k = 0; k <= 5; ++k) {
        double sec = vals.size() * k / 5.0;
        double xx = ML + pw * k / 5.0;
        o << "<text x=\"" << xx << "\" y=\"" << (h - MB + 18)
          << "\" font-size=\"11\" fill=\"#718096\" text-anchor=\"middle\">"
          << fmt_axis_time(sec) << "</text>";
    }
    double bw = (double)pw / v.size();
    double draw_w = (bw > 2) ? bw - 1 : bw;
    if (draw_w < 0.5) draw_w = 0.5;
    for (size_t i = 0; i < v.size(); ++i) {
        double yy = Y(v[i]);
        o << "<rect x=\"" << fmt_num(ML + i * bw, 2) << "\" y=\"" << fmt_num(yy, 2)
          << "\" width=\"" << fmt_num(draw_w, 2) << "\" height=\"" << fmt_num(MT + ph - yy, 2)
          << "\" fill=\"" << color << "\" opacity=\"0.85\"/>";
    }
    if (avg_line > 0) {
        double yy = Y(avg_line);
        o << "<line x1=\"" << ML << "\" y1=\"" << yy << "\" x2=\"" << (w - MR)
          << "\" y2=\"" << yy << "\" stroke=\"#2f855a\" stroke-width=\"1\" stroke-dasharray=\"6,4\"/>"
          << "<text x=\"" << (w - MR - 4) << "\" y=\"" << (yy - 4)
          << "\" font-size=\"11\" fill=\"#2f855a\" text-anchor=\"end\">平均 "
          << fmt_num(avg_line, 0) << " kbps</text>";
    }
    o << "<rect x=\"" << ML << "\" y=\"" << MT << "\" width=\"" << pw << "\" height=\"" << ph
      << "\" fill=\"none\" stroke=\"#cbd5e0\" stroke-width=\"1\"/>";
    o << "</svg>";
    return o.str();
}

// 生成质量事件表格
static std::string gen_gap_event_table(const std::vector<sunxilong::GapEvent>& evs, bool is_drop, size_t max_rows = 500) {
    std::ostringstream o;
    o << "<table><tr><th>序号</th><th>时间戳</th><th>帧序号</th><th>实际间隔(ms)</th><th>预期间隔(ms)</th>";
    if (is_drop) o << "<th>估计丢帧数</th>";
    o << "</tr>";
    size_t show = std::min(evs.size(), max_rows);
    for (size_t i = 0; i < show; ++i) {
        const auto& ev = evs[i];
        o << "<tr><td>" << (i + 1) << "</td><td>" << fmt_time(ev.t) << "</td><td>" << ev.frame_index
          << "</td><td>" << fmt_num(ev.gap_ms, 2) << "</td><td>" << fmt_num(ev.expected_ms, 2) << "</td>";
        if (is_drop) o << "<td>" << ev.dropped_count << "</td>";
        o << "</tr>";
    }
    o << "</table>";
    if (evs.size() > max_rows) {
        o << "<p class=\"note\">事件过多, 仅显示前 " << max_rows << " 条, 共 " << evs.size() << " 条。</p>";
    }
    return o.str();
}

// ============================================================
// 递归生成 box 的 HTML (嵌套 <details>)
// ============================================================

static void gen_box_html(std::ostringstream& o, const std::shared_ptr<Mp4Box>& box, int depth) {
    bool is_uuid = (memcmp(box->type, "uuid", 4) == 0);
    bool is_sxmd = false;
    SxmdInfo sxmd;
    if (is_uuid) {
        is_sxmd = Mp4Parser::try_parse_sxmd(box, sxmd);
    }

    bool is_large_data = (memcmp(box->type, "mdat", 4) == 0) ||
                         (memcmp(box->type, "free", 4) == 0) ||
                         (memcmp(box->type, "skip", 4) == 0);
    bool has_children = !box->children.empty();

    // 构建 summary 文本
    std::string box_label;
    if (is_uuid && is_sxmd) {
        char buf[128];
        snprintf(buf, sizeof(buf), "uuid [SXMD] — %s, %s",
                 sxmd.record_type_str().c_str(),
                 size_human(box->box_size).c_str());
        box_label = buf;
    } else if (is_uuid) {
        char uuid_str[40];
        for (int i = 0; i < 16; ++i) {
            snprintf(uuid_str + i * 2, 3, "%02x", box->uuid[i]);
        }
        box_label = std::string("uuid [") + uuid_str + "] — " + size_human(box->box_size);
    } else {
        box_label = std::string(box->type) + " — " + size_human(box->box_size);
    }

    // 顶层 box 或有子 box 的默认展开，深层 box 默认折叠
    bool default_open = (depth < 2) || has_children;

    o << "<details class=\"box level-" << depth << "\""
      << (default_open ? " open" : "") << ">\n";
    o << "<summary>" << box_label << "</summary>\n";

    // 基本信息表
    o << "<table class=\"box-info\">\n";
    o << "<tr><td>File Offset</td><td>" << hex_str_64(box->file_offset)
      << " (" << (unsigned long)box->file_offset << ")</td></tr>";
    o << "<tr><td>Box Size</td><td>" << (unsigned long)box->box_size << " bytes</td></tr>";
    o << "<tr><td>Header Size</td><td>" << (unsigned long)box->header_size << " bytes</td></tr>";
    o << "<tr><td>Data Offset</td><td>" << hex_str_64(box->data_offset) << "</td></tr>";
    o << "<tr><td>Data Size</td><td>" << (unsigned long)box->data_size << " bytes</td></tr>";

    // 已知 box 的解析字段
    gen_known_fields_html(o, box);

    // SXMD 特殊显示
    if (is_sxmd) {
        o << "<tr class=\"sxmd-highlight\"><td>SXMD Record Type</td><td><strong>"
          << sxmd.record_type_str() << "</strong></td></tr>\n";
        if (sxmd.is_slow_motion()) {
            o << "<tr class=\"sxmd-highlight\"><td>慢动作倍数</td><td><strong>x"
              << (sxmd.slow_motion_multiplier_x100 / 100.0) << "</strong></td></tr>\n";
        }
        if (sxmd.is_timelapse()) {
            o << "<tr class=\"sxmd-highlight\"><td>延时倍数</td><td><strong>x"
              << (sxmd.timelapse_multiplier_x100 / 100.0) << "</strong></td></tr>\n";
            o << "<tr class=\"sxmd-highlight\"><td>延时间隔帧</td><td>"
              << sxmd.timelapse_interval_frame << "</td></tr>\n";
            o << "<tr class=\"sxmd-highlight\"><td>目标 FPS</td><td>"
              << sxmd.target_fps << "</td></tr>\n";
        }
        o << "<tr><td>Flags</td><td>0x" << std::hex << (int)sxmd.flags << std::dec;
        if (sxmd.flags & 0x01) o << " [no_audio]";
        if (sxmd.flags & 0x02) o << " [timestamp_modified]";
        o << "</td></tr>\n";
    }

    o << "</table>\n";

    // Hex dump (默认折叠)
    if (box->raw_data.size() > 0 && !is_large_data) {
        o << "<details class=\"hex-section\">\n";
        o << "<summary>Raw Data (" << box->raw_data.size() << " bytes)</summary>\n";
        o << gen_hex_dump_html(box->raw_data.data(), box->raw_data.size());
        o << "</details>\n";
    } else if (is_large_data && box->raw_data.size() > 0) {
        o << "<details class=\"hex-section\">\n";
        o << "<summary>Raw Data (first " << box->raw_data.size()
          << " of " << (unsigned long)box->data_size << " bytes)</summary>\n";
        o << gen_hex_dump_html(box->raw_data.data(), box->raw_data.size());
        o << "</details>\n";
    }

    // 递归子 box
    for (const auto& child : box->children) {
        gen_box_html(o, child, depth + 1);
    }

    o << "</details>\n";
}

// ============================================================
// 主入口: 生成 HTML 报告
// ============================================================

bool write_mp4_html_report(const Mp4Parser& parser,
                           const std::string& source_filepath,
                           uint64_t file_size,
                           const std::string& output_path,
                           const sunxilong::AnalyzeResult* qc_result,
                           const sunxilong::AnalyzeOptions* qc_opt)
{
    std::ostringstream o;
    const auto& boxes = parser.get_top_boxes();
    bool has_qc = (qc_result && qc_result->valid);

    // 时间戳
    char timebuf[64];
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tmv);

    // HTML head
    o << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"UTF-8\">\n"
      << "<title>MP4 综合分析报告 - " << html_escape(source_filepath) << "</title>\n"
      << "<style>\n"
      << "body{font-family:'Segoe UI','Microsoft YaHei',sans-serif;margin:24px;color:#2d3748;background:#f7fafc;}\n"
      << "h1{font-size:22px;margin-bottom:4px;} h2{font-size:17px;margin-top:32px;border-left:4px solid #4299e1;padding-left:8px;}\n"
      << "table{border-collapse:collapse;margin-top:6px;background:#fff;font-size:13px;}\n"
      << "th,td{border:1px solid #e2e8f0;padding:4px 10px;text-align:right;}\n"
      << "td:first-child{background:#f7fafc;font-weight:600;white-space:nowrap;width:180px;}\n"
      << "th{background:#edf2f7;}\n"
      << ".note{font-size:12px;color:#718096;}\n"
      << "details{margin-left:16px;border-left:2px solid #e2e8f0;padding-left:8px;margin-top:2px;}\n"
      << "details.level-0{margin-left:0;border-left:none;padding-left:0;margin-top:8px;}\n"
      << "details.level-1{margin-left:12px;}\n"
      << "summary{cursor:pointer;padding:3px 6px;border-radius:4px;font-size:14px;font-weight:600;color:#2b6cb0;}\n"
      << "summary:hover{background:#ebf4ff;}\n"
      << ".box-info{margin:4px 0 8px 0;}\n"
      << ".hex-section{margin-top:4px;}\n"
      << ".hexdump{background:#1a202c;color:#e2e8f0;border-radius:6px;padding:10px;overflow-x:auto;max-height:400px;overflow-y:auto;}\n"
      << ".hexdump pre{margin:0;font-family:'Cascadia Code','Fira Code','Consolas',monospace;font-size:12px;line-height:1.5;}\n"
      << ".sxmd-highlight td{background:#fefcbf !important;font-weight:600;}\n"
      << ".cards{display:flex;flex-wrap:wrap;gap:12px;margin:12px 0;}\n"
      << ".card{background:#fff;border:1px solid #e2e8f0;border-radius:8px;padding:12px 18px;min-width:160px;}\n"
      << ".card .num{font-size:24px;font-weight:700;}\n"
      << ".card .lbl{font-size:12px;color:#718096;margin-top:2px;}\n"
      << ".ok{color:#2f855a;} .bad{color:#e53e3e;} .warn{color:#dd6b20;} .info{color:#2b6cb0;}\n"
      << ".sxmd-banner{background:#fefcbf;border:2px solid #d69e2e;border-radius:8px;padding:16px;margin:12px 0;}\n"
      << ".sxmd-banner h3{margin:0 0 8px 0;color:#744210;}\n"
      << ".sxmd-banner .field{margin:4px 0;font-size:14px;}\n"
      << ".sxmd-banner .value{font-weight:700;color:#2d3748;}\n"
      << ".scroll{max-height:420px;overflow-y:auto;border:1px solid #e2e8f0;border-radius:6px;background:#fff;}\n"
      << ".legend{font-size:12px;color:#4a5568;margin:6px 0;}\n"
      << "</style>\n</head>\n<body>\n";

    // 标题
    o << "<h1>MP4 综合分析报告</h1>\n"
      << "<p class=\"note\">生成时间: " << timebuf << "</p>\n";

    // ---- 概览 ----
    o << "<h2>1. 文件概览</h2>\n";
    o << "<div class=\"cards\">\n";
    o << "<div class=\"card\"><div class=\"num info\">" << size_human(file_size) << "</div>"
      << "<div class=\"lbl\">文件大小</div></div>\n";
    o << "<div class=\"card\"><div class=\"num\">" << boxes.size() << "</div>"
      << "<div class=\"lbl\">顶层 Box 数量</div></div>\n";

    // 统计 box 总数
    size_t total_boxes = 0;
    std::function<void(const std::shared_ptr<Mp4Box>&)> count_boxes;
    count_boxes = [&](const std::shared_ptr<Mp4Box>& b) {
        total_boxes++;
        for (const auto& c : b->children) count_boxes(c);
    };
    for (const auto& b : boxes) count_boxes(b);
    o << "<div class=\"card\"><div class=\"num\">" << total_boxes << "</div>"
      << "<div class=\"lbl\">Box 总数 (含嵌套)</div></div>\n";

    // 收集 SXMD
    std::vector<SxmdInfo> sxmd_list;
    parser.collect_sxmd(sxmd_list);
    if (!sxmd_list.empty()) {
        o << "<div class=\"card\"><div class=\"num ok\">" << sxmd_list.size() << "</div>"
          << "<div class=\"lbl\">SXMD 元数据</div></div>\n";
    }

    // 质量分析卡片
    if (has_qc) {
        o << "<div class=\"card\"><div class=\"num " << (qc_result->fps_ok ? "ok\">PASS" : "bad\">FAIL")
          << "</div><div class=\"lbl\">帧率检查 (" << fmt_num(qc_result->preset_fps, 1) << " fps)</div></div>\n";
        o << "<div class=\"card\"><div class=\"num " << (qc_result->drop_events.empty() ? "ok\">0" : "bad\">" + std::to_string(qc_result->drop_events.size()))
          << "</div><div class=\"lbl\">丢帧事件</div></div>\n";
        o << "<div class=\"card\"><div class=\"num " << (qc_result->fast_events.empty() ? "ok\">0" : "warn\">" + std::to_string(qc_result->fast_events.size()))
          << "</div><div class=\"lbl\">超快帧事件</div></div>\n";
        o << "<div class=\"card\"><div class=\"num\">" << fmt_num(qc_result->vbr.avg_kbps, 0)
          << "</div><div class=\"lbl\">平均码率 (kbps)</div></div>\n";
        o << "<div class=\"card\"><div class=\"num\">" << fmt_num(qc_result->vbr.stddev_kbps, 0)
          << "</div><div class=\"lbl\">码率标准差 (kbps)</div></div>\n";
        double cv = (qc_result->vbr.avg_kbps > 0) ? (qc_result->vbr.stddev_kbps / qc_result->vbr.avg_kbps * 100.0) : 0.0;
        o << "<div class=\"card\"><div class=\"num\">" << fmt_num(cv, 1) << "%</div>"
          << "<div class=\"lbl\">码率波动系数 (CV)</div></div>\n";
    }

    o << "</div>\n"; // cards

    // 文件信息表
    o << "<table><tr><td>文件路径</td><td style=\"text-align:left;\">" << html_escape(source_filepath) << "</td></tr>"
      << "<tr><td>文件大小</td><td>" << (unsigned long)file_size << " bytes (" << size_human(file_size) << ")</td></tr>"
      << "<tr><td>顶层 Box 数</td><td>" << boxes.size() << "</td></tr>";
    if (has_qc) {
        o << "<tr><td>编码格式</td><td>" << html_escape(qc_result->codec_name) << "</td></tr>"
          << "<tr><td>分辨率</td><td>" << qc_result->width << "x" << qc_result->height << "</td></tr>"
          << "<tr><td>时长</td><td>" << fmt_num(qc_result->duration_sec, 2) << " s</td></tr>"
          << "<tr><td>总帧数</td><td>" << qc_result->frames.size() << "</td></tr>"
          << "<tr><td>容器帧率</td><td>" << fmt_num(qc_result->container_fps, 3) << " fps</td></tr>"
          << "<tr><td>实测帧率</td><td>" << fmt_num(qc_result->measured_fps, 3) << " fps</td></tr>";
    }
    o << "</table>\n";

    // ---- SXMD 摘要 ----
    if (!sxmd_list.empty()) {
        o << "<h2>2. SXMD 私有元数据</h2>\n";
        for (size_t i = 0; i < sxmd_list.size(); ++i) {
            const SxmdInfo& s = sxmd_list[i];
            o << "<div class=\"sxmd-banner\">\n";
            o << "<h3>SXMD #" << (i + 1) << "</h3>\n";
            o << "<div class=\"field\">录像类型: <span class=\"value\">" << s.record_type_str() << "</span></div>\n";

            if (s.is_slow_motion()) {
                o << "<div class=\"field\">慢动作倍数: <span class=\"value\">x"
                  << (s.slow_motion_multiplier_x100 / 100.0) << "</span></div>\n";
            }
            if (s.is_timelapse()) {
                o << "<div class=\"field\">延时倍数: <span class=\"value\">x"
                  << (s.timelapse_multiplier_x100 / 100.0) << "</span></div>\n";
                o << "<div class=\"field\">延时间隔帧: <span class=\"value\">"
                  << s.timelapse_interval_frame << "</span></div>\n";
                o << "<div class=\"field\">目标 FPS: <span class=\"value\">"
                  << s.target_fps << "</span></div>\n";
            }

            o << "<div class=\"field\">Flags: <span class=\"value\">0x" << std::hex << (int)s.flags << std::dec << "</span>";
            if (s.flags & 0x01) o << " [无音频]";
            if (s.flags & 0x02) o << " [时间戳已修改]";
            o << "</div>\n";

            // 业务含义
            o << "<div class=\"field\" style=\"margin-top:8px;font-size:15px;\">";
            if (s.is_slow_motion()) {
                o << "&#x2192; 这是一个<strong>慢动作录像</strong>，x"
                  << (s.slow_motion_multiplier_x100 / 100.0) << " 慢速回放";
            } else if (s.record_type == 2) {
                o << "&#x2192; 这是一个<strong>静止延时录像</strong>，x"
                  << (s.timelapse_multiplier_x100 / 100.0) << " 加速";
            } else if (s.record_type == 3) {
                o << "&#x2192; 这是一个<strong>运动延时录像</strong>，x"
                  << (s.timelapse_multiplier_x100 / 100.0) << " 加速";
            } else if (s.record_type == 4) {
                o << "&#x2192; 这是一个<strong>轨迹延时录像</strong>，x"
                  << (s.timelapse_multiplier_x100 / 100.0) << " 加速";
            } else {
                o << "&#x2192; 普通录像";
            }
            o << "</div>\n";
            o << "</div>\n"; // sxmd-banner
        }
    }

    // ---- Box 结构树 ----
    int section_num = sxmd_list.empty() ? 2 : 3;
    int next_section = section_num;

    // ---- 质量分析章节 ----
    if (has_qc) {
        const double expected_ms = 1000.0 / qc_result->preset_fps;
        const double drop_th_ms = expected_ms * (qc_opt ? qc_opt->drop_ratio : 1.5);
        const double fast_th_ms = expected_ms * (qc_opt ? qc_opt->fast_ratio : 0.5);
        const double t0 = qc_result->frames.empty() ? 0.0 : qc_result->frames.front().pts;

        // 检查结论
        next_section++;
        o << "<h2>" << next_section << ". 质量检查结论</h2>\n";
        o << "<p>预设帧率: " << fmt_num(qc_result->preset_fps, 1) << " fps, 预期间隔: "
          << fmt_num(expected_ms, 2) << " ms</p>\n";
        o << "<p>丢帧阈值: " << fmt_num(drop_th_ms, 2) << " ms, 超快阈值: "
          << fmt_num(fast_th_ms, 2) << " ms</p>\n";
        if (qc_result->bad_pts_count > 0) {
            o << "<p class=\"warn\">注意: " << qc_result->bad_pts_count << " 处 PTS 不单调/重复</p>\n";
        }

        // 帧间隔分布图
        next_section++;
        o << "<h2>" << next_section << ". 帧间隔分布</h2>\n";
        {
            std::vector<double> xs, ys;
            xs.reserve(qc_result->intervals_ms.size());
            const size_t n = qc_result->frames.size();
            for (size_t i = 1; i < n; ++i) xs.push_back(qc_result->frames[i].pts - t0);
            ys = qc_result->intervals_ms;
            std::vector<double> dx, dy;
            downsample_max(xs, ys, 2000, dx, dy);
            double ymax = drop_th_ms * 1.2;
            for (double v : dy) if (v > ymax) ymax = v;
            ymax *= 1.05;
            std::vector<std::pair<double, std::string>> hlines;
            hlines.push_back({expected_ms, "#38a169"});
            hlines.push_back({drop_th_ms, "#e53e3e"});
            hlines.push_back({fast_th_ms, "#dd6b20"});
            o << "<p class=\"legend\">蓝线: 实际帧间隔(ms) | "
              << "<span style=\"color:#38a169\">绿虚线: 预期间隔 " << fmt_num(expected_ms, 1) << " ms</span> | "
              << "<span style=\"color:#e53e3e\">红虚线: 丢帧阈值 " << fmt_num(drop_th_ms, 1) << " ms</span> | "
              << "<span style=\"color:#dd6b20\">橙虚线: 超快阈值 " << fmt_num(fast_th_ms, 1) << " ms</span>"
              << "</p>\n";
            o << svg_line_chart(dx, dy, 1200, 320, 0.0, ymax, "#2b6cb0", hlines) << "\n";
        }

        // 丢帧明细
        next_section++;
        o << "<h2>" << next_section << ". 丢帧明细 (事件 " << qc_result->drop_events.size()
          << " 处, 估计共丢 " << qc_result->total_dropped_frames << " 帧)</h2>\n";
        if (qc_result->drop_events.empty()) {
            o << "<p class=\"ok\">未检测到丢帧。</p>\n";
        } else {
            o << "<div class=\"scroll\">" << gen_gap_event_table(qc_result->drop_events, true) << "</div>\n";
        }

        // 超快帧明细
        next_section++;
        o << "<h2>" << next_section << ". 超快帧明细 (共 " << qc_result->fast_events.size() << " 处)</h2>\n";
        if (qc_result->fast_events.empty()) {
            o << "<p class=\"ok\">未检测到超快帧。</p>\n";
        } else {
            o << "<div class=\"scroll\">" << gen_gap_event_table(qc_result->fast_events, false) << "</div>\n";
        }

        // 码率分析
        next_section++;
        o << "<h2>" << next_section << ". 码率分析</h2>\n";
        o << "<table>"
          << "<tr><td>平均码率</td><td>" << fmt_num(qc_result->vbr.avg_kbps, 0) << " kbps</td></tr>"
          << "<tr><td>最大秒级码率</td><td>" << fmt_num(qc_result->vbr.max_kbps, 0) << " kbps</td></tr>"
          << "<tr><td>最小秒级码率</td><td>" << fmt_num(qc_result->vbr.min_kbps, 0) << " kbps</td></tr>"
          << "<tr><td>标准差(波动)</td><td>" << fmt_num(qc_result->vbr.stddev_kbps, 0) << " kbps</td></tr>"
          << "<tr><td>视频数据量</td><td>" << fmt_num(qc_result->video_bytes / 1048576.0, 2) << " MB</td></tr>"
          << "</table>\n";
        o << svg_bar_chart(qc_result->vbr.per_second_kbps, 1200, 300, "#4299e1", qc_result->vbr.avg_kbps) << "\n";
    }

    // Box 结构树
    next_section++;
    o << "<h2>" << next_section << ". Box 结构树</h2>\n";
    o << "<p class=\"note\">点击三角标展开/折叠各级 Box。二进制数据默认隐藏，点击 \"Raw Data\" 可查看。</p>\n";

    for (const auto& box : boxes) {
        gen_box_html(o, box, 0);
    }

    o << "</body>\n</html>\n";

    // 写入文件
    std::ofstream ofs(output_path.c_str(), std::ios::binary);
    if (!ofs.is_open()) return false;
    const std::string s = o.str();
    ofs.write(s.c_str(), (std::streamsize)s.size());
    return ofs.good();
}
