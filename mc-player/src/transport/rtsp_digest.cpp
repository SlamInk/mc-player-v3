#include "transport/rtsp_digest.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace mcp::transport {

namespace {

constexpr DWORD kMd5DigestBytes = 16;

bool md5_raw(std::string_view input, std::array<uint8_t, kMd5DigestBytes>& out) noexcept {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = ::BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(st)) return false;

    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    do {
        st = ::BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
        if (!BCRYPT_SUCCESS(st)) break;
        st = ::BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                              static_cast<ULONG>(input.size()), 0);
        if (!BCRYPT_SUCCESS(st)) break;
        st = ::BCryptFinishHash(h, out.data(), kMd5DigestBytes, 0);
        if (!BCRYPT_SUCCESS(st)) break;
        ok = true;
    } while (false);

    if (h)   ::BCryptDestroyHash(h);
    if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

constexpr char kHexLower[] = "0123456789abcdef";

std::string to_hex(const std::array<uint8_t, kMd5DigestBytes>& data) noexcept {
    std::string out(kMd5DigestBytes * 2, '0');
    for (std::size_t i = 0; i < kMd5DigestBytes; ++i) {
        out[i * 2 + 0] = kHexLower[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHexLower[(data[i]     ) & 0x0F];
    }
    return out;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' || s.back()  == '\r')) s.remove_suffix(1);
    return s;
}

// Token-aware split for header parameters: "k=v, k2=\"v2\", k3=v3".
std::pair<std::string_view, std::string_view> next_param(std::string_view& s) noexcept {
    s = trim(s);
    if (s.empty()) return {};
    auto eq = s.find('=');
    if (eq == std::string_view::npos) {
        std::string_view k = s;
        s = {};
        return {trim(k), {}};
    }
    std::string_view key = trim(s.substr(0, eq));
    std::string_view rest = s.substr(eq + 1);
    rest = trim(rest);
    std::string_view val;
    if (!rest.empty() && rest.front() == '"') {
        rest.remove_prefix(1);
        auto end_q = rest.find('"');
        if (end_q == std::string_view::npos) {
            val = rest;
            rest = {};
        } else {
            val = rest.substr(0, end_q);
            rest = rest.substr(end_q + 1);
        }
        auto comma = rest.find(',');
        s = (comma == std::string_view::npos) ? std::string_view{} : trim(rest.substr(comma + 1));
    } else {
        auto comma = rest.find(',');
        if (comma == std::string_view::npos) {
            val = trim(rest);
            s = {};
        } else {
            val = trim(rest.substr(0, comma));
            s   = trim(rest.substr(comma + 1));
        }
    }
    return {key, val};
}

}  // namespace

std::string md5_hex(std::string_view input) noexcept {
    std::array<uint8_t, kMd5DigestBytes> raw{};
    if (!md5_raw(input, raw)) return {};
    return to_hex(raw);
}

std::optional<DigestChallenge> parse_digest_challenge(std::string_view header_value) noexcept {
    auto v = trim(header_value);
    // 期望以 "Digest" 开头（不区分大小写）。
    if (v.size() < 6) return std::nullopt;
    std::string_view scheme = v.substr(0, 6);
    std::string scheme_lower(scheme);
    std::transform(scheme_lower.begin(), scheme_lower.end(), scheme_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (scheme_lower != "digest") return std::nullopt;
    v = trim(v.substr(6));

    DigestChallenge ch;
    while (!v.empty()) {
        auto [k, val] = next_param(v);
        std::string klow{k};
        std::transform(klow.begin(), klow.end(), klow.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if      (klow == "realm")     ch.realm.assign(val);
        else if (klow == "nonce")     ch.nonce.assign(val);
        else if (klow == "opaque")    ch.opaque.assign(val);
        else if (klow == "qop")       ch.qop.assign(val);
        else if (klow == "algorithm") ch.algorithm.assign(val);
        else if (klow == "stale") {
            std::string lv{val};
            std::transform(lv.begin(), lv.end(), lv.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            ch.stale = (lv == "true");
        }
    }
    if (ch.realm.empty() || ch.nonce.empty()) return std::nullopt;
    return ch;
}

std::string build_digest_authorization(const DigestChallenge& ch,
                                        std::string_view username,
                                        std::string_view password,
                                        std::string_view http_method,
                                        std::string_view uri,
                                        std::string_view cnonce,
                                        std::string_view nc) noexcept {
    if (username.empty()) return {};

    std::string ha1_in;
    ha1_in.reserve(username.size() + ch.realm.size() + password.size() + 2);
    ha1_in.append(username);
    ha1_in.push_back(':');
    ha1_in.append(ch.realm);
    ha1_in.push_back(':');
    ha1_in.append(password);
    std::string ha1 = md5_hex(ha1_in);
    if (ha1.empty()) return {};

    // MD5-sess（RFC 2617 §3.2.2.2）：HA1 = MD5(MD5(user:realm:pass):nonce:cnonce)
    std::string algo_lower = ch.algorithm;
    std::transform(algo_lower.begin(), algo_lower.end(), algo_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (algo_lower == "md5-sess") {
        std::string sess_in = ha1 + ":" + ch.nonce + ":" + std::string{cnonce};
        ha1 = md5_hex(sess_in);
    }

    std::string ha2_in;
    ha2_in.reserve(http_method.size() + uri.size() + 1);
    ha2_in.append(http_method);
    ha2_in.push_back(':');
    ha2_in.append(uri);
    std::string ha2 = md5_hex(ha2_in);
    if (ha2.empty()) return {};

    std::string response;
    const bool use_qop = !ch.qop.empty();
    if (use_qop) {
        std::string r;
        r.reserve(ha1.size() + ch.nonce.size() + nc.size() + cnonce.size() + ch.qop.size() + ha2.size() + 5);
        r.append(ha1);  r.push_back(':');
        r.append(ch.nonce);  r.push_back(':');
        r.append(nc);   r.push_back(':');
        r.append(cnonce);   r.push_back(':');
        // qop 通常是 "auth"；server 可能给 "auth,auth-int"，按 RFC 取第一个。
        std::string qop_use = ch.qop;
        if (auto comma = qop_use.find(','); comma != std::string::npos) {
            qop_use = qop_use.substr(0, comma);
        }
        r.append(qop_use); r.push_back(':');
        r.append(ha2);
        response = md5_hex(r);
    } else {
        std::string r;
        r.reserve(ha1.size() + ch.nonce.size() + ha2.size() + 2);
        r.append(ha1); r.push_back(':');
        r.append(ch.nonce); r.push_back(':');
        r.append(ha2);
        response = md5_hex(r);
    }

    std::string out;
    out.reserve(256);
    out.append("Digest username=\"").append(username).append("\", ");
    out.append("realm=\"").append(ch.realm).append("\", ");
    out.append("nonce=\"").append(ch.nonce).append("\", ");
    out.append("uri=\"").append(uri).append("\", ");
    out.append("response=\"").append(response).append("\"");
    if (!ch.opaque.empty()) {
        out.append(", opaque=\"").append(ch.opaque).append("\"");
    }
    if (use_qop) {
        std::string qop_use = ch.qop;
        if (auto comma = qop_use.find(','); comma != std::string::npos) {
            qop_use = qop_use.substr(0, comma);
        }
        out.append(", qop=").append(qop_use);
        out.append(", nc=").append(nc);
        out.append(", cnonce=\"").append(cnonce).append("\"");
    }
    if (!ch.algorithm.empty()) {
        out.append(", algorithm=").append(ch.algorithm);
    }
    return out;
}

}  // namespace mcp::transport
