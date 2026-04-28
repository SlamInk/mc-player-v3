#include "media/jitter_buffer_audio.h"

#include "transport/rtp_packet.h"

namespace mcp::media {

JitterBufferAudio::JitterBufferAudio(EmitFn emit) noexcept : emit_{std::move(emit)} {}

void JitterBufferAudio::on_rtp(const transport::RtpDatagram& dg) noexcept {
    transport::RtpPacket pkt;
    if (!transport::parse_rtp(dg.bytes, &pkt)) return;
    AudioJitterFrame f;
    f.pts_us       = dg.arrival_qpc_ns / 1000;
    f.rtp_seq      = pkt.sequence;
    f.rtp_payload.assign(pkt.payload.begin(), pkt.payload.end());
    if (emit_) emit_(std::move(f));
    // TODO: NetEQ 三相位调度。
}

void JitterBufferAudio::reset() noexcept { buffer_ms_ = 0; }

}  // namespace mcp::media
