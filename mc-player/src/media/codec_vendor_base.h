/*
 * CodecVendorBase — vendor SDK 直驱解码器抽象基类（ADR-015 档 1 公共接口）。
 *
 * 设计动机：
 *   Phase 5/6/7 三档 vendor SDK（NVDEC / oneVPL / AMF）的 DLL 加载、entry-point
 *   probing、controller 路由表，pattern 高度雷同。引一个最小公共抽象避免每档复制
 *   "动态加载 + 探测 + start/stop/submit/pull"五件套。
 *
 *   不强制 vendor 实现继承本基类（它们公共方法名已对位），仅作"概念锚点"
 *   用于 review / 文档导航。
 *
 * 不在范围：
 *   - 不替换具体 codec 实现的 ABI（H.264 / H.265 / AV1 各自的 codec 配置仍由
 *     具体 vendor 实现自管），不引数据结构耦合
 *   - 不引入虚函数表（vendor 实现互斥，运行期不需要 dispatch）
 *   - 不替代 ADR-015 ChainSelector 的 codec 选档（那是 controller 层）
 *
 * 接口契约（每档 vendor 实现都应提供同名方法签名）：
 *   - start() / stop() — D3D11 device + emit 回调注入；失败返 MC_ERR_NO_HARDWARE
 *     (SDK 缺失) / MC_ERR_UNSUPPORTED (vendor mismatch) / MC_ERR_INTERNAL
 *   - submit(au, pts, qpc) — 单 AU 入队，与 codec_dxva_video / codec_mft_video 对齐
 *   - flush() — 排空 + 重置内部 DPB / parser
 *
 * 探测层（运行前）：
 *   - VendorId 检测：DXGI_ADAPTER_DESC1::VendorId
 *     0x10DE = NVIDIA / 0x8086 = Intel / 0x1002 = AMD
 *   - SDK 存在性：LoadLibraryW(SDK_DLL) 试打开 → FreeLibrary 关闭
 *
 * skip reason 分类（性能量度规范 §2.4 mc.probe.tier_skip_reason.tier1.*）：
 *   - vendor_mismatch    (adapter VendorId 与本档目标不符)
 *   - sdk_missing        (DLL 文件不存在)
 *   - sdk_init_failed    (DLL 加载但 entry-point 缺失)
 *   - sdk_decode_pending (DLL 已加载，decode 实装未完成 — Phase 5/6/7 中间态)
 *   - profile_unsupported (codec / profile / 分辨率 不在 SDK 实装范围)
 */

#ifndef MC_PLAYER_MEDIA_CODEC_VENDOR_BASE_H_
#define MC_PLAYER_MEDIA_CODEC_VENDOR_BASE_H_

#include <cstdint>

namespace mcp::media {

// IHV 厂商 PCI VendorId（DXGI_ADAPTER_DESC1::VendorId）。
constexpr uint32_t kVendorIdNvidia = 0x10DE;
constexpr uint32_t kVendorIdIntel  = 0x8086;
constexpr uint32_t kVendorIdAmd    = 0x1002;

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_CODEC_VENDOR_BASE_H_
