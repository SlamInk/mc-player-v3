/*
 * COM apartment 管理 — 每条线程一次性 CoInitializeEx + RAII 释放。
 *
 * Media Foundation 与 D3D11/DXGI 大量场景要求 MTA（multi-threaded apartment）；
 * UI 线程通常 STA（在 App 侧由 host 负责）。
 *
 * mc-player 内部线程统一 MTA。
 */

#ifndef MC_PLAYER_PAL_COM_APARTMENT_H_
#define MC_PLAYER_PAL_COM_APARTMENT_H_

#include "mc-player/mc_player_types.h"

namespace mcp::pal {

class ComApartment {
public:
    enum class Model {
        mta,    // Multi-threaded — mc-player 默认
        sta,    // Single-threaded — 仅 caller-driven UI 集成
    };

    explicit ComApartment(Model m = Model::mta) noexcept;
    ~ComApartment() noexcept;

    ComApartment(const ComApartment&)            = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ComApartment(ComApartment&&)                 = delete;
    ComApartment& operator=(ComApartment&&)      = delete;

    /// CoInitializeEx 是否成功（true = 当前线程持有此 init；false = 已被外部初始化或失败）。
    [[nodiscard]] bool owns_init() const noexcept { return owns_init_; }

private:
    bool owns_init_ = false;
};

}  // namespace mcp::pal

#endif  // MC_PLAYER_PAL_COM_APARTMENT_H_
