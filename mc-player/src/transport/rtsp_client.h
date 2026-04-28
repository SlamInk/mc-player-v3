/*
 * RTSP 1.0 信令客户端 — 状态机：
 *
 *   IDLE → CONNECTING(TCP) → OPTIONS → DESCRIBE → SETUP×N → PLAY → KEEPALIVE 循环 → TEARDOWN
 *
 * 与 transport 解耦：RtspClient 只面向「如何写一个请求 / 如何读响应」，
 * 网络 I/O 由 ts_rtsp_udp / ts_rtsp_tcp 各自的 socket 提供（注入 Bytes I/O 接口）。
 *
 * 401 处理：
 *   - 收到 401 → 解析 WWW-Authenticate；优先 Digest（MD5 或 MD5-sess）。
 *   - 同 cseq+nc 重发原请求带 Authorization 头。
 *   - 二次 401 直接 fail（避免无限重试）。
 *
 * Session header 处理：
 *   - SETUP 200 响应中带 Session: <id>[;timeout=N]；记录 session id，所有后续 PLAY/TEARDOWN/GET_PARAMETER 携带。
 *   - keepalive 间隔取 timeout/2，默认 30 s。
 */

#ifndef MC_PLAYER_TRANSPORT_RTSP_CLIENT_H_
#define MC_PLAYER_TRANSPORT_RTSP_CLIENT_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "mc-player/mc_player_types.h"
#include "transport/rtsp_digest.h"
#include "transport/rtsp_message.h"
#include "transport/sdp_parser.h"

namespace mcp::transport {

enum class RtspState {
    idle,
    connecting,
    sent_options,
    sent_describe,
    sent_setup,
    sent_play,
    playing,
    keepalive,
    tearing_down,
    closed,
    failed,
};

/// 注入的字节流读写接口 — 由 ts_rtsp_udp / ts_rtsp_tcp 提供。
struct RtspByteIo {
    std::function<bool(std::span<const uint8_t>)>   send;
    std::function<bool(std::vector<uint8_t>&)>      read_some;       // 阻塞读，写入返回 true
    std::function<void()>                           shutdown;
};

struct RtspSetupTarget {
    SdpMedia::Kind  kind;
    std::string     control_uri;     // 绝对或相对
    uint16_t        client_rtp_port  = 0;     // UDP path 时填写
    uint16_t        client_rtcp_port = 0;
    bool            interleaved      = false;
    uint8_t         interleaved_rtp_channel  = 0;
    uint8_t         interleaved_rtcp_channel = 1;
    /// 续接已有 session（多 track 场景：第二次起的 SETUP 必须带回首次返回的 Session id，
    /// 否则服务器会另开 session 或拒 454，PLAY 时也只会推首条 track。RFC 2326 §12.37）。
    std::string     existing_session_id;
};

struct RtspSetupResult {
    SdpMedia::Kind  kind;
    std::string     session_id;
    uint16_t        server_rtp_port  = 0;
    uint16_t        server_rtcp_port = 0;
    std::string     server_source;             // 服务器声明的 source（多播 / 转发节点）
    uint32_t        ssrc              = 0;
    uint32_t        timeout_seconds   = 60;
};

class RtspClient {
public:
    RtspClient();
    ~RtspClient();

    void set_credentials(std::string username, std::string password) noexcept;
    void set_byte_io(RtspByteIo io) noexcept;

    /// 完整 OPTIONS+DESCRIBE 序列；成功后填充 sdp_。
    mc_status_t do_describe(const std::string& url,
                             uint32_t timeout_ms,
                             SdpSession& out_sdp) noexcept;

    /// 单条 SETUP 请求；caller 按媒体类型多次调用。
    mc_status_t do_setup(const std::string& base_url,
                          const RtspSetupTarget& target,
                          uint32_t timeout_ms,
                          RtspSetupResult& out_result) noexcept;

    /// PLAY；启动数据流。
    mc_status_t do_play(const std::string& base_url,
                         const std::string& session_id,
                         uint32_t timeout_ms) noexcept;

    /// keepalive：默认 GET_PARAMETER（OPTIONS 兜底）。
    mc_status_t do_keepalive(const std::string& base_url,
                              const std::string& session_id,
                              uint32_t timeout_ms) noexcept;

    /// TEARDOWN — 失败也忽略；属于尽力清理。
    void do_teardown(const std::string& base_url,
                      const std::string& session_id) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::transport

#endif  // MC_PLAYER_TRANSPORT_RTSP_CLIENT_H_
