/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The test's random numbers the same on every machine: std::mt19937 is, but how a distribution turns its numbers into
 *  values is the C++ library's own (libstdc++ on Linux, libc++ on a Mac), and the same seed gave other noise and other
 *  answer times on a Mac (2026-11-19: 70 of 72 there, 72 of 72 in CI). These are libstdc++'s algorithms (bits/random.tcc:
 *  generate_canonical, uniform_real_distribution, normal_distribution's Marsaglia polar method with its saved second
 *  value), step for step, so a Mac draws what CI and the radio's GCC build draw.
 */

#pragma once

#include <cmath>
#include <random>

namespace portable {

/* generate_canonical<double, 53> on a 32-bit engine: two of its numbers, the first the low half */
inline double canonical(std::mt19937 &g) {
    const double r = 4294967296.0;      /* max - min + 1 */
    double       sum = 0, tmp = 1;

    for (int k = 2; k != 0; --k) {
        sum += double(g() - std::mt19937::min()) * tmp;
        tmp *= r;
    }
    double ret = sum / tmp;
    if (ret >= 1) ret = std::nextafter(1.0, 0.0);
    return ret;
}

/* uniform_real_distribution<double>(a, b) */
inline double uniform(std::mt19937 &g, double a, double b) {
    return canonical(g) * (b - a) + a;
}

/* normal_distribution<double>(mean, stddev): two values a time, the second kept for the next call */
class normal {
  public:
    normal(double mean = 0.0, double stddev = 1.0) : mean_(mean), stddev_(stddev) {}

    double operator()(std::mt19937 &g) {
        double ret;

        if (saved_ok_) {
            saved_ok_ = false;
            ret       = saved_;
        } else {
            double x, y, r2;
            do {
                x  = 2.0 * canonical(g) - 1.0;
                y  = 2.0 * canonical(g) - 1.0;
                r2 = x * x + y * y;
            } while (r2 > 1.0 || r2 == 0.0);
            const double mult = std::sqrt(-2 * std::log(r2) / r2);
            saved_    = x * mult;
            saved_ok_ = true;
            ret       = y * mult;
        }
        return ret * stddev_ + mean_;
    }

  private:
    double mean_, stddev_, saved_ = 0;
    bool   saved_ok_ = false;
};

}  // namespace portable
