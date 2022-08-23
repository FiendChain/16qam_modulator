#include "constellation.h"

#define _USE_MATH_DEFINES
#include <math.h>
constexpr float PI = (float)M_PI;

SquareConstellation::SquareConstellation(const int _L)
: L(_L), N(_L*_L) 
{
    C = new std::complex<float>[N];

    const float offset = (L-1)/2.0f;
    const float scale = 1.0f/std::sqrtf(2.0f) * 1.0f/offset * 0.5f;

    for (int i = 0; i < L; i++) {
        const float I = 2.0f * ((float)i - offset);
        for (int j = 0; j < L; j++) {
            const float Q = 2.0f * ((float)j - offset);
            const int idx = i*L + j;
            const auto sym = std::complex<float>(I, Q);
            C[idx] = sym * scale;
        }
    }
    m_avg_power = CalculateAveragePower(C, N);
}

SquareConstellation::~SquareConstellation() {
    delete [] C;
}

uint8_t SquareConstellation::GetNearestSymbol(const std::complex<float> x) {
    float min_err = INFINITY;
    uint8_t best_match = 0;

    for (uint8_t i = 0; i < N; i++) {
        auto err_vec = C[i]-x;
        auto err = std::abs(err_vec);
        if (err < min_err) {
            best_match = i;
            min_err = err;
        }
    }

    return best_match;
}

float SquareConstellation::CalculateAveragePower(const std::complex<float>* C, const int N) {
    float avg_power = 0.0f;
    for (int i = 0; i < N; i++) {
        const auto& x = C[i];
        const float I = x.real();
        const float Q = x.imag();
        avg_power += (I*I + Q*Q);
    }
    avg_power /= (float)(N);
    return avg_power;
}

// get the phase error from the known constellation
ConstellationErrorResult estimate_phase_error(const std::complex<float> x, const std::complex<float>* C, const int N) {
    int min_index = 0;
    float best_mag_error = INFINITY;
    for (int i = 0; i < N; i++) {
        auto error = x - C[i];
        auto I = error.real();
        auto Q = error.imag();
        auto mag_error = I*I + Q*Q;

        if (mag_error < best_mag_error) {
            best_mag_error = mag_error;
            min_index = i;
        }
    }

    const auto closest_point = C[min_index];
    const float angle1 = std::atan2f(closest_point.real(), closest_point.imag());
    const float angle2 = std::atan2f(x.real(), x.imag());

    float phase_error = angle1-angle2;
    phase_error = std::fmodf(phase_error + 3*PI, 2*PI);
    phase_error -= PI;

    float mag_error = std::abs(closest_point) - std::abs(x);
    mag_error = std::abs(mag_error);

    return {phase_error, mag_error};
}