#include "media/ui_overlay.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <mutex>

#include "pal/clock.h"
#include "pal/log.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace mcp::media {

namespace {

// 字体回退策略（direction-c.jsx 用 Inter / JetBrains Mono / SF Mono；Win11 LTSC 自带替代）：
//   sans → Segoe UI Variable Display → fallback Segoe UI
//   mono → Cascadia Mono → fallback Consolas
constexpr wchar_t kFontSans[]    = L"Segoe UI Variable Display";
constexpr wchar_t kFontSansAlt[] = L"Segoe UI";
constexpr wchar_t kFontMono[]    = L"Cascadia Mono";
constexpr wchar_t kFontMonoAlt[] = L"Consolas";

// 对齐 direction-c.jsx 的 C 调色板。
constexpr D2D1_COLOR_F kColorBg          = { 0.0f, 0.0f, 0.0f, 1.0f };
constexpr D2D1_COLOR_F kColorBgSoft      = { 0.039f, 0.039f, 0.039f, 1.0f };
constexpr D2D1_COLOR_F kColorText        = { 1.0f, 1.0f, 1.0f, 1.0f };
constexpr D2D1_COLOR_F kColorDim         = { 1.0f, 1.0f, 1.0f, 0.55f };
constexpr D2D1_COLOR_F kColorFaint       = { 1.0f, 1.0f, 1.0f, 0.28f };
constexpr D2D1_COLOR_F kColorGhost       = { 1.0f, 1.0f, 1.0f, 0.12f };
constexpr D2D1_COLOR_F kColorBorder      = { 1.0f, 1.0f, 1.0f, 0.08f };
constexpr D2D1_COLOR_F kColorBorderStrong= { 1.0f, 1.0f, 1.0f, 0.16f };
constexpr D2D1_COLOR_F kColorPanelBg     = { 0.0f, 0.0f, 0.0f, 0.78f };  // stats drawer / addModal
constexpr D2D1_COLOR_F kColorOverlayBg   = { 0.0f, 0.0f, 0.0f, 0.65f };  // addModal scrim

constexpr float kPi = 3.14159265358979323846f;

HRESULT create_text_format(IDWriteFactory* dw, const wchar_t* primary, const wchar_t* fallback,
                            float size, DWRITE_FONT_WEIGHT weight,
                            DWRITE_TEXT_ALIGNMENT halign, DWRITE_PARAGRAPH_ALIGNMENT valign,
                            ComPtr<IDWriteTextFormat>& out) noexcept {
    HRESULT hr = dw->CreateTextFormat(primary, nullptr, weight,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                       size, L"zh-CN", &out);
    if (FAILED(hr) || !out) {
        hr = dw->CreateTextFormat(fallback, nullptr, weight,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   size, L"zh-CN", &out);
    }
    if (SUCCEEDED(hr) && out) {
        out->SetTextAlignment(halign);
        out->SetParagraphAlignment(valign);
    }
    return hr;
}

D2D1_COLOR_F mix_alpha(D2D1_COLOR_F c, float scale) noexcept {
    c.a = std::clamp(c.a * scale, 0.0f, 1.0f);
    return c;
}

}  // namespace

struct UiOverlay::Impl {
    Config                              cfg;

    ComPtr<ID2D1Factory1>               d2d_factory;
    ComPtr<IDWriteFactory>              dwrite_factory;

    // ── TextFormat 池（按尺寸 / 字重预创建，避免 per-frame 重建） ──
    ComPtr<IDWriteTextFormat>           tf_wordmark;        // 72px Sans Thin    centered
    ComPtr<IDWriteTextFormat>           tf_ghost80;         // 280px Mono Thin   centered (CEmpty 背景)
    ComPtr<IDWriteTextFormat>           tf_elapsed;         // 96px  Mono Thin   centered (CConnecting)
    ComPtr<IDWriteTextFormat>           tf_latency_hud;     // 34px  Mono Thin   leading  (CPlaying top-left)
    ComPtr<IDWriteTextFormat>           tf_latency_drawer;  // 44px  Mono Thin   leading  (stats drawer 大数字)
    ComPtr<IDWriteTextFormat>           tf_modal_title;     // 18px  Sans Light  leading
    ComPtr<IDWriteTextFormat>           tf_modal_field;     // 13px  Sans        leading
    ComPtr<IDWriteTextFormat>           tf_url_mono;        // 13px  Mono        leading
    ComPtr<IDWriteTextFormat>           tf_subtitle;        // 11px  Sans 400    centered
    ComPtr<IDWriteTextFormat>           tf_subtitle_left;   // 11px  Sans 400    leading
    ComPtr<IDWriteTextFormat>           tf_caps10;          // 10px  Mono caps   leading
    ComPtr<IDWriteTextFormat>           tf_caps10_center;   // 10px  Mono caps   centered
    ComPtr<IDWriteTextFormat>           tf_caps10_right;    // 10px  Mono caps   trailing
    ComPtr<IDWriteTextFormat>           tf_caps9;           // 9px   Mono caps   leading
    ComPtr<IDWriteTextFormat>           tf_caps11_button;   // 11px  Mono bold caps centered (按钮)
    ComPtr<IDWriteTextFormat>           tf_step;            // 11px  Mono        leading

    // ── bind 阶段持有 ──
    ComPtr<ID2D1RenderTarget>           rt;
    UINT                                rt_width  = 0;
    UINT                                rt_height = 0;

    // 静态 brushes（与 RT 生命周期绑定）
    ComPtr<ID2D1SolidColorBrush>        br_bg;
    ComPtr<ID2D1SolidColorBrush>        br_bgsoft;
    ComPtr<ID2D1SolidColorBrush>        br_text;
    ComPtr<ID2D1SolidColorBrush>        br_dim;
    ComPtr<ID2D1SolidColorBrush>        br_faint;
    ComPtr<ID2D1SolidColorBrush>        br_ghost;
    ComPtr<ID2D1SolidColorBrush>        br_border;
    ComPtr<ID2D1SolidColorBrush>        br_border_strong;
    ComPtr<ID2D1SolidColorBrush>        br_panel_bg;
    ComPtr<ID2D1SolidColorBrush>        br_overlay_bg;

    // ── 状态（跨线程）──
    mutable std::mutex                  state_mu;
    UiStage                             stage              = UiStage::empty;
    bool                                show_stats         = false;
    bool                                show_add_modal     = false;
    std::wstring                        url;
    std::wstring                        err_code           = L"CONNECTION_TIMEOUT";
    std::wstring                        err_detail         = L"";
    std::wstring                        err_url            = L"";
    mc_stats_t                          stats              {};
    mc_stream_info_t                    stream_info        {};
    bool                                stream_info_set    = false;

    // ── 时间基（动画相位）──
    int64_t                             start_time_ns      = 0;
    int64_t                             stage_enter_time_ns= 0;
    std::atomic<int64_t>                now_ns             {0};

    HRESULT create_factories() noexcept {
        D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
        opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                          __uuidof(ID2D1Factory1),
                                          &opts,
                                          reinterpret_cast<void**>(d2d_factory.GetAddressOf()));
        if (FAILED(hr)) return hr;
        return ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                      __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(dwrite_factory.GetAddressOf()));
    }

    HRESULT create_text_formats() noexcept {
        const auto C = DWRITE_TEXT_ALIGNMENT_CENTER;
        const auto L = DWRITE_TEXT_ALIGNMENT_LEADING;
        const auto R = DWRITE_TEXT_ALIGNMENT_TRAILING;
        const auto VC = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;

        HRESULT hr = S_OK;
#define MK(out, primary, alt, size, weight, halign, valign) \
        do { hr = create_text_format(dwrite_factory.Get(), primary, alt, size, weight, halign, valign, out); \
             if (FAILED(hr)) return hr; } while(0)

        MK(tf_wordmark,        kFontSans, kFontSansAlt,  72, DWRITE_FONT_WEIGHT_THIN,        C,  VC);
        MK(tf_ghost80,         kFontMono, kFontMonoAlt, 280, DWRITE_FONT_WEIGHT_THIN,        C,  VC);
        MK(tf_elapsed,         kFontMono, kFontMonoAlt,  96, DWRITE_FONT_WEIGHT_THIN,        C,  VC);
        MK(tf_latency_hud,     kFontMono, kFontMonoAlt,  34, DWRITE_FONT_WEIGHT_LIGHT,       L,  VC);
        MK(tf_latency_drawer,  kFontMono, kFontMonoAlt,  44, DWRITE_FONT_WEIGHT_THIN,        L,  VC);
        MK(tf_modal_title,     kFontSans, kFontSansAlt,  18, DWRITE_FONT_WEIGHT_LIGHT,       L,  VC);
        MK(tf_modal_field,     kFontSans, kFontSansAlt,  13, DWRITE_FONT_WEIGHT_NORMAL,      L,  VC);
        MK(tf_url_mono,        kFontMono, kFontMonoAlt,  13, DWRITE_FONT_WEIGHT_NORMAL,      L,  VC);
        MK(tf_subtitle,        kFontSans, kFontSansAlt,  11, DWRITE_FONT_WEIGHT_NORMAL,      C,  VC);
        MK(tf_subtitle_left,   kFontSans, kFontSansAlt,  11, DWRITE_FONT_WEIGHT_NORMAL,      L,  VC);
        MK(tf_caps10,          kFontMono, kFontMonoAlt,  10, DWRITE_FONT_WEIGHT_NORMAL,      L,  VC);
        MK(tf_caps10_center,   kFontMono, kFontMonoAlt,  10, DWRITE_FONT_WEIGHT_NORMAL,      C,  VC);
        MK(tf_caps10_right,    kFontMono, kFontMonoAlt,  10, DWRITE_FONT_WEIGHT_NORMAL,      R,  VC);
        MK(tf_caps9,           kFontMono, kFontMonoAlt,   9, DWRITE_FONT_WEIGHT_NORMAL,      L,  VC);
        MK(tf_caps11_button,   kFontMono, kFontMonoAlt,  11, DWRITE_FONT_WEIGHT_BOLD,        C,  VC);
        MK(tf_step,            kFontMono, kFontMonoAlt,  11, DWRITE_FONT_WEIGHT_NORMAL,      L,  VC);
#undef MK
        return S_OK;
    }

    HRESULT create_brushes() noexcept {
        if (!rt) return E_FAIL;
        rt->CreateSolidColorBrush(kColorBg,           &br_bg);
        rt->CreateSolidColorBrush(kColorBgSoft,       &br_bgsoft);
        rt->CreateSolidColorBrush(kColorText,         &br_text);
        rt->CreateSolidColorBrush(kColorDim,          &br_dim);
        rt->CreateSolidColorBrush(kColorFaint,        &br_faint);
        rt->CreateSolidColorBrush(kColorGhost,        &br_ghost);
        rt->CreateSolidColorBrush(kColorBorder,       &br_border);
        rt->CreateSolidColorBrush(kColorBorderStrong, &br_border_strong);
        rt->CreateSolidColorBrush(kColorPanelBg,      &br_panel_bg);
        rt->CreateSolidColorBrush(kColorOverlayBg,    &br_overlay_bg);
        return (br_text && br_dim && br_faint && br_border) ? S_OK : E_FAIL;
    }

    void release_brushes() noexcept {
        br_bg.Reset(); br_bgsoft.Reset();
        br_text.Reset(); br_dim.Reset(); br_faint.Reset(); br_ghost.Reset();
        br_border.Reset(); br_border_strong.Reset();
        br_panel_bg.Reset(); br_overlay_bg.Reset();
    }

    // ── 时间基 helper ─────────────────────────────────────────
    float elapsed_ms_total() const noexcept {
        return static_cast<float>((now_ns.load(std::memory_order_relaxed) - start_time_ns) / 1'000'000);
    }
    float elapsed_ms_in_stage() const noexcept {
        return static_cast<float>((now_ns.load(std::memory_order_relaxed) - stage_enter_time_ns) / 1'000'000);
    }
    bool blink_on(float period_ms = 1000.0f) const noexcept {
        const float t = std::fmod(elapsed_ms_total(), period_ms);
        return t < period_ms * 0.5f;
    }
    float breathe_alpha(float period_ms = 6000.0f, float min_a = 0.85f, float max_a = 1.0f) const noexcept {
        const float t = elapsed_ms_total() / period_ms * 2.0f * kPi;
        return min_a + (max_a - min_a) * (0.5f + 0.5f * std::sin(t));
    }

    // ── 共享原语 ────────────────────────────────────────────────────────

    // CShell 的 halo：3 层 RadialGradientBrush 叠加，alpha 由 breathe 调制。
    // origin: 'bottom' (y >= 1.0)、'center' (y == 0.5)、'top' (y <= 0)
    enum class HaloOrigin { bottom, center, top };
    void draw_shell_halo(HaloOrigin origin, float glow) noexcept {
        if (!rt || !d2d_factory) return;
        const float w = static_cast<float>(rt_width);
        const float h = static_cast<float>(rt_height);
        const float modulate = breathe_alpha();

        struct Layer { float cx_frac, cy_frac, rx_frac, ry_frac, alpha; };
        Layer layers[3]{};
        if (origin == HaloOrigin::bottom) {
            layers[0] = { 0.5f, 1.05f, 0.45f, 0.275f, 0.22f };
            layers[1] = { 0.5f, 1.00f, 0.275f, 0.175f, 0.35f };
            layers[2] = { 0.5f, 0.98f, 0.15f, 0.10f, 0.50f };
        } else if (origin == HaloOrigin::center) {
            layers[0] = { 0.5f, 0.5f, 0.35f, 0.275f, 0.18f };
            layers[1] = { 0.5f, 0.5f, 0.175f, 0.14f, 0.30f };
            layers[2] = { 0.5f, 0.5f, 0.0f, 0.0f, 0.0f };  // 仅 2 层
        } else {
            layers[0] = { 0.5f, -0.05f, 0.45f, 0.25f, 0.22f };
            layers[1] = { 0.5f, 0.0f, 0.275f, 0.16f, 0.40f };
            layers[2] = { 0.5f, 0.0f, 0.0f, 0.0f, 0.0f };
        }

        for (const auto& L : layers) {
            if (L.alpha <= 0.0f || L.rx_frac <= 0.0f) continue;
            const D2D1_POINT_2F center = D2D1::Point2F(w * L.cx_frac, h * L.cy_frac);
            const float rx = w * L.rx_frac;
            const float ry = h * L.ry_frac;
            const float a  = std::clamp(L.alpha * glow * modulate, 0.0f, 1.0f);

            D2D1_GRADIENT_STOP gs[2] = {
                { 0.0f, D2D1::ColorF(1, 1, 1, a) },
                { 1.0f, D2D1::ColorF(1, 1, 1, 0) },
            };
            ComPtr<ID2D1GradientStopCollection> stops;
            if (FAILED(rt->CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2,
                        D2D1_EXTEND_MODE_CLAMP, &stops))) continue;
            D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES props =
                D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0, 0), rx, ry);
            ComPtr<ID2D1RadialGradientBrush> brush;
            if (FAILED(rt->CreateRadialGradientBrush(props, stops.Get(), &brush))) continue;

            // 椭圆覆盖区域填充——以 halo 椭圆 bounding box 为限，避免全屏重 fill
            const D2D1_ELLIPSE el = D2D1::Ellipse(center, rx, ry);
            rt->FillEllipse(el, brush.Get());
        }

        // 底部 light-seam（仅 bottom origin 时）
        if (origin == HaloOrigin::bottom) {
            // 横向白色渐变 1px 高
            D2D1_GRADIENT_STOP seam_stops[3] = {
                { 0.0f, D2D1::ColorF(1, 1, 1, 0) },
                { 0.5f, D2D1::ColorF(1, 1, 1, 0.5f * glow) },
                { 1.0f, D2D1::ColorF(1, 1, 1, 0) },
            };
            ComPtr<ID2D1GradientStopCollection> seam_stops_coll;
            if (SUCCEEDED(rt->CreateGradientStopCollection(seam_stops, 3, D2D1_GAMMA_2_2,
                        D2D1_EXTEND_MODE_CLAMP, &seam_stops_coll))) {
                D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp =
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(0, h - 1), D2D1::Point2F(w, h - 1));
                ComPtr<ID2D1LinearGradientBrush> seam;
                if (SUCCEEDED(rt->CreateLinearGradientBrush(lp, seam_stops_coll.Get(), &seam))) {
                    rt->FillRectangle(D2D1::RectF(0, h - 1, w, h), seam.Get());
                }
            }
        }
    }

    void draw_corner_ticks() noexcept {
        const float w = static_cast<float>(rt_width);
        const float h = static_cast<float>(rt_height);
        constexpr float kInset = 16.0f;
        constexpr float kLen   = 10.0f;
        const auto draw_corner = [&](float x, float y, float dx, float dy) {
            rt->FillRectangle(D2D1::RectF(std::min(x, x + dx*kLen), y - 0.5f,
                                           std::max(x, x + dx*kLen), y + 0.5f),
                               br_ghost.Get());
            rt->FillRectangle(D2D1::RectF(x - 0.5f, std::min(y, y + dy*kLen),
                                           x + 0.5f, std::max(y, y + dy*kLen)),
                               br_ghost.Get());
        };
        draw_corner(kInset,        kInset,        +1, +1);
        draw_corner(w - kInset,    kInset,        -1, +1);
        draw_corner(kInset,        h - kInset,    +1, -1);
        draw_corner(w - kInset,    h - kInset,    -1, -1);
    }

    // 视频区域内四角的 framing marks（CPlaying 视频 frame 内）。14×14 px L 形，opacity .7
    void draw_video_frame_marks(const D2D1_RECT_F& region) noexcept {
        constexpr float kInset = 10.0f;
        constexpr float kLen   = 14.0f;
        ComPtr<ID2D1SolidColorBrush> br;
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.7f), &br);
        if (!br) return;
        const auto mark = [&](float x, float y, float dx, float dy) {
            rt->FillRectangle(D2D1::RectF(std::min(x, x + dx*kLen), y - 0.5f,
                                           std::max(x, x + dx*kLen), y + 0.5f), br.Get());
            rt->FillRectangle(D2D1::RectF(x - 0.5f, std::min(y, y + dy*kLen),
                                           x + 0.5f, std::max(y, y + dy*kLen)), br.Get());
        };
        mark(region.left  + kInset, region.top    + kInset, +1, +1);
        mark(region.right - kInset, region.top    + kInset, -1, +1);
        mark(region.left  + kInset, region.bottom - kInset, +1, -1);
        mark(region.right - kInset, region.bottom - kInset, -1, -1);
    }

    // CUrlBar — 4 态共享。绘制坐标返回该 bar 的 D2D1_RECT_F（外部用以避免遮挡）。
    // state: idle / connecting / playing / error
    enum class UrlState { idle, connecting, playing, error };
    D2D1_RECT_F draw_url_bar(UrlState ust, const std::wstring& url_value, bool show_caret) noexcept {
        const float w = static_cast<float>(rt_width);
        const float bar_w = std::min(760.0f, w - 64.0f);
        const float bar_x = (w - bar_w) * 0.5f;
        const float bar_y = 68.0f;
        const float bar_h = 52.0f;
        const D2D1_RECT_F rect = D2D1::RectF(bar_x, bar_y, bar_x + bar_w, bar_y + bar_h);

        // 背景 + 边框
        rt->FillRectangle(rect, br_bgsoft.Get());
        ID2D1Brush* bb = (ust == UrlState::playing) ? br_border_strong.Get() : br_border.Get();
        rt->DrawRectangle(rect, bb, 1.0f);

        // playing 时下边光带
        if (ust == UrlState::playing) {
            D2D1_GRADIENT_STOP s[3] = {
                { 0.0f, D2D1::ColorF(1, 1, 1, 0) },
                { 0.5f, D2D1::ColorF(1, 1, 1, 0.9f) },
                { 1.0f, D2D1::ColorF(1, 1, 1, 0) },
            };
            ComPtr<ID2D1GradientStopCollection> sc;
            if (SUCCEEDED(rt->CreateGradientStopCollection(s, 3, D2D1_GAMMA_2_2,
                        D2D1_EXTEND_MODE_CLAMP, &sc))) {
                D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp =
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(rect.left + bar_w * 0.08f, rect.bottom),
                        D2D1::Point2F(rect.right - bar_w * 0.08f, rect.bottom));
                ComPtr<ID2D1LinearGradientBrush> seam;
                if (SUCCEEDED(rt->CreateLinearGradientBrush(lp, sc.Get(), &seam))) {
                    rt->FillRectangle(D2D1::RectF(rect.left + bar_w * 0.08f, rect.bottom,
                                                    rect.right - bar_w * 0.08f, rect.bottom + 1),
                                       seam.Get());
                }
            }
        }

        // 圆点指示器（左 padding = 20px）
        const D2D1_POINT_2F dot_c = D2D1::Point2F(rect.left + 20.0f + 4.0f, rect.top + bar_h * 0.5f);
        switch (ust) {
            case UrlState::playing: {
                rt->FillEllipse(D2D1::Ellipse(dot_c, 4, 4), br_text.Get());
                // 外圈呼吸光晕
                const float t = std::fmod(elapsed_ms_total(), 1400.0f) / 1400.0f;  // 0..1
                const float halo_r = 4.0f + t * 8.0f;
                ComPtr<ID2D1SolidColorBrush> halo;
                rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, (1.0f - t) * 0.35f), &halo);
                if (halo) rt->FillEllipse(D2D1::Ellipse(dot_c, halo_r, halo_r), halo.Get());
                break;
            }
            case UrlState::connecting: {
                // 旋转环
                ComPtr<ID2D1SolidColorBrush> ring;
                rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.5f), &ring);
                if (ring) rt->DrawEllipse(D2D1::Ellipse(dot_c, 4, 4), ring.Get(), 1.0f);
                // 旋转一段弧（用 2 段弧近似）
                const float ang = std::fmod(elapsed_ms_total(), 1400.0f) / 1400.0f * 2.0f * kPi;
                D2D1_POINT_2F arc_end = D2D1::Point2F(
                    dot_c.x + std::cos(ang) * 4.0f, dot_c.y + std::sin(ang) * 4.0f);
                rt->DrawLine(dot_c, arc_end, br_text.Get(), 1.5f);
                break;
            }
            case UrlState::error:
                rt->FillEllipse(D2D1::Ellipse(dot_c, 4, 4), br_text.Get());
                break;
            case UrlState::idle:
            default: {
                ComPtr<ID2D1SolidColorBrush> ring;
                rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.25f), &ring);
                if (ring) rt->DrawEllipse(D2D1::Ellipse(dot_c, 4, 4), ring.Get(), 1.0f);
                break;
            }
        }

        // 圆点 → 分隔细线（16px gap） → URL 文本
        const float sep_x = rect.left + 20.0f + 8.0f + 16.0f;
        rt->FillRectangle(D2D1::RectF(sep_x, rect.top + 17.0f, sep_x + 1.0f, rect.top + 35.0f),
                           br_ghost.Get());

        // URL 文本：rtsp:// (faint) + 余下 (text)；空值用 placeholder + 闪烁 caret
        const float text_x = sep_x + 16.0f;
        const D2D1_RECT_F text_area = D2D1::RectF(text_x, rect.top, rect.right - 100.0f, rect.bottom);
        if (url_value.empty()) {
            const wchar_t* ph = L"paste an rtsp url";
            rt->DrawText(ph, static_cast<UINT32>(wcslen(ph)),
                          tf_url_mono.Get(), text_area, br_faint.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_NONE);
            if (show_caret && blink_on(1000.0f)) {
                // caret 在 placeholder 末尾近似位置（不算字宽），简化：text_area.left + ~120px
                const float cx = text_area.left + 120.0f;
                rt->FillRectangle(D2D1::RectF(cx, rect.top + 18.0f, cx + 2.0f, rect.top + 34.0f),
                                   br_text.Get());
            }
        } else {
            // 优先识别 rtsp:// 前缀
            std::wstring scheme;
            std::wstring rest = url_value;
            const wchar_t* prefix = L"rtsp://";
            if (rest.rfind(prefix, 0) == 0) {
                scheme = prefix;
                rest   = rest.substr(7);
            }
            float cursor_x = text_x;
            if (!scheme.empty()) {
                rt->DrawText(scheme.c_str(), static_cast<UINT32>(scheme.size()),
                              tf_url_mono.Get(),
                              D2D1::RectF(cursor_x, rect.top, rect.right - 100.0f, rect.bottom),
                              br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                // 估算 scheme 宽度（mono 13px ≈ 7.8px / char）
                cursor_x += static_cast<float>(scheme.size()) * 7.8f;
            }
            if (!rest.empty()) {
                rt->DrawText(rest.c_str(), static_cast<UINT32>(rest.size()),
                              tf_url_mono.Get(),
                              D2D1::RectF(cursor_x, rect.top, rect.right - 100.0f, rect.bottom),
                              br_text.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }

        // 右侧 Enter (⏎) hint
        const float kbd_w = 28.0f, kbd_h = 22.0f;
        const float kbd_x = rect.right - 6.0f - 40.0f - 6.0f - kbd_w;
        const float kbd_y = rect.top + (bar_h - kbd_h) * 0.5f;
        const D2D1_RECT_F kbd_rect = D2D1::RectF(kbd_x, kbd_y, kbd_x + kbd_w, kbd_y + kbd_h);
        rt->DrawRectangle(kbd_rect, br_border.Get(), 1.0f);
        const wchar_t* enter_glyph = L"⏎";  // Return Symbol
        rt->DrawText(enter_glyph, 1, tf_caps10_center.Get(), kbd_rect, br_faint.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_NONE);

        // 最右：Play/Pause 按钮 40×40
        const float btn = 40.0f;
        const float btn_x = rect.right - 6.0f - btn;
        const float btn_y = rect.top + (bar_h - btn) * 0.5f;
        const D2D1_RECT_F btn_rect = D2D1::RectF(btn_x, btn_y, btn_x + btn, btn_y + btn);
        if (ust == UrlState::playing) {
            // 透明边框 + 白色 pause
            rt->DrawRectangle(btn_rect, br_border_strong.Get(), 1.0f);
            const float cx = btn_rect.left + btn * 0.5f;
            const float cy = btn_rect.top  + btn * 0.5f;
            rt->FillRectangle(D2D1::RectF(cx - 5.0f, cy - 6.0f, cx - 2.0f, cy + 6.0f), br_text.Get());
            rt->FillRectangle(D2D1::RectF(cx + 2.0f, cy - 6.0f, cx + 5.0f, cy + 6.0f), br_text.Get());
        } else {
            // 白底 play 三角
            rt->FillRectangle(btn_rect, br_text.Get());
            const float cx = btn_rect.left + btn * 0.5f;
            const float cy = btn_rect.top  + btn * 0.5f;
            // 用 PathGeometry 画三角
            ComPtr<ID2D1PathGeometry> tri;
            if (SUCCEEDED(d2d_factory->CreatePathGeometry(&tri))) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(tri->Open(&sink))) {
                    sink->BeginFigure(D2D1::Point2F(cx - 4.0f, cy - 6.0f),
                                       D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(cx + 5.0f, cy));
                    sink->AddLine(D2D1::Point2F(cx - 4.0f, cy + 6.0f));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    rt->FillGeometry(tri.Get(), br_bg.Get());
                }
            }
        }

        return rect;
    }

    // 按钮（按钮文字、大小、是否填充白底）
    void draw_button(const D2D1_RECT_F& r, const wchar_t* label, bool filled) noexcept {
        if (filled) {
            rt->FillRectangle(r, br_text.Get());
            rt->DrawText(label, static_cast<UINT32>(wcslen(label)),
                          tf_caps11_button.Get(), r, br_bg.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        } else {
            rt->DrawRectangle(r, br_border.Get(), 1.0f);
            rt->DrawText(label, static_cast<UINT32>(wcslen(label)),
                          tf_caps11_button.Get(), r, br_dim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }

    // ── stage 绘制 ─────────────────────────────────────────────────────

    void draw_stage_empty() noexcept {
        const float w = static_cast<float>(rt_width);
        const float h = static_cast<float>(rt_height);

        rt->FillRectangle(D2D1::RectF(0, 0, w, h), br_bg.Get());
        draw_shell_halo(HaloOrigin::bottom, 1.0f);
        draw_corner_ticks();
        draw_url_bar(UrlState::idle, L"", true);

        // 280px ghost "80" 居中（背景层）
        const D2D1_RECT_F ghost_rect = D2D1::RectF(0, h * 0.5f - 140.0f, w, h * 0.5f + 140.0f);
        rt->DrawText(L"80", 2, tf_ghost80.Get(), ghost_rect, br_ghost.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_NONE);

        // 72px wordmark "mc·player" + 副标题
        const D2D1_RECT_F wm_rect = D2D1::RectF(0, h * 0.5f - 52.0f, w, h * 0.5f + 24.0f);
        rt->DrawText(L"mc·player", 9, tf_wordmark.Get(), wm_rect, br_text.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_NONE);
        const D2D1_RECT_F sub_rect = D2D1::RectF(0, h * 0.5f + 30.0f, w, h * 0.5f + 50.0f);
        rt->DrawText(L"BUILT FOR 80–130 MILLISECONDS", 29, tf_subtitle.Get(), sub_rect,
                      br_dim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

        // 底部 spec rail
        constexpr float kRailMx = 40.0f, kRailMb = 30.0f, kRailRowH = 14.0f;
        const float rail_y = h - kRailMb;
        rt->FillRectangle(D2D1::RectF(kRailMx, rail_y - kRailRowH - 1.0f,
                                       w - kRailMx, rail_y - kRailRowH),
                           br_border.Get());
        struct R { const wchar_t* hi; const wchar_t* lo; };
        static const R items[] = {
            { L"TCP",     L" · NO BUFFER" },
            { L"DISCARD", L" LOOP FILTER" },
            { L"D3D11VA", L" HWACCEL" },
            { L"MFT",     L" H264/HEVC" },
            { L"WIN",     L" 10 / 11 X64" },
        };
        const float total_w = w - 2 * kRailMx;
        const float slot_w  = total_w / static_cast<float>(_countof(items));
        for (size_t i = 0; i < _countof(items); ++i) {
            const float x0 = kRailMx + slot_w * static_cast<float>(i);
            std::wstring all = std::wstring(items[i].hi) + items[i].lo;
            D2D1_RECT_F slot = D2D1::RectF(x0, rail_y - kRailRowH, x0 + slot_w, rail_y);
            rt->DrawText(all.c_str(), static_cast<UINT32>(all.size()),
                          tf_caps10.Get(), slot, br_faint.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_NONE);
            rt->DrawText(items[i].hi, static_cast<UINT32>(wcslen(items[i].hi)),
                          tf_caps10.Get(), slot, br_text.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }

    void draw_stage_connecting() noexcept {
        const float w = static_cast<float>(rt_width);
        const float h = static_cast<float>(rt_height);

        rt->FillRectangle(D2D1::RectF(0, 0, w, h), br_bg.Get());
        draw_shell_halo(HaloOrigin::center, 1.1f);
        draw_corner_ticks();
        draw_url_bar(UrlState::connecting, url, false);

        // 96px elapsed ms（0..999，由进入 stage 起算 elapsed）
        const float ems = std::clamp(elapsed_ms_in_stage(), 0.0f, 999.0f);
        wchar_t buf[16];
        std::swprintf(buf, _countof(buf), L"%03d", static_cast<int>(ems));
        const float ctr_y = h * 0.45f;
        // "HANDSHAKING" 标签
        const D2D1_RECT_F lbl_rect = D2D1::RectF(0, ctr_y - 60.0f, w, ctr_y - 40.0f);
        rt->DrawText(L"HANDSHAKING", 11, tf_caps10_center.Get(), lbl_rect, br_faint.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_NONE);
        // 大数字
        const D2D1_RECT_F num_rect = D2D1::RectF(0, ctr_y - 30.0f, w, ctr_y + 50.0f);
        rt->DrawText(buf, 3, tf_elapsed.Get(), num_rect, br_text.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_NONE);
        // " ms" 后缀（小字 22px 简化为同 caps10）
        const D2D1_RECT_F unit_rect = D2D1::RectF(w * 0.5f + 60.0f, ctr_y + 14.0f,
                                                    w * 0.5f + 120.0f, ctr_y + 36.0f);
        rt->DrawText(L"MS", 2, tf_caps10.Get(), unit_rect, br_faint.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_NONE);

        // 进度 hairline 440px
        const float bar_w = 440.0f;
        const float bar_x = (w - bar_w) * 0.5f;
        const float bar_y = ctr_y + 80.0f;
        rt->FillRectangle(D2D1::RectF(bar_x, bar_y, bar_x + bar_w, bar_y + 1), br_border.Get());
        const float prog = std::clamp(ems / 180.0f, 0.0f, 1.0f);
        rt->FillRectangle(D2D1::RectF(bar_x, bar_y, bar_x + bar_w * prog, bar_y + 1),
                           br_text.Get());
        // scan glow（左→右 1.4s 循环）
        const float sg_t = std::fmod(elapsed_ms_total(), 1400.0f) / 1400.0f;
        const float sg_x = bar_x + (bar_w - 60.0f) * sg_t;
        ComPtr<ID2D1SolidColorBrush> sg;
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.4f), &sg);
        if (sg) {
            rt->FillRectangle(D2D1::RectF(sg_x, bar_y - 5.0f, sg_x + 60.0f, bar_y + 6.0f), sg.Get());
        }

        // step log — 5 行 (440 width)
        struct Step { float t; const wchar_t* label; };
        static const Step steps[] = {
            { 12.0f,  L"dns · resolved" },
            { 40.0f,  L"tcp · handshake" },
            { 86.0f,  L"rtsp · describe" },
            { 130.0f, L"rtsp · setup / play" },
            { 180.0f, L"probe codec" },
        };
        const float row_h = 22.0f;
        const float log_y = bar_y + 32.0f;
        for (size_t i = 0; i < _countof(steps); ++i) {
            const float y = log_y + row_h * static_cast<float>(i);
            const bool done = ems > steps[i].t;
            // 时间数字（左 40px）
            wchar_t tbuf[8];
            std::swprintf(tbuf, _countof(tbuf), L"%03d", static_cast<int>(steps[i].t));
            rt->DrawText(tbuf, 3, tf_step.Get(),
                          D2D1::RectF(bar_x, y, bar_x + 40.0f, y + row_h),
                          br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            // 状态标记 ✓ / ·
            const wchar_t* mark = done ? L"✓" : L"·";
            rt->DrawText(mark, 1, tf_step.Get(),
                          D2D1::RectF(bar_x + 50.0f, y, bar_x + 70.0f, y + row_h),
                          done ? br_text.Get() : br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            // 标签
            rt->DrawText(steps[i].label, static_cast<UINT32>(wcslen(steps[i].label)),
                          tf_step.Get(),
                          D2D1::RectF(bar_x + 78.0f, y, bar_x + bar_w, y + row_h),
                          done ? br_text.Get() : br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }

    // playing stage — 含 stats drawer 时拐到 draw_stats_drawer
    void draw_stage_playing() noexcept {
        // 播放态：UI 让位于视频，不画任何 overlay。
        // 仅当用户显式开启 show_stats 时叠加右上 stats drawer。
        if (!show_stats) return;

        const float w = static_cast<float>(rt_width);
        const float h = static_cast<float>(rt_height);
        const float vid_l = 40.0f;
        const float vid_r = w - 40.0f;
        const float vid_t = 68.0f + 52.0f + 20.0f;
        const float vid_b = h - 40.0f;
        const D2D1_RECT_F vid_rect = D2D1::RectF(vid_l, vid_t, vid_r, vid_b);
        draw_stats_drawer(vid_rect);
    }

    void draw_stats_drawer(const D2D1_RECT_F& vid_rect) noexcept {
        const float drawer_w = 240.0f;
        const float drawer_h = 320.0f;
        const float x = vid_rect.right - drawer_w - 20.0f;
        const float y = vid_rect.top + 20.0f;
        const D2D1_RECT_F r = D2D1::RectF(x, y, x + drawer_w, y + drawer_h);

        rt->FillRectangle(r, br_panel_bg.Get());
        rt->DrawRectangle(r, br_border_strong.Get(), 1.0f);

        const float pad = 18.0f;

        // 头部 LATENCY + avg/p99
        rt->DrawText(L"LATENCY", 7, tf_caps9.Get(),
                      D2D1::RectF(r.left + pad, r.top + 12, r.left + pad + 100, r.top + 28),
                      br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        rt->DrawText(L"avg 94 · p99 118", 16, tf_caps9.Get(),
                      D2D1::RectF(r.right - pad - 100, r.top + 12, r.right - pad, r.top + 28),
                      br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        // 大数字 latency
        const int lat_ms = static_cast<int>(stats.end_to_end_latency_us / 1000);
        wchar_t buf[16];
        std::swprintf(buf, _countof(buf), L"%d", std::clamp(lat_ms, 0, 999));
        rt->DrawText(buf, static_cast<UINT32>(wcslen(buf)),
                      tf_latency_drawer.Get(),
                      D2D1::RectF(r.left + pad, r.top + 28, r.right - pad, r.top + 80),
                      br_text.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        rt->DrawText(L"MS", 2, tf_caps10.Get(),
                      D2D1::RectF(r.left + pad + 70, r.top + 56, r.left + pad + 110, r.top + 76),
                      br_dim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

        // 趋势线占位
        ComPtr<ID2D1SolidColorBrush> sp_br;
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.85f), &sp_br);
        if (sp_br) {
            const float sx0 = r.left + pad;
            const float sx1 = r.right - pad;
            const float sy  = r.top + 100;
            for (int i = 0; i < 59; ++i) {
                const float t = elapsed_ms_total() * 0.0008f + i * 0.25f;
                const float y0 = sy - std::sin(t) * 9.0f;
                const float y1 = sy - std::sin(t + 0.25f) * 9.0f;
                const float x0 = sx0 + i * (sx1 - sx0) / 59.0f;
                const float x1 = sx0 + (i + 1) * (sx1 - sx0) / 59.0f;
                rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), sp_br.Get(), 0.7f);
            }
        }

        // 分隔
        const float sep_y = r.top + 132;
        rt->FillRectangle(D2D1::RectF(r.left, sep_y, r.right, sep_y + 1), br_border.Get());

        // 7 行 row
        struct Row { const wchar_t* k; std::wstring v; };
        wchar_t fbuf[16]; wchar_t rbuf[24]; wchar_t bbuf[16]; wchar_t dbuf[16];
        std::swprintf(fbuf, _countof(fbuf), L"%.2f",
                       stream_info_set && stream_info.video_fps_den > 0
                           ? static_cast<float>(stream_info.video_fps_num) / stream_info.video_fps_den : 0.0f);
        std::swprintf(rbuf, _countof(rbuf), L"%u × %u",
                       stream_info_set ? stream_info.video_width  : 0,
                       stream_info_set ? stream_info.video_height : 0);
        std::swprintf(bbuf, _countof(bbuf), L"0.0");      // bitrate Slice 后续
        std::swprintf(dbuf, _countof(dbuf), L"%llu",
                       static_cast<unsigned long long>(stats.video_frames_dropped_post_decode));
        const wchar_t* codec_label = L"h.264";
        if (stream_info_set && stream_info.video_codec == MC_VIDEO_CODEC_H265) codec_label = L"hevc";
        const wchar_t* hw_label = L"d3d11va";
        if (stream_info_set && stream_info.video_decoder_kind == MC_DECODER_LIBCODEC) hw_label = L"libcodec";
        const Row rows[] = {
            { L"FPS",       fbuf },
            { L"RES",       rbuf },
            { L"CODEC",     codec_label },
            { L"HWACCEL",   hw_label },
            { L"BITRATE",   std::wstring(bbuf) + L" mbps" },
            { L"TRANSPORT", L"udp" },
            { L"DROPPED",   dbuf },
        };
        const float row_h = 24.0f;
        const float row_y0 = sep_y + 8.0f;
        for (size_t i = 0; i < _countof(rows); ++i) {
            const float ry = row_y0 + row_h * static_cast<float>(i);
            rt->DrawText(rows[i].k, static_cast<UINT32>(wcslen(rows[i].k)), tf_caps10.Get(),
                          D2D1::RectF(r.left + pad, ry, r.left + pad + 100, ry + row_h - 4),
                          br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            rt->DrawText(rows[i].v.c_str(), static_cast<UINT32>(rows[i].v.size()), tf_caps10_right.Get(),
                          D2D1::RectF(r.right - pad - 100, ry, r.right - pad, ry + row_h - 4),
                          br_text.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            rt->FillRectangle(D2D1::RectF(r.left + pad, ry + row_h - 4,
                                            r.right - pad, ry + row_h - 3),
                               br_border.Get());
        }
    }

    void draw_stage_error() noexcept {
        const float w = static_cast<float>(rt_width);
        const float h = static_cast<float>(rt_height);

        rt->FillRectangle(D2D1::RectF(0, 0, w, h), br_bg.Get());
        draw_shell_halo(HaloOrigin::center, 0.4f);
        draw_corner_ticks();
        draw_url_bar(UrlState::error, url, false);

        const float ctr_x = w * 0.5f;
        const float ctr_y = h * 0.5f - 30.0f;

        // 80×80 圆 + 内 X icon
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ctr_x, ctr_y), 40, 40),
                         br_border_strong.Get(), 1.0f);
        // X 30×30
        ComPtr<ID2D1SolidColorBrush> xbr;
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1.0f), &xbr);
        if (xbr) {
            rt->DrawLine(D2D1::Point2F(ctr_x - 9, ctr_y - 9),
                          D2D1::Point2F(ctr_x + 9, ctr_y + 9), xbr.Get(), 1.2f);
            rt->DrawLine(D2D1::Point2F(ctr_x + 9, ctr_y - 9),
                          D2D1::Point2F(ctr_x - 9, ctr_y + 9), xbr.Get(), 1.2f);
        }

        // CONNECTION_TIMEOUT 标题
        const D2D1_RECT_F title_rect = D2D1::RectF(0, ctr_y + 60, w, ctr_y + 84);
        rt->DrawText(err_code.c_str(), static_cast<UINT32>(err_code.size()),
                      tf_url_mono.Get(), title_rect, br_text.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_NONE);
        // 居中：临时切 alignment（tf_url_mono 是 leading）—— 改用 tf_subtitle 的 mono 等价物
        // 简化方案：直接让 subtitle 兜底
        // 子文 (detail + url)
        if (!err_detail.empty()) {
            rt->DrawText(err_detail.c_str(), static_cast<UINT32>(err_detail.size()),
                          tf_subtitle.Get(),
                          D2D1::RectF(0, ctr_y + 96, w, ctr_y + 116),
                          br_dim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        if (!err_url.empty()) {
            std::wstring slashed = L"// " + err_url;
            rt->DrawText(slashed.c_str(), static_cast<UINT32>(slashed.size()),
                          tf_subtitle.Get(),
                          D2D1::RectF(0, ctr_y + 116, w, ctr_y + 134),
                          br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        // 诊断三联 (host / port / auth)
        struct Diag { const wchar_t* key; const wchar_t* val; bool ok; };
        static const Diag diags[] = {
            { L"host", L"reachable",  true  },
            { L"port", L"554 open",   true  },
            { L"auth", L"unverified", false },
        };
        const float diag_y = ctr_y + 152;
        const float cell_w = 130.0f;
        const float diag_total_w = cell_w * 3.0f;
        const float diag_x0 = (w - diag_total_w) * 0.5f;
        for (size_t i = 0; i < _countof(diags); ++i) {
            const float cx = diag_x0 + cell_w * static_cast<float>(i);
            D2D1_RECT_F cell = D2D1::RectF(cx, diag_y, cx + cell_w, diag_y + 30);
            rt->DrawRectangle(cell, br_border.Get(), 1.0f);
            const wchar_t* mark_glyph = diags[i].ok ? L"✓" : L"✕";
            rt->DrawText(mark_glyph, 1, tf_caps10.Get(),
                          D2D1::RectF(cell.left + 10, cell.top, cell.left + 24, cell.bottom),
                          diags[i].ok ? br_text.Get() : br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            rt->DrawText(diags[i].key, static_cast<UINT32>(wcslen(diags[i].key)), tf_caps10.Get(),
                          D2D1::RectF(cell.left + 30, cell.top, cell.left + 60, cell.bottom),
                          br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            rt->DrawText(diags[i].val, static_cast<UINT32>(wcslen(diags[i].val)), tf_caps10.Get(),
                          D2D1::RectF(cell.left + 64, cell.top, cell.right - 8, cell.bottom),
                          diags[i].ok ? br_text.Get() : br_dim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        // 双按钮 RETRY (filled) + DIAGNOSTICS (outline)
        const float btn_y0 = diag_y + 50;
        const float btn_h = 32.0f;
        const D2D1_RECT_F btn_retry =
            D2D1::RectF(w * 0.5f - 116, btn_y0, w * 0.5f - 4, btn_y0 + btn_h);
        const D2D1_RECT_F btn_diag =
            D2D1::RectF(w * 0.5f + 4, btn_y0, w * 0.5f + 132, btn_y0 + btn_h);
        draw_button(btn_retry, L"RETRY", true);
        draw_button(btn_diag,  L"DIAGNOSTICS", false);
    }

    // addModal — 在 base stage 之上叠遮罩 + 模态
    void draw_stage_add_modal_overlay() noexcept {
        const float w = static_cast<float>(rt_width);
        const float h = static_cast<float>(rt_height);

        // 全屏遮罩
        rt->FillRectangle(D2D1::RectF(0, 0, w, h), br_overlay_bg.Get());

        const float modal_w = 480.0f;
        const float modal_h = 360.0f;
        const float mx = (w - modal_w) * 0.5f;
        const float my = (h - modal_h) * 0.5f;
        const D2D1_RECT_F m = D2D1::RectF(mx, my, mx + modal_w, my + modal_h);

        rt->FillRectangle(m, br_bg.Get());
        rt->DrawRectangle(m, br_border_strong.Get(), 1.0f);

        // 顶部 1px 白色光带
        D2D1_GRADIENT_STOP s[3] = {
            { 0.0f, D2D1::ColorF(1, 1, 1, 0) },
            { 0.5f, D2D1::ColorF(1, 1, 1, 1) },
            { 1.0f, D2D1::ColorF(1, 1, 1, 0) },
        };
        ComPtr<ID2D1GradientStopCollection> sc;
        if (SUCCEEDED(rt->CreateGradientStopCollection(s, 3, D2D1_GAMMA_2_2,
                    D2D1_EXTEND_MODE_CLAMP, &sc))) {
            D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp =
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(m.left, m.top), D2D1::Point2F(m.right, m.top));
            ComPtr<ID2D1LinearGradientBrush> lb;
            if (SUCCEEDED(rt->CreateLinearGradientBrush(lp, sc.Get(), &lb))) {
                rt->FillRectangle(D2D1::RectF(m.left, m.top, m.right, m.top + 1.0f), lb.Get());
            }
        }

        const float pad = 28.0f;
        // // new source
        rt->DrawText(L"// NEW SOURCE", 13, tf_caps10.Get(),
                      D2D1::RectF(m.left + pad, m.top + pad, m.right - pad, m.top + pad + 14),
                      br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        // 标题
        rt->DrawText(L"添加 RTSP 流", 8, tf_modal_title.Get(),
                      D2D1::RectF(m.left + pad, m.top + pad + 18, m.right - pad, m.top + pad + 50),
                      br_text.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

        // Name 字段
        const float field_y0 = m.top + pad + 70;
        rt->DrawText(L"NAME", 4, tf_caps9.Get(),
                      D2D1::RectF(m.left + pad, field_y0, m.left + pad + 80, field_y0 + 12),
                      br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        const D2D1_RECT_F name_rect =
            D2D1::RectF(m.left + pad, field_y0 + 16, m.right - pad, field_y0 + 54);
        rt->FillRectangle(name_rect, br_bgsoft.Get());
        rt->DrawRectangle(name_rect, br_border.Get(), 1.0f);
        rt->DrawText(L"前门摄像头 · 101", 9, tf_modal_field.Get(),
                      D2D1::RectF(name_rect.left + 12, name_rect.top, name_rect.right - 12,
                                   name_rect.bottom),
                      br_text.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

        // RTSP URL 字段
        const float url_y0 = field_y0 + 72;
        rt->DrawText(L"RTSP URL", 8, tf_caps9.Get(),
                      D2D1::RectF(m.left + pad, url_y0, m.left + pad + 80, url_y0 + 12),
                      br_faint.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        const D2D1_RECT_F url_rect =
            D2D1::RectF(m.left + pad, url_y0 + 16, m.right - pad, url_y0 + 54);
        rt->FillRectangle(url_rect, br_bgsoft.Get());
        rt->DrawRectangle(url_rect, br_border_strong.Get(), 1.0f);
        const wchar_t* placeholder_url =
            L"rtsp://admin:xxx@192.168.1.64:554/streaming/channels/101";
        rt->DrawText(placeholder_url, static_cast<UINT32>(wcslen(placeholder_url)),
                      tf_url_mono.Get(),
                      D2D1::RectF(url_rect.left + 12, url_rect.top, url_rect.right - 12,
                                   url_rect.bottom),
                      br_text.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        if (blink_on(1000.0f)) {
            // caret 在 URL 末尾近似
            const float cx = url_rect.left + 12 + 360.0f;
            rt->FillRectangle(D2D1::RectF(cx, url_rect.top + 12, cx + 1.5f, url_rect.bottom - 10),
                               br_text.Get());
        }

        // preflight 三行
        const float pre_y = url_y0 + 70;
        const D2D1_RECT_F pre_rect =
            D2D1::RectF(m.left + pad, pre_y, m.right - pad, pre_y + 64);
        rt->DrawRectangle(pre_rect, br_border.Get(), 1.0f);
        struct PL { const wchar_t* mark; const wchar_t* line; bool ok; };
        static const PL pl[] = {
            { L"✓", L"syntax ok", true },
            { L"✓", L"host reachable · ping 2ms", true },
            { L"·", L"transport = tcp · auth = basic", true },
        };
        for (size_t i = 0; i < _countof(pl); ++i) {
            const float py = pre_rect.top + 8 + 18.0f * static_cast<float>(i);
            rt->DrawText(pl[i].mark, 1, tf_step.Get(),
                          D2D1::RectF(pre_rect.left + 10, py, pre_rect.left + 26, py + 16),
                          pl[i].ok ? br_text.Get() : br_faint.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_NONE);
            rt->DrawText(pl[i].line, static_cast<UINT32>(wcslen(pl[i].line)), tf_step.Get(),
                          D2D1::RectF(pre_rect.left + 30, py, pre_rect.right - 10, py + 16),
                          br_dim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        // 双按钮 取消 / 保存并播放（右下）
        const float btn_y = m.bottom - pad - 32;
        const float btn_h = 32.0f;
        const D2D1_RECT_F btn_save =
            D2D1::RectF(m.right - pad - 130, btn_y, m.right - pad, btn_y + btn_h);
        const D2D1_RECT_F btn_cancel =
            D2D1::RectF(btn_save.left - 10 - 70, btn_y, btn_save.left - 10, btn_y + btn_h);
        draw_button(btn_cancel, L"取消", false);
        draw_button(btn_save,   L"保存并播放", true);
    }

    // ── 顶层调度 ──
    void draw_current_locked() noexcept {
        const D2D1_SIZE_F sz = rt->GetSize();
        rt_width  = static_cast<UINT>(sz.width);
        rt_height = static_cast<UINT>(sz.height);

        switch (stage) {
            case UiStage::empty:        draw_stage_empty();       break;
            case UiStage::connecting:   draw_stage_connecting();  break;
            case UiStage::playing:      draw_stage_playing();     break;
            case UiStage::error:        draw_stage_error();       break;
            case UiStage::add_modal:    draw_stage_empty(); break;  // base
        }
        if (show_add_modal && stage != UiStage::add_modal) {
            // 主 stage 为非 add_modal 但 show_add_modal=true：叠加遮罩
            draw_stage_add_modal_overlay();
        } else if (stage == UiStage::add_modal) {
            draw_stage_add_modal_overlay();
        }
    }
};

// ── public API impl ────────────────────────────────────────────────

UiOverlay::UiOverlay(Config cfg) : impl_{std::make_unique<Impl>()} {
    impl_->cfg = std::move(cfg);
}

UiOverlay::~UiOverlay() {
    unbind_backbuffer();
}

HRESULT UiOverlay::initialize() noexcept {
    if (!impl_->cfg.device) return E_INVALIDARG;
    HRESULT hr = impl_->create_factories();
    if (FAILED(hr)) {
        MCP_LOGF(pal::LogLevel::error, "UiOverlay: factory create failed hr=0x%08lX", hr);
        return hr;
    }
    hr = impl_->create_text_formats();
    if (FAILED(hr)) {
        MCP_LOGF(pal::LogLevel::error, "UiOverlay: text formats failed hr=0x%08lX", hr);
        return hr;
    }
    impl_->start_time_ns       = pal::Clock::now_ns();
    impl_->stage_enter_time_ns = impl_->start_time_ns;
    impl_->now_ns.store(impl_->start_time_ns, std::memory_order_release);
    impl_->stats.struct_size      = sizeof(impl_->stats);
    impl_->stream_info.struct_size = sizeof(impl_->stream_info);
    return S_OK;
}

HRESULT UiOverlay::bind_backbuffer(ComPtr<ID3D11Texture2D> backbuffer) noexcept {
    if (!impl_->d2d_factory) return E_FAIL;
    if (!backbuffer) return E_INVALIDARG;
    unbind_backbuffer();

    ComPtr<IDXGISurface> surface;
    HRESULT hr = backbuffer.As(&surface);
    if (FAILED(hr)) return hr;
    D3D11_TEXTURE2D_DESC bb_desc{};
    backbuffer->GetDesc(&bb_desc);
    impl_->rt_width  = bb_desc.Width;
    impl_->rt_height = bb_desc.Height;

    D2D1_RENDER_TARGET_PROPERTIES props =
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_HARDWARE,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE));
    hr = impl_->d2d_factory->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &impl_->rt);
    if (FAILED(hr)) {
        MCP_LOGF(pal::LogLevel::error,
                 "UiOverlay: CreateDxgiSurfaceRenderTarget failed hr=0x%08lX", hr);
        return hr;
    }
    impl_->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    impl_->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    hr = impl_->create_brushes();
    if (FAILED(hr)) {
        impl_->rt.Reset();
        return hr;
    }
    return S_OK;
}

void UiOverlay::unbind_backbuffer() noexcept {
    if (!impl_) return;
    impl_->release_brushes();
    impl_->rt.Reset();
    impl_->rt_width = impl_->rt_height = 0;
}

void UiOverlay::set_stage(UiStage stage) noexcept {
    std::lock_guard<std::mutex> lk{impl_->state_mu};
    if (impl_->stage != stage) {
        impl_->stage = stage;
        impl_->stage_enter_time_ns = impl_->now_ns.load(std::memory_order_relaxed);
    }
}
void UiOverlay::set_show_stats(bool show) noexcept {
    std::lock_guard<std::mutex> lk{impl_->state_mu};
    impl_->show_stats = show;
}
void UiOverlay::set_show_add_modal(bool show) noexcept {
    std::lock_guard<std::mutex> lk{impl_->state_mu};
    impl_->show_add_modal = show;
}
void UiOverlay::set_url(std::wstring url) noexcept {
    std::lock_guard<std::mutex> lk{impl_->state_mu};
    impl_->url = std::move(url);
}
void UiOverlay::set_stats(const mc_stats_t& stats) noexcept {
    std::lock_guard<std::mutex> lk{impl_->state_mu};
    impl_->stats = stats;
}
void UiOverlay::set_stream_info(const mc_stream_info_t& info) noexcept {
    std::lock_guard<std::mutex> lk{impl_->state_mu};
    impl_->stream_info = info;
    impl_->stream_info_set = true;
}
void UiOverlay::set_error(std::wstring code, std::wstring detail, std::wstring url) noexcept {
    std::lock_guard<std::mutex> lk{impl_->state_mu};
    impl_->err_code   = std::move(code);
    impl_->err_detail = std::move(detail);
    impl_->err_url    = std::move(url);
}
UiStage UiOverlay::stage() const noexcept {
    std::lock_guard<std::mutex> lk{impl_->state_mu};
    return impl_->stage;
}
void UiOverlay::tick(int64_t now_ns) noexcept {
    impl_->now_ns.store(now_ns, std::memory_order_release);
}

void UiOverlay::render() noexcept {
    if (!impl_->rt) return;

    impl_->rt->BeginDraw();
    impl_->rt->SetTransform(D2D1::Matrix3x2F::Identity());

    {
        std::lock_guard<std::mutex> lk{impl_->state_mu};
        impl_->draw_current_locked();
    }

    HRESULT hr = impl_->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        MCP_LOGF(pal::LogLevel::warn, "UiOverlay: D2DERR_RECREATE_TARGET; releasing RT");
        unbind_backbuffer();
    } else if (FAILED(hr)) {
        MCP_LOGF(pal::LogLevel::warn, "UiOverlay: EndDraw failed hr=0x%08lX", hr);
    }
}

}  // namespace mcp::media
