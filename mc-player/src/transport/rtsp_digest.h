/*
 * RTSP / HTTP Digest Authentication — RFC 2617 (qop=auth, MD5).
 *
 * 现网兼容路径：海康/大华/Axis 等老 IPC 都仅支持 MD5。
 * RTSP 1.0 (RFC 2326 §12.5) 引用 RFC 2069；RFC 2617 在此基础上新增 qop / cnonce / nc，
 * 现代 IPC 普遍上 RFC 2617。SHA-256 (RFC 7616) 留作扩展点。
 *
 * 实现使用 Windows BCrypt MD5（CNG）— 平台原生，无第三方依赖。
 */

#ifndef MC_PLAYER_TRANSPORT_RTSP_DIGEST_H_
#define MC_PLAYER_TRANSPORT_RTSP_DIGEST_H_

#include <optional>
#include <string>
#include <string_view>

namespace mcp::transport {

/// 解析 server 发来的 `WWW-Authenticate: Digest …` 头。
struct DigestChallenge {
    std::string realm;
    std::string nonce;
    std::string opaque;
    std::string qop;            // "auth" / "auth,auth-int" / 空
    std::string algorithm;      // "MD5" / "MD5-sess" / 空
    bool        stale = false;
};

std::optional<DigestChallenge> parse_digest_challenge(std::string_view header_value) noexcept;

/// 给定挑战 + 凭据 + RTSP 方法 + URI，构造 `Authorization: Digest …` 头值。
/// nc 由 caller 维护单调递增（HEX 8 位字符串，例 "00000001"）。
/// 返回完整 Authorization 头值（不含 "Authorization: " 前缀）。失败返回空。
std::string build_digest_authorization(const DigestChallenge& ch,
                                        std::string_view username,
                                        std::string_view password,
                                        std::string_view http_method,    // "OPTIONS" / "DESCRIBE" / ...
                                        std::string_view uri,
                                        std::string_view cnonce,
                                        std::string_view nc) noexcept;

/// 工具：MD5 of input → 32 字符 hex（小写）。
std::string md5_hex(std::string_view input) noexcept;

}  // namespace mcp::transport

#endif  // MC_PLAYER_TRANSPORT_RTSP_DIGEST_H_
