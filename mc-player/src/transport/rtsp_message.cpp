#include "transport/rtsp_message.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace mcp::transport {

namespace {

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
    return s;
}

}  // namespace

void RtspRequest::set_header(std::string name, std::string value) {
    for (auto& h : headers) {
        if (iequals(h.name, name)) {
            h.value = std::move(value);
            return;
        }
    }
    headers.push_back({std::move(name), std::move(value)});
}

std::string RtspRequest::serialize() const {
    std::string out;
    out.reserve(256 + body.size());
    out.append(method).append(" ").append(uri).append(" ").append(version).append("\r\n");
    out.append("CSeq: ").append(std::to_string(cseq)).append("\r\n");
    for (const auto& h : headers) {
        out.append(h.name).append(": ").append(h.value).append("\r\n");
    }
    if (!body.empty()) {
        bool has_cl = false;
        for (const auto& h : headers) {
            if (iequals(h.name, "Content-Length")) {
                has_cl = true;
                break;
            }
        }
        if (!has_cl) {
            out.append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
        }
    }
    out.append("\r\n");
    out.append(body);
    return out;
}

const std::string* RtspResponse::find_header(std::string_view name) const noexcept {
    for (const auto& h : headers) {
        if (iequals(h.name, name)) return &h.value;
    }
    return nullptr;
}

void RtspParser::append(std::string_view bytes) {
    buf_.append(bytes);
}

std::optional<RtspResponse> RtspParser::next_response() noexcept {
    constexpr std::string_view kHeaderEnd = "\r\n\r\n";
    auto end = buf_.find(kHeaderEnd);
    if (end == std::string::npos) return std::nullopt;

    const std::string_view header_block = std::string_view{buf_}.substr(0, end);
    std::size_t consumed_to = end + kHeaderEnd.size();

    RtspResponse resp;

    // 解析状态行
    auto first_eol = header_block.find("\r\n");
    if (first_eol == std::string_view::npos) return std::nullopt;
    std::string_view status_line = header_block.substr(0, first_eol);
    auto sp1 = status_line.find(' ');
    if (sp1 == std::string_view::npos) return std::nullopt;
    auto sp2 = status_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return std::nullopt;
    resp.version.assign(status_line.substr(0, sp1));
    auto code_str = status_line.substr(sp1 + 1, sp2 - sp1 - 1);
    int code = 0;
    auto [_, ec] = std::from_chars(code_str.data(), code_str.data() + code_str.size(), code);
    if (ec != std::errc{}) return std::nullopt;
    resp.status_code = code;
    resp.reason.assign(status_line.substr(sp2 + 1));

    // 头部
    std::string_view rest = header_block.substr(first_eol + 2);
    while (!rest.empty()) {
        auto eol = rest.find("\r\n");
        std::string_view line = (eol == std::string_view::npos) ? rest : rest.substr(0, eol);
        if (eol == std::string_view::npos) rest = {};
        else rest = rest.substr(eol + 2);
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        RtspHeader h;
        h.name.assign(trim(line.substr(0, colon)));
        h.value.assign(trim(line.substr(colon + 1)));
        if (iequals(h.name, "CSeq")) {
            uint32_t cseq = 0;
            auto vsv = std::string_view{h.value};
            std::from_chars(vsv.data(), vsv.data() + vsv.size(), cseq);
            resp.cseq = cseq;
        }
        resp.headers.push_back(std::move(h));
    }

    // body 长度
    std::size_t body_len = 0;
    if (auto* cl = resp.find_header("Content-Length"); cl != nullptr) {
        std::from_chars(cl->data(), cl->data() + cl->size(), body_len);
    }
    if (buf_.size() - consumed_to < body_len) {
        return std::nullopt;     // body 还没到齐
    }
    resp.body.assign(buf_, consumed_to, body_len);
    consumed_to += body_len;
    buf_.erase(0, consumed_to);
    return resp;
}

}  // namespace mcp::transport
