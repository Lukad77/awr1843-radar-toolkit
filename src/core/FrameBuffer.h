#pragma once
// FrameBuffer.h — contiguous, cache-friendly storage for one parsed frame.
//
// Replaces the old vector<vector<vector<complex<float>>>> (three levels of heap
// indirection, terrible locality, per-frame reallocation). Layout is row-major
// [chirp][rx][sample]:  idx = (chirp * numRx + rx) * numSamples + sample.
// A single flat allocation is ideal for FFT/CFAR/NN and for pooling/reuse.

#include <complex>
#include <cstddef>
#include <vector>

namespace radar {

// Logical dimensions of one radar frame's parsed samples.
struct FrameShape {
    std::size_t chirps = 0;
    std::size_t rx = 0;
    std::size_t samples = 0;

    std::size_t total() const noexcept { return chirps * rx * samples; }
    bool operator==(const FrameShape& o) const noexcept {
        return chirps == o.chirps && rx == o.rx && samples == o.samples;
    }
    bool operator!=(const FrameShape& o) const noexcept { return !(*this == o); }
};

class FrameBuffer {
public:
    using Sample = std::complex<float>;

    FrameBuffer() = default;
    explicit FrameBuffer(const FrameShape& s) { resize(s); }

    // Resize only reallocates when the total element count changes.
    void resize(const FrameShape& s) {
        shape_ = s;
        if (data_.size() != s.total()) data_.resize(s.total());
    }

    const FrameShape& shape() const noexcept { return shape_; }
    std::size_t size() const noexcept { return data_.size(); }

    Sample* data() noexcept { return data_.data(); }
    const Sample* data() const noexcept { return data_.data(); }

    std::size_t index(std::size_t chirp, std::size_t rx, std::size_t sample) const noexcept {
        return (chirp * shape_.rx + rx) * shape_.samples + sample;
    }
    Sample& at(std::size_t chirp, std::size_t rx, std::size_t sample) noexcept {
        return data_[index(chirp, rx, sample)];
    }
    const Sample& at(std::size_t chirp, std::size_t rx, std::size_t sample) const noexcept {
        return data_[index(chirp, rx, sample)];
    }

private:
    FrameShape shape_{};
    std::vector<Sample> data_;
};

} // namespace radar
