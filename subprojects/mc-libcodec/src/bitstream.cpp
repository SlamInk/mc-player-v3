#include "bitstream.h"

namespace mclc {

uint32_t BitReader::read_bits(uint32_t n) noexcept {
    if (n == 0) return 0;
    if (n > 32 || pos_bits_ + n > size_bits_) {
        bad_ = true;
        return 0;
    }
    uint32_t value = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const std::size_t byte_idx = pos_bits_ >> 3;
        const uint32_t    bit_idx  = 7u - (static_cast<uint32_t>(pos_bits_) & 7u);
        const uint32_t    bit      = (data_[byte_idx] >> bit_idx) & 1u;
        value = (value << 1) | bit;
        ++pos_bits_;
    }
    return value;
}

uint32_t BitReader::peek_bits(uint32_t n) const noexcept {
    if (n == 0) return 0;
    if (n > 32 || pos_bits_ + n > size_bits_) return 0;
    uint32_t    value = 0;
    std::size_t pos   = pos_bits_;
    for (uint32_t i = 0; i < n; ++i) {
        const std::size_t byte_idx = pos >> 3;
        const uint32_t    bit_idx  = 7u - (static_cast<uint32_t>(pos) & 7u);
        const uint32_t    bit      = (data_[byte_idx] >> bit_idx) & 1u;
        value = (value << 1) | bit;
        ++pos;
    }
    return value;
}

void BitReader::skip_bits(uint32_t n) noexcept {
    if (pos_bits_ + n > size_bits_) {
        bad_ = true;
        pos_bits_ = size_bits_;
        return;
    }
    pos_bits_ += n;
}

void BitReader::byte_align() noexcept {
    pos_bits_ = (pos_bits_ + 7) & ~static_cast<std::size_t>(7);
    if (pos_bits_ > size_bits_) {
        bad_     = true;
        pos_bits_ = size_bits_;
    }
}

uint32_t BitReader::read_ue() noexcept {
    // 数前导零 bit 数 leading_zeros，然后读 leading_zeros + 1 个 bit 得到 codeNum + 1。
    // codeNum = (1 << leading_zeros) - 1 + read_bits(leading_zeros)。
    uint32_t leading_zeros = 0;
    while (leading_zeros < 32 && pos_bits_ < size_bits_) {
        if (read_bits(1) == 1u) break;
        ++leading_zeros;
    }
    if (bad_ || leading_zeros >= 32) {
        bad_ = true;
        return 0;
    }
    const uint32_t suffix = leading_zeros == 0 ? 0u : read_bits(leading_zeros);
    if (bad_) return 0;
    return (1u << leading_zeros) - 1u + suffix;
}

int32_t BitReader::read_se() noexcept {
    const uint32_t k = read_ue();
    if (bad_) return 0;
    // mapping：k=0→0；k 奇 → +(k+1)/2；k 偶 → -(k/2)。
    if ((k & 1u) == 1u) return static_cast<int32_t>((k + 1u) >> 1);
    return -static_cast<int32_t>(k >> 1);
}

bool BitReader::more_rbsp_data() const noexcept {
    // §7.2: more_rbsp_data() 为真当 RBSP trailing bit 之后仍有数据。
    // 简化判断：仍有 bit 可读 且 不正好处于 trailing pattern (0b1 + 0..7 个 0)。
    if (pos_bits_ >= size_bits_) return false;

    // 探测末尾 trailing pattern。
    std::size_t last_one_bit = size_bits_;
    while (last_one_bit > pos_bits_) {
        --last_one_bit;
        const std::size_t byte_idx = last_one_bit >> 3;
        const uint32_t    bit_idx  = 7u - (static_cast<uint32_t>(last_one_bit) & 7u);
        if (((data_[byte_idx] >> bit_idx) & 1u) != 0u) break;
    }
    return last_one_bit > pos_bits_;
}

}  // namespace mclc
