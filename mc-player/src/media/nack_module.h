/*
 * NACK 调度 — Chromium nack_module2 主线模型（ADD §5.3.3）。
 *
 *   - 收到 RTP，比对期望 seq；缺失加入 nack_list。
 *   - 首次立即触发；后续按 rtt × backoff_factor 指数退避。
 *   - 重发上限 10 次（kMaxNackRetries），超出放弃 → 升级 PLI。
 */

#ifndef MC_PLAYER_MEDIA_NACK_MODULE_H_
#define MC_PLAYER_MEDIA_NACK_MODULE_H_

#include <cstdint>
#include <functional>
#include <vector>

namespace mcp::media {

struct NackEntry {
    uint16_t seq;
    uint8_t  retries;
    int64_t  next_send_us;
};

class NackModule {
public:
    using SendNackFn = std::function<void(uint16_t pid, uint16_t blp)>;
    using SendPliFn  = std::function<void()>;

    NackModule(SendNackFn nack, SendPliFn pli) noexcept;

    void on_received(uint16_t seq) noexcept;
    void on_recovered(uint16_t seq) noexcept;       // 重传到达，删除 entry
    void tick(int64_t now_us, uint32_t rtt_us) noexcept;

    void reset() noexcept;

private:
    SendNackFn              send_nack_;
    SendPliFn               send_pli_;
    int64_t                 last_seq_       = -1;
    std::vector<NackEntry>  pending_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_NACK_MODULE_H_
