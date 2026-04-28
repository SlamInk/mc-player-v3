#include "media/nack_module.h"

namespace mcp::media {

NackModule::NackModule(SendNackFn nack, SendPliFn pli) noexcept
    : send_nack_{std::move(nack)}, send_pli_{std::move(pli)} {}

void NackModule::on_received(uint16_t seq) noexcept {
    if (last_seq_ < 0) { last_seq_ = seq; return; }
    // TODO: 16-bit wrap-aware gap 检测；缺失序号入 pending_。
    last_seq_ = seq;
}

void NackModule::on_recovered(uint16_t seq) noexcept {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                   [seq](const NackEntry& e) { return e.seq == seq; }),
                   pending_.end());
}

void NackModule::tick(int64_t /*now_us*/, uint32_t /*rtt_us*/) noexcept {
    // TODO: 按 rtt × backoff 重发；满 10 次 → send_pli_()。
}

void NackModule::reset() noexcept {
    last_seq_ = -1;
    pending_.clear();
}

}  // namespace mcp::media
