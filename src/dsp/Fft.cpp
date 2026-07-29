#include "dsp/Fft.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace radar {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

FftPlan::FftPlan(std::size_t n) : n_(n) {
    if (n == 0 || (n & (n - 1)) != 0)
        throw std::invalid_argument("FftPlan: size must be a power of two");

    std::size_t lg = 0;
    while ((std::size_t{1} << lg) < n) ++lg;

    rev_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t r = 0;
        for (std::size_t b = 0; b < lg; ++b)
            if ((i >> b) & 1) r |= std::size_t{1} << (lg - 1 - b);
        rev_[i] = r;
    }

    tw_.resize(n / 2);
    for (std::size_t k = 0; k < n / 2; ++k) {
        const double a = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
        tw_[k] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }
}

void FftPlan::forward(std::complex<float>* x) const {
    // Bit-reversal permutation, then iterative Cooley-Tukey DIT butterflies.
    for (std::size_t i = 0; i < n_; ++i)
        if (i < rev_[i]) std::swap(x[i], x[rev_[i]]);

    for (std::size_t len = 2; len <= n_; len <<= 1) {
        const std::size_t half = len >> 1;
        const std::size_t step = n_ / len;  // twiddle stride into tw_
        for (std::size_t i = 0; i < n_; i += len) {
            for (std::size_t j = 0; j < half; ++j) {
                const std::complex<float> w = tw_[j * step];
                const std::complex<float> u = x[i + j];
                const std::complex<float> v = x[i + j + half] * w;
                x[i + j] = u + v;
                x[i + j + half] = u - v;
            }
        }
    }
}

void fftshift(std::complex<float>* x, std::size_t n) {
    const std::size_t half = n / 2;
    for (std::size_t i = 0; i < half; ++i) std::swap(x[i], x[i + half]);
}

std::vector<float> makeHannWindow(std::size_t n) {
    std::vector<float> w(n, 1.f);
    if (n < 2) return w;
    for (std::size_t i = 0; i < n; ++i)
        w[i] = static_cast<float>(
            0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                  static_cast<double>(n - 1))));
    return w;
}

}  // namespace radar
