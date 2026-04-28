#include "nal.h"

namespace mclc {

namespace {

/// 找下一个 Annex-B 起始码（0x000001 或 0x00000001）。返回起始码起始位置；找不到返 size。
std::size_t find_start_code(std::span<const uint8_t> data, std::size_t from) noexcept {
    if (data.size() < 3) return data.size();
    for (std::size_t i = from; i + 2 < data.size(); ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i;
    }
    return data.size();
}

/// 起始码长度：3（0x000001）或 4（0x00000001）。
std::size_t start_code_length(std::span<const uint8_t> data, std::size_t pos) noexcept {
    if (pos > 0 && data[pos - 1] == 0) return 4;
    return 3;
}

void extract_rbsp(std::span<const uint8_t> ebsp, std::vector<uint8_t>& rbsp) noexcept {
    rbsp.clear();
    rbsp.reserve(ebsp.size());
    // 剔 emulation_prevention_three_byte：连续两个 0x00 后若是 0x03 则丢 0x03（§7.4.1.1）。
    std::size_t i = 0;
    const std::size_t n = ebsp.size();
    while (i < n) {
        if (i + 2 < n && ebsp[i] == 0 && ebsp[i + 1] == 0 && ebsp[i + 2] == 0x03) {
            rbsp.push_back(0);
            rbsp.push_back(0);
            i += 3;
        } else {
            rbsp.push_back(ebsp[i]);
            ++i;
        }
    }
}

}  // namespace

void hevc_split_nals(std::span<const uint8_t> annexb,
                      const NalCallback&        cb,
                      std::vector<uint8_t>&     scratch_rbsp) noexcept {
    std::size_t i = find_start_code(annexb, 0);
    while (i < annexb.size()) {
        const std::size_t start = i + start_code_length(annexb, i);
        const std::size_t next  = find_start_code(annexb, start + 1);

        // 计算 NAL 边界：[start, end)，end 是下一个起始码前一字节（剔可选 0x00 padding）。
        std::size_t end = next;
        if (end > start && next < annexb.size()) {
            // 下一个起始码可能是 4 字节 (0x00 00 00 01)，回退一字节。
            if (next > 0 && annexb[next - 1] == 0) --end;
        }
        if (end <= start + 2) { i = next; continue; }     // 至少 2 字节 NAL header

        std::span<const uint8_t> ebsp = annexb.subspan(start, end - start);

        // NAL header 解析（2 字节）。
        const uint8_t b0 = ebsp[0];
        const uint8_t b1 = ebsp[1];
        HevcNalHeader hdr{};
        hdr.forbidden_zero_bit = static_cast<uint8_t>((b0 >> 7) & 0x01);
        hdr.nal_unit_type      = static_cast<uint8_t>((b0 >> 1) & 0x3F);
        hdr.layer_id           = static_cast<uint8_t>(((b0 & 0x01) << 5) | ((b1 >> 3) & 0x1F));
        hdr.temporal_id_plus1  = static_cast<uint8_t>(b1 & 0x07);

        // RBSP 抽取从 byte 2 起（NAL header 不参与 emulation prevention 规则）。
        extract_rbsp(ebsp.subspan(2), scratch_rbsp);
        cb(hdr, std::span<const uint8_t>(scratch_rbsp));

        i = next;
    }
}

}  // namespace mclc
