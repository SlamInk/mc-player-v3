/*
 * RTSP 消息编解码 — 类似 HTTP/1.1，CRLF 分隔，请求行 + 头部 + 可选 body。
 *
 * 仅支持 RTSP 1.0 (RFC 2326)。RTSP 2.0 (RFC 7826) 后续按需扩展。
 */

#ifndef MC_PLAYER_TRANSPORT_RTSP_MESSAGE_H_
#define MC_PLAYER_TRANSPORT_RTSP_MESSAGE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::transport {

struct RtspHeader {
    std::string name;       // 原样保留大小写
    std::string value;
};

struct RtspRequest {
    std::string             method;     // OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN / GET_PARAMETER ...
    std::string             uri;
    std::string             version{"RTSP/1.0"};
    uint32_t                cseq{0};
    std::vector<RtspHeader> headers;
    std::string             body;       // 通常为空；DESCRIBE 响应会带 SDP body

    void set_header(std::string name, std::string value);

    /// 序列化为原始字节流。
    std::string serialize() const;
};

struct RtspResponse {
    std::string             version{"RTSP/1.0"};
    int                     status_code{0};
    std::string             reason;
    uint32_t                cseq{0};
    std::vector<RtspHeader> headers;
    std::string             body;

    /// 头部按 case-insensitive 查找；返回首个匹配。
    [[nodiscard]] const std::string* find_header(std::string_view name) const noexcept;
};

/// 增量解析器：调用方按收到字节追加，parser 在每完整一条消息时返回。
class RtspParser {
public:
    void append(std::string_view bytes);

    /// 取下一条完整响应。无完整消息返回 std::nullopt；body 长度由 Content-Length 决定。
    std::optional<RtspResponse> next_response() noexcept;

private:
    std::string buf_;
};

}  // namespace mcp::transport

#endif  // MC_PLAYER_TRANSPORT_RTSP_MESSAGE_H_
