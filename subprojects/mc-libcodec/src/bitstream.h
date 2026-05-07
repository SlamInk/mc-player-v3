/*
 * Bit reader — Annex-B RBSP 比特流读取（ITU-T H.265 §B.2 / §9.2）。
 *
 * 范围只覆盖 v1 路径：
 *   - 顺序读 N bit（unsigned）
 *   - Exp-Golomb 无符号 ue(v) / 有符号 se(v)（§9.2.2）
 *   - 字节对齐 / RBSP trailing 检测
 *
 * 不做：CABAC（在 cabac.h），不读完整 NAL（caller 先剔 emulation prevention）。
 */

#ifndef MC_LIBCODEC_BITSTREAM_H_
#define MC_LIBCODEC_BITSTREAM_H_

#include <cstdint>
#include <cstddef>

namespace mclc {

class BitReader {
public:
    BitReader(const uint8_t* data, std::size_t size_bytes) noexcept
        : data_{data}, size_bits_{size_bytes * 8u} {}

    /// 读 n bit 无符号（n ≤ 32）。越界返 0 并设 bad_。
    uint32_t read_bits(uint32_t n) noexcept;

    /// peek n bit 不消耗。
    uint32_t peek_bits(uint32_t n) const noexcept;

    /// 单 bit。
    bool read_bit1() noexcept { return read_bits(1) != 0; }

    /// Exp-Golomb 无符号 ue(v)（§9.2.2.1）。越界返 0 并设 bad_。
    uint32_t read_ue() noexcept;

    /// Exp-Golomb 有符号 se(v)（§9.2.2.2）：先 ue，再 ((-1)^(k+1)) * ceil(k/2)。
    int32_t  read_se() noexcept;

    /// 跳 n bit（不读取）。
    void skip_bits(uint32_t n) noexcept;

    /// 字节对齐到下一字节（§9.2.1.1）；不读 byte。
    void byte_align() noexcept;

    [[nodiscard]] std::size_t bits_consumed() const noexcept { return pos_bits_; }
    [[nodiscard]] std::size_t bits_remaining() const noexcept {
        return pos_bits_ <= size_bits_ ? size_bits_ - pos_bits_ : 0;
    }

    [[nodiscard]] bool bad() const noexcept { return bad_; }
    /// 显式置 bad,语义解析(syntax check)失败时由 caller 调用,与 IO 越界一致归类。
    void               set_bad() noexcept { bad_ = true; }
    [[nodiscard]] bool more_rbsp_data() const noexcept;

private:
    const uint8_t* data_;
    std::size_t    size_bits_;
    std::size_t    pos_bits_{0};
    bool           bad_{false};
};

}  // namespace mclc

#endif  // MC_LIBCODEC_BITSTREAM_H_
