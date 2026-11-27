/*
 * Minimal RIFF/WAVE PCM16 backend for the ATRACDEnc build used by LBPPSP2008.
 * It deliberately supports only little-endian PCM16 mono/stereo at 44.1 kHz.
 * ATRACDEnc itself remains under its upstream LGPL-2.1-or-later license.
 */
#include "wav.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
static uint16_t read16(std::istream& f) {
    unsigned char b[2];
    f.read(reinterpret_cast<char*>(b), 2);
    if (!f) throw std::runtime_error("truncated WAV file");
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
}
static uint32_t read32(std::istream& f) {
    unsigned char b[4];
    f.read(reinterpret_cast<char*>(b), 4);
    if (!f) throw std::runtime_error("truncated WAV file");
    return static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
}
static void write16(std::ostream& f, uint16_t v) {
    unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
    f.write(reinterpret_cast<const char*>(b), 2);
}
static void write32(std::ostream& f, uint32_t v) {
    unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                          static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
    f.write(reinterpret_cast<const char*>(b), 4);
}
static uint32_t fourcc(const char* s) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[3])) << 24);
}

class SimpleWav : public IPCMProviderImpl {
public:
    explicit SimpleWav(const std::string& path)
        : file_(path, std::ios::binary | std::ios::in), channels_(0), rate_(0), total_(0),
          data_left_(0), write_mode_(false) {
        if (!file_) throw std::runtime_error("cannot open WAV input: " + path);
        char magic[4];
        file_.read(magic, 4);
        if (!file_ || std::string(magic, 4) != "RIFF")
            throw std::runtime_error("input is not a RIFF/WAVE file");
        (void)read32(file_);
        file_.read(magic, 4);
        if (!file_ || std::string(magic, 4) != "WAVE")
            throw std::runtime_error("input is not a WAVE file");

        bool have_fmt = false;
        bool have_data = false;
        while (file_ && !have_data) {
            char id[4];
            file_.read(id, 4);
            if (file_.gcount() != 4) break;
            const uint32_t size = read32(file_);
            const uint32_t tag = fourcc(id);
            if (tag == fourcc("fmt ")) {
                if (size < 16) throw std::runtime_error("invalid WAV fmt chunk");
                const uint16_t format = read16(file_);
                channels_ = read16(file_);
                rate_ = read32(file_);
                (void)read32(file_); // byte rate
                const uint16_t align = read16(file_);
                const uint16_t bits = read16(file_);
                if (format != 1 || bits != 16 || channels_ == 0 || channels_ > 2 ||
                    rate_ != 44100 || align != channels_ * 2)
                    throw std::runtime_error("WAV input must be PCM16 mono/stereo at 44100 Hz");
                const uint32_t extra = size - 16;
                file_.seekg(extra, std::ios::cur);
                have_fmt = true;
            } else if (tag == fourcc("data")) {
                if (!have_fmt) throw std::runtime_error("WAV data chunk precedes fmt chunk");
                data_left_ = size - (size % (channels_ * 2));
                total_ = data_left_ / (channels_ * 2);
                have_data = true;
            } else {
                file_.seekg(size, std::ios::cur);
            }
            if (size & 1) file_.seekg(1, std::ios::cur);
        }
        if (!have_fmt || !have_data)
            throw std::runtime_error("WAV input is missing fmt or data chunk");
    }

    SimpleWav(const std::string& path, size_t channels, size_t sampleRate)
        : file_(path, std::ios::binary | std::ios::in | std::ios::out |
                      std::ios::trunc),
          channels_(channels), rate_(sampleRate), total_(0), data_left_(0),
          write_mode_(true), data_bytes_(0) {
        if (!file_) throw std::runtime_error("cannot create WAV output: " + path);
        if (channels_ == 0 || channels_ > 2 || rate_ != 44100)
            throw std::runtime_error("WAV output must be mono/stereo at 44100 Hz");
        writeHeader(0, 0);
    }

    ~SimpleWav() override {
        if (write_mode_ && file_) {
            file_.seekp(0, std::ios::beg);
            writeHeader(data_bytes_, total_);
            file_.flush();
        }
    }

    size_t GetChannelsNum() const override { return channels_; }
    size_t GetSampleRate() const override { return rate_; }
    size_t GetTotalSamples() const override { return total_; }

    size_t Read(TPCMBuffer& buffer, size_t requested) override {
        if (write_mode_) throw std::runtime_error("cannot read WAV output");
        if (buffer.Channels() != channels_)
            throw std::runtime_error("WAV read channel count mismatch");
        const size_t frameBytes = channels_ * 2;
        const size_t frames = std::min(requested, static_cast<size_t>(data_left_ / frameBytes));
        if (!frames) return 0;
        std::vector<unsigned char> raw(frames * frameBytes);
        file_.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        const size_t gotBytes = static_cast<size_t>(file_.gcount());
        const size_t gotFrames = gotBytes / frameBytes;
        for (size_t i = 0; i < gotFrames * channels_; ++i) {
            const uint16_t u = static_cast<uint16_t>(raw[i * 2] | (raw[i * 2 + 1] << 8));
            const int16_t s = static_cast<int16_t>(u);
            buffer[0][i] = static_cast<float>(s) / 32768.0f;
        }
        data_left_ -= static_cast<uint32_t>(gotFrames * frameBytes);
        return gotFrames;
    }

    size_t Write(const TPCMBuffer& buffer, size_t frames) override {
        if (!write_mode_) throw std::runtime_error("cannot write WAV input");
        if (buffer.Channels() != channels_)
            throw std::runtime_error("WAV write channel count mismatch");
        std::vector<unsigned char> raw(frames * channels_ * 2);
        size_t k = 0;
        for (size_t i = 0; i < frames * channels_; ++i) {
            const float sample = std::max(-1.0f, std::min(1.0f, buffer[0][i]));
            const int32_t v = static_cast<int32_t>(sample * (sample < 0 ? 32768.0f : 32767.0f));
            const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(v));
            raw[k++] = static_cast<unsigned char>(u);
            raw[k++] = static_cast<unsigned char>(u >> 8);
        }
        file_.write(reinterpret_cast<const char*>(raw.data()),
                    static_cast<std::streamsize>(raw.size()));
        if (!file_) throw std::runtime_error("failed writing WAV output");
        data_bytes_ += static_cast<uint32_t>(raw.size());
        total_ += frames;
        return frames;
    }

private:
    void writeHeader(uint32_t dataBytes, uint64_t frameCount) {
        const uint32_t byteRate = static_cast<uint32_t>(rate_ * channels_ * 2);
        file_.write("RIFF", 4);
        write32(file_, 36 + dataBytes);
        file_.write("WAVEfmt ", 8);
        write32(file_, 16);
        write16(file_, 1);
        write16(file_, static_cast<uint16_t>(channels_));
        write32(file_, static_cast<uint32_t>(rate_));
        write32(file_, byteRate);
        write16(file_, static_cast<uint16_t>(channels_ * 2));
        write16(file_, 16);
        file_.write("data", 4);
        write32(file_, dataBytes);
        (void)frameCount;
    }

    std::fstream file_;
    size_t channels_;
    size_t rate_;
    uint64_t total_;
    uint32_t data_left_;
    bool write_mode_;
    uint32_t data_bytes_ = 0;
};
} // namespace

IPCMProviderImpl* CreatePCMIOReadImpl(const std::string& path) {
    return new SimpleWav(path);
}
IPCMProviderImpl* CreatePCMIOWriteImpl(const std::string& path, int channels, int sampleRate) {
    return new SimpleWav(path, static_cast<size_t>(channels), static_cast<size_t>(sampleRate));
}
