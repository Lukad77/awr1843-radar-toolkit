#pragma once
// Fft.h — self-contained iterative radix-2 FFT (no external dependency).
//
// Rationale: every transform size in this pipeline is a power of two by
// construction (RadarConfig::derive() rounds numRangeBins up; doppler size ==
// numChirpsPerFrame; numAngleBins validated pow2), and the sizes are tiny
// (64..256), where a table-driven radix-2 runs in microseconds. FftPlan is the
// seam to swap in pffft/FFTW later without touching any stage.
//
// FftPlan precomputes the bit-reversal permutation and twiddle table once at
// construction; forward() is allocation-free and safe to call concurrently
// from multiple threads (all state is read-only after construction).

#include <complex>
#include <cstddef>
#include <vector>

namespace radar {

class FftPlan {
public:
    // n must be a power of two (>= 1); throws std::invalid_argument otherwise.
    explicit FftPlan(std::size_t n);

    std::size_t size() const noexcept { return n_; }

    // In-place, unnormalized forward DFT: X[k] = sum x[n] e^{-2*pi*i*k*n/N}.
    void forward(std::complex<float>* x) const;

private:
    std::size_t n_;
    std::vector<std::size_t> rev_;            // bit-reversal permutation
    std::vector<std::complex<float>> tw_;     // W_n^k, k in [0, n/2)
};

// Swap halves so zero frequency lands in the center (n must be even).
void fftshift(std::complex<float>* x, std::size_t n);

// Symmetric Hann window of length n (n>=1; returns {1} for n==1).
std::vector<float> makeHannWindow(std::size_t n);

}  // namespace radar
