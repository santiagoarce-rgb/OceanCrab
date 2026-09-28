#pragma once

#include "spongebob_audio_resampler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace spongebob {

// One guest-channel mixer. waveOut and SDL only differ in how a block is queued.
class AudioMixer {
public:
    static constexpr std::uint32_t kSampleRate = StreamingLinearResampler::kOutputRate;
    static constexpr std::uint32_t kOutputChannels = 2u;
    static constexpr std::size_t kBlockFrames = 512u;
    static constexpr std::size_t kBlockCount = 24u;
    static constexpr std::size_t kDefaultPrebufferBlocks = 6u;
    static constexpr std::uint64_t kMixSafetyFrames = 1024u;
    static constexpr std::size_t kRingFrames = kSampleRate * 2u;
    static constexpr std::size_t kGuestChannels = 9u;
    static constexpr std::uint64_t kChannelDiscontinuityFrames = 64u;
    static constexpr std::size_t kBlockSamples = kBlockFrames * kOutputChannels;

    [[nodiscard]] static bool diagnostics_enabled() {
        static const bool enabled = std::getenv("PSPRECOMP_AUDIO_DIAG") != nullptr;
        return enabled;
    }

    [[nodiscard]] static bool summary_diagnostics_enabled() {
        static const bool enabled = [] {
            const char *text = std::getenv("PSPRECOMP_AUDIO_SUMMARY");
            if (text != nullptr)
                return *text != '\0' && std::strcmp(text, "0") != 0;
            return false;
        }();
        return enabled;
    }

    [[nodiscard]] bool opened() const noexcept { return opened_; }
    [[nodiscard]] std::size_t prebuffer_blocks() const noexcept { return prebuffer_blocks_; }

    void note_device_opened() {
        ring_.assign(kRingFrames * kOutputChannels, 0);
        output_frame_ = 0u;
        queued_blocks_ = 0u;
        prebuffer_blocks_ = configured_prebuffer_blocks();
        recovery_prebuffer_blocks_ = std::clamp<std::size_t>(
            prebuffer_blocks_ + 2u, prebuffer_blocks_, kBlockCount - 2u);
        playback_started_ = false;
        recovering_from_underrun_ = false;
        opened_ = true;
        if (summary_diagnostics_enabled()) {
            diagnostics_log_.open("SpongeBobAudio.log", std::ios::out | std::ios::trunc);
            if (diagnostics_log_)
                diagnostics_log_ << "[audio-log] block_frames=" << kBlockFrames
                                 << " startup_blocks=" << prebuffer_blocks_
                                 << " recovery_blocks=" << recovery_prebuffer_blocks_ << "\n";
        }
        open_wav_capture();
    }

    template <typename Device>
    void submit(std::span<const std::int16_t> pcm, std::uint32_t frames, bool stereo,
                std::uint32_t left, std::uint32_t right, std::uint32_t source_rate,
                std::uint32_t channel, std::uint64_t start_time_us, std::uint64_t now_us,
                Device &device) {
        if (frames == 0u || channel >= kGuestChannels) return;
        if (source_rate == 0u) source_rate = kSampleRate;
        const std::size_t needed = static_cast<std::size_t>(frames) * (stereo ? 2u : 1u);
        if (pcm.size() < needed) return;
        const bool measure_submit = summary_diagnostics_enabled();
        const auto submit_started = measure_submit
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (!device.ensure_open(*this)) return;

        if (!timeline_anchored_) {
            guest_anchor_us_ = std::min(start_time_us, now_us);
            timeline_anchored_ = true;
            output_frame_ = 0u;
        }
        advance_locked(now_us, device);

        Channel &stream = channels_[channel];
        const std::uint64_t scheduled = guest_frame_for(start_time_us);
        const auto distance = [](std::uint64_t a, std::uint64_t b) {
            return a > b ? a - b : b - a;
        };
        const bool format_changed = stream.active &&
            (stream.source_rate != source_rate || stream.stereo != stereo);
        const bool discontinuity = stream.active &&
            distance(stream.cursor, scheduled) > kChannelDiscontinuityFrames;
        const std::uint64_t previous_cursor = stream.cursor;
        if (!stream.active || format_changed || discontinuity) {
            stream = Channel{};
            stream.active = true;
            stream.source_rate = source_rate;
            stream.stereo = stereo;
            stream.resampler.reset(source_rate, stereo);
            stream.cursor = std::max(scheduled, output_frame_);
            if (discontinuity) ++timeline_resyncs_;
            if (diagnostics_enabled() && discontinuity)
                std::cerr << "[audio-host] channel " << channel << " timeline resync old="
                          << previous_cursor << " scheduled=" << scheduled << "\n";
        }
        if (stream.cursor < output_frame_) {
            late_frames_dropped_ += output_frame_ - stream.cursor;
            stream.cursor = output_frame_;
            stream.resampler.reset(source_rate, stereo);
        }

        const std::uint32_t master = 100u;
        const std::int64_t left_gain = (static_cast<std::int64_t>(left) * master) / 100;
        const std::int64_t right_gain = (static_cast<std::int64_t>(right) * master) / 100;
        const std::uint64_t ring_limit = output_frame_ + kRingFrames - kBlockFrames;
        stream.resampler.process(pcm, frames, stereo, source_rate,
            [&](std::int16_t source_left, std::int16_t source_right) {
                if (stream.cursor >= ring_limit) {
                    ++overrun_frames_dropped_;
                    ++stream.cursor;
                    return;
                }
                const std::size_t slot =
                    static_cast<std::size_t>(stream.cursor % kRingFrames) * kOutputChannels;
                const std::int64_t mixed_left =
                    (static_cast<std::int64_t>(source_left) * left_gain) >> 15;
                const std::int64_t mixed_right =
                    (static_cast<std::int64_t>(source_right) * right_gain) >> 15;
                ring_[slot] += static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    mixed_left, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
                ring_[slot + 1u] += static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    mixed_right, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
                ++stream.cursor;
            });
        stream.last_guest_time_us = start_time_us;
        advance_locked(now_us, device);
        if (measure_submit) {
            const std::uint64_t submit_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - submit_started).count());
            ++submit_calls_;
            submit_cpu_ns_ += submit_ns;
            submit_cpu_max_ns_ = std::max(submit_cpu_max_ns_, submit_ns);
        }
    }

    template <typename Device>
    void advance(std::uint64_t guest_time_us, Device &device) {
        if (!opened_) return;
        advance_locked(guest_time_us, device);
    }

    template <typename Device>
    void write_summary(std::uint64_t guest_time_us, Device &device) {
        if (!opened_ || !summary_diagnostics_enabled()) return;
        if (last_summary_guest_us_ != 0u && guest_time_us - last_summary_guest_us_ < 2'000'000u)
            return;
        const std::uint64_t average_submit_us = submit_calls_ == 0u ? 0u
            : submit_cpu_ns_ / submit_calls_ / 1000u;
        std::ostringstream line;
        line << "[audio-summary] guest_us=" << guest_time_us
             << " guest_frame=" << guest_frame_for(guest_time_us)
             << " output_frame=" << output_frame_
             << " outstanding_blocks=" << device.outstanding_blocks()
             << " playback=" << playback_started_
             << " recovering=" << recovering_from_underrun_
             << " underrun_rebuffers=" << underrun_rebuffers_
             << " resyncs=" << timeline_resyncs_
             << " late_frames=" << late_frames_dropped_
             << " overrun_frames=" << overrun_frames_dropped_
             << " submit_calls=" << submit_calls_
             << " submit_avg_us=" << average_submit_us
             << " submit_max_us=" << submit_cpu_max_ns_ / 1000u << "\n";
        std::cerr << line.str();
        if (diagnostics_log_) {
            diagnostics_log_ << line.str();
            diagnostics_log_.flush();
        }
        last_summary_guest_us_ = guest_time_us;
    }

    void reset_channel(std::uint32_t channel) {
        if (channel >= channels_.size()) return;
        channels_[channel] = Channel{};
    }

    template <typename Device>
    void shutdown(Device &device) {
        if (!opened_) return;
        device.close_device(playback_started_);
        close_capture();
        ring_.clear();
        timeline_anchored_ = false;
        playback_started_ = false;
        recovering_from_underrun_ = false;
        queued_blocks_ = 0u;
        output_frame_ = 0u;
        last_summary_guest_us_ = 0u;
        opened_ = false;
        for (std::uint32_t channel = 0u; channel < kGuestChannels; ++channel)
            reset_channel(channel);
        if (diagnostics_enabled() &&
            (late_frames_dropped_ != 0u || overrun_frames_dropped_ != 0u)) {
            std::cerr << "[audio-host] shutdown late_frames=" << late_frames_dropped_
                      << " overrun_frames=" << overrun_frames_dropped_ << "\n";
        }
        late_frames_dropped_ = 0u;
        overrun_frames_dropped_ = 0u;
    }

private:
    struct Channel {
        StreamingLinearResampler resampler;
        std::uint64_t cursor{};
        std::uint64_t last_guest_time_us{};
        std::uint32_t source_rate{kSampleRate};
        bool stereo{true};
        bool active{};
    };

    [[nodiscard]] static std::size_t configured_prebuffer_blocks() {
        const char *text = std::getenv("PSPRECOMP_AUDIO_PREBUFFER_BLOCKS");
        if (text == nullptr || *text == '\0') return kDefaultPrebufferBlocks;
        char *end = nullptr;
        const unsigned long value = std::strtoul(text, &end, 0);
        if (end == text || *end != '\0') return kDefaultPrebufferBlocks;
        return std::clamp<std::size_t>(static_cast<std::size_t>(value), 2u, kBlockCount - 2u);
    }

    [[nodiscard]] std::uint64_t guest_frame_for(std::uint64_t guest_time_us) const {
        if (!timeline_anchored_ || guest_time_us <= guest_anchor_us_) return 0u;
        const std::uint64_t delta = guest_time_us - guest_anchor_us_;
        return (delta * kSampleRate + 500000u) / 1000000u;
    }

    void open_wav_capture() {
        const char *path = std::getenv("PSPRECOMP_AUDIO_WAV");
        if (path == nullptr || *path == '\0') return;
        wav_capture_.open(path, std::ios::binary | std::ios::trunc);
        if (!wav_capture_) {
            if (diagnostics_enabled())
                std::cerr << "[audio-host] unable to create WAV capture: " << path << "\n";
            return;
        }
        wav_write_header(wav_capture_, 0u);
        wav_frames_ = 0u;
        if (diagnostics_enabled())
            std::cerr << "[audio-host] WAV capture: " << path << "\n";
    }

    void close_capture() {
        if (wav_capture_.is_open()) {
            wav_capture_.flush();
            wav_capture_.seekp(0, std::ios::beg);
            wav_write_header(wav_capture_, wav_frames_);
            wav_capture_.close();
        }
        if (diagnostics_log_.is_open()) diagnostics_log_.close();
    }

    static void wav_write_u16(std::ostream &out, std::uint16_t value) {
        const std::array<char, 2> bytes{
            static_cast<char>(value & 0xFFu), static_cast<char>((value >> 8u) & 0xFFu)};
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    static void wav_write_u32(std::ostream &out, std::uint32_t value) {
        const std::array<char, 4> bytes{
            static_cast<char>(value & 0xFFu), static_cast<char>((value >> 8u) & 0xFFu),
            static_cast<char>((value >> 16u) & 0xFFu), static_cast<char>((value >> 24u) & 0xFFu)};
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    static void wav_write_header(std::ostream &out, std::uint64_t frames) {
        const std::uint64_t payload64 = frames * kOutputChannels * sizeof(std::int16_t);
        const std::uint32_t payload = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(payload64, 0xFFFFFFFFull - 44u));
        out.write("RIFF", 4); wav_write_u32(out, 36u + payload);
        out.write("WAVEfmt ", 8); wav_write_u32(out, 16u);
        wav_write_u16(out, 1u); wav_write_u16(out, static_cast<std::uint16_t>(kOutputChannels));
        wav_write_u32(out, kSampleRate);
        wav_write_u32(out, kSampleRate * kOutputChannels * sizeof(std::int16_t));
        wav_write_u16(out, static_cast<std::uint16_t>(kOutputChannels * sizeof(std::int16_t)));
        wav_write_u16(out, 16u);
        out.write("data", 4); wav_write_u32(out, payload);
    }

    template <typename Device>
    bool queue_one_block(Device &device) {
        std::int16_t *samples = nullptr;
        if (!device.begin_block(samples) || samples == nullptr) return false;
        for (std::size_t frame = 0u; frame < kBlockFrames; ++frame) {
            const std::size_t slot =
                static_cast<std::size_t>((output_frame_ + frame) % kRingFrames) * kOutputChannels;
            for (std::size_t channel = 0u; channel < kOutputChannels; ++channel) {
                samples[frame * kOutputChannels + channel] = static_cast<std::int16_t>(
                    std::clamp(ring_[slot + channel], -32768, 32767));
                ring_[slot + channel] = 0;
            }
        }
        if (wav_capture_.is_open()) {
            wav_capture_.write(reinterpret_cast<const char *>(samples),
                               static_cast<std::streamsize>(kBlockSamples * sizeof(std::int16_t)));
            if (wav_capture_) wav_frames_ += kBlockFrames;
        }
        if (!device.commit_block()) return false;
        output_frame_ += kBlockFrames;
        ++queued_blocks_;
        const std::size_t target_blocks = recovering_from_underrun_
            ? recovery_prebuffer_blocks_ : prebuffer_blocks_;
        if (!playback_started_ && device.outstanding_blocks() >= target_blocks) {
            device.resume_playback();
            playback_started_ = true;
            recovering_from_underrun_ = false;
            if (diagnostics_enabled())
                std::cerr << "[audio-host] " << device.backend_name()
                          << " started with " << queued_blocks_ << " prebuffered blocks\n";
        }
        return true;
    }

    template <typename Device>
    void advance_locked(std::uint64_t guest_time_us, Device &device) {
        if (!timeline_anchored_ || !opened_) return;
        std::size_t outstanding = device.outstanding_blocks();
        if (playback_started_ && outstanding == 0u) {
            device.pause_playback();
            ++underrun_rebuffers_;
            playback_started_ = false;
            recovering_from_underrun_ = true;
        }
        const std::uint64_t guest_frame = guest_frame_for(guest_time_us);
        const std::uint64_t safety_frames = playback_started_ && outstanding <= 2u
            ? 0u : kMixSafetyFrames;
        const std::uint64_t sealed_frame = guest_frame > safety_frames
            ? guest_frame - safety_frames : 0u;
        const std::size_t latency_limit_blocks = prebuffer_blocks_ + 2u;
        while (sealed_frame >= output_frame_ + kBlockFrames) {
            if (playback_started_ && outstanding >= latency_limit_blocks) {
                for (std::size_t frame = 0u; frame < kBlockFrames; ++frame) {
                    const std::size_t slot =
                        static_cast<std::size_t>((output_frame_ + frame) % kRingFrames) * kOutputChannels;
                    for (std::size_t channel = 0u; channel < kOutputChannels; ++channel)
                        ring_[slot + channel] = 0;
                }
                output_frame_ += kBlockFrames;
                late_frames_dropped_ += kBlockFrames;
                continue;
            }
            if (!queue_one_block(device)) break;
            ++outstanding;
        }
    }

    std::vector<std::int32_t> ring_;
    std::uint64_t output_frame_{};
    std::uint64_t guest_anchor_us_{};
    bool timeline_anchored_{};
    std::array<Channel, kGuestChannels> channels_{};
    std::uint64_t late_frames_dropped_{};
    std::uint64_t overrun_frames_dropped_{};
    std::uint64_t queued_blocks_{};
    std::uint64_t underrun_rebuffers_{};
    std::uint64_t timeline_resyncs_{};
    std::uint64_t submit_calls_{};
    std::uint64_t submit_cpu_ns_{};
    std::uint64_t submit_cpu_max_ns_{};
    std::uint64_t last_summary_guest_us_{};
    std::ofstream wav_capture_;
    std::ofstream diagnostics_log_;
    std::uint64_t wav_frames_{};
    std::size_t prebuffer_blocks_{kDefaultPrebufferBlocks};
    std::size_t recovery_prebuffer_blocks_{kDefaultPrebufferBlocks * 2u};
    bool playback_started_{};
    bool recovering_from_underrun_{};
    bool opened_{};
};

}
