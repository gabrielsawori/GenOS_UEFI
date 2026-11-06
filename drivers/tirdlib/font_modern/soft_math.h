/*
 * soft_math.h — Implementasi fungsi matematika untuk freestanding kernel.
 *
 * File ini menyediakan floor, ceil, sqrt, pow, cos, acos, fabs, fmod
 * yang dibutuhkan oleh stb_truetype.h. Tidak bergantung pada libm.
 *
 * CATATAN: File ini harus di-compile dengan SSE2 enabled (-msse2)
 * agar operasi float/double bisa berjalan. Digunakan HANYA oleh
 * font_renderer.c yang memiliki CFLAGS khusus.
 */
#ifndef SOFT_MATH_H
#define SOFT_MATH_H

/* ── fabs ─────────────────────────────────────────────────────────── */
static inline double soft_fabs(double x) {
    return x < 0.0 ? -x : x;
}

/* ── floor ────────────────────────────────────────────────────────── */
static inline double soft_floor(double x) {
    long long i = (long long)x;
    double d = (double)i;
    /* Jika x negatif dan bukan tepat integer, bulatkan ke bawah */
    if (x < 0.0 && d != x) return d - 1.0;
    return d;
}

/* ── ceil ─────────────────────────────────────────────────────────── */
static inline double soft_ceil(double x) {
    long long i = (long long)x;
    double d = (double)i;
    /* Jika x positif dan bukan tepat integer, bulatkan ke atas */
    if (x > 0.0 && d != x) return d + 1.0;
    return d;
}

/* ── fmod ─────────────────────────────────────────────────────────── */
static inline double soft_fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    long long q = (long long)(x / y);
    return x - (double)q * y;
}

/* ── sqrt — Newton-Raphson ────────────────────────────────────────── */
static inline double soft_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    if (x == 1.0) return 1.0;

    double guess = (x > 1.0) ? x * 0.5 : x;
    for (int i = 0; i < 30; i++) {
        double next = 0.5 * (guess + x / guess);
        if (soft_fabs(next - guess) < 1e-15 * guess) break;
        guess = next;
    }
    return guess;
}

/* ── cos — Taylor series + range reduction ────────────────────────── */
static inline double soft_cos(double x) {
    const double PI      = 3.14159265358979323846;
    const double TWO_PI  = 6.28318530717958647692;

    if (x < 0.0) x = -x;                     /* cos(-x) = cos(x) */
    if (x >= TWO_PI) {
        long long n = (long long)(x / TWO_PI);
        x -= (double)n * TWO_PI;
    }
    if (x > PI) x = TWO_PI - x;              /* cos(2π-x) = cos(x) */

    /* Taylor: cos(x) = Σ (-1)^n x^(2n) / (2n)!  — 12 istilah */
    double x2   = x * x;
    double term  = 1.0;
    double sum   = 1.0;
    for (int i = 1; i <= 12; i++) {
        term *= -x2 / ((2.0 * i - 1.0) * (2.0 * i));
        sum  += term;
    }
    return sum;
}

/* ── sin — Taylor series (helper internal) ────────────────────────── */
static inline double soft_sin(double x) {
    const double PI     = 3.14159265358979323846;
    const double TWO_PI = 6.28318530717958647692;

    /* Range reduction ke [-PI, PI] */
    if (x >= TWO_PI || x <= -TWO_PI) {
        long long n = (long long)(x / TWO_PI);
        x -= (double)n * TWO_PI;
    }
    if (x > PI)  x -= TWO_PI;
    if (x < -PI) x += TWO_PI;

    /* Taylor: sin(x) = x - x³/3! + x⁵/5! - … */
    double x2   = x * x;
    double term = x;
    double sum  = x;
    for (int i = 1; i <= 12; i++) {
        term *= -x2 / ((2.0 * i) * (2.0 * i + 1.0));
        sum  += term;
    }
    return sum;
}

/* ── ln — natural logarithm ───────────────────────────────────────── */
static inline double soft_ln(double x) {
    if (x <= 0.0) return -1e30;
    if (x == 1.0) return 0.0;

    const double LN2 = 0.69314718055994530941;
    int exponent = 0;
    double m = x;
    while (m >= 2.0) { m *= 0.5; exponent++; }
    while (m <  0.5) { m *= 2.0; exponent--; }

    /* Seri: ln(m) = 2 * Σ [ ((m-1)/(m+1))^(2k+1) / (2k+1) ] */
    double t  = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double sum = t;
    double pw  = t;
    for (int i = 1; i <= 25; i++) {
        pw  *= t2;
        sum += pw / (2.0 * i + 1.0);
    }
    return 2.0 * sum + (double)exponent * LN2;
}

/* ── exp — exponential ────────────────────────────────────────────── */
static inline double soft_exp(double x) {
    if (x >  700.0) return 1e308;
    if (x < -700.0) return 0.0;

    const double LN2 = 0.69314718055994530941;
    long long k = (long long)(x / LN2 + (x >= 0.0 ? 0.5 : -0.5));
    double r = x - (double)k * LN2;

    /* Taylor: exp(r) = 1 + r + r²/2! + r³/3! + … */
    double term = 1.0;
    double sum  = 1.0;
    for (int i = 1; i <= 25; i++) {
        term *= r / (double)i;
        sum  += term;
    }

    /* Kalikan dengan 2^k */
    if (k > 0) { for (long long i = 0; i < k; i++) sum *= 2.0; }
    else       { for (long long i = 0; i < -k; i++) sum *= 0.5; }
    return sum;
}

/* ── pow — x^y ────────────────────────────────────────────────────── */
static inline double soft_pow(double x, double y) {
    if (x == 0.0) return 0.0;
    if (y == 0.0) return 1.0;
    if (y == 1.0) return x;
    if (y == 2.0) return x * x;

    /* Cek integer power → repeated squaring */
    long long yi = (long long)y;
    if ((double)yi == y && yi >= 0 && yi < 64) {
        double result = 1.0;
        double base   = x;
        long long e   = yi;
        while (e > 0) {
            if (e & 1) result *= base;
            base *= base;
            e >>= 1;
        }
        return result;
    }

    /* General: x^y = exp(y · ln(x)) */
    if (x < 0.0) return 0.0;  /* Undefined untuk non-integer y */
    return soft_exp(y * soft_ln(x));
}

/* ── acos — arc cosine ────────────────────────────────────────────── */
static inline double soft_acos(double x) {
    const double PI      = 3.14159265358979323846;
    const double HALF_PI = 1.57079632679489661923;

    if (x >=  1.0) return 0.0;
    if (x <= -1.0) return PI;

    /*
     * Untuk |x| < 0.7  → acos(x) = π/2 − asin(x), asin via Taylor
     * Untuk |x| >= 0.7 → acos(x) = 2·asin(√((1−x)/2))  (lebih stabil)
     */
    if (soft_fabs(x) < 0.7) {
        /* Taylor: asin(x) = x + x³·1/(2·3) + x⁵·1·3/(2·4·5) + … */
        double x2     = x * x;
        double term   = x;
        double result = x;
        for (int i = 1; i <= 25; i++) {
            term *= x2 * (2.0 * i - 1.0) / (2.0 * i);
            result += term / (2.0 * i + 1.0);
        }
        return HALF_PI - result;
    } else {
        /* acos(x) = 2 · asin(√((1-x)/2)) untuk |x| dekat 1 */
        double s = soft_sqrt((1.0 - soft_fabs(x)) * 0.5);
        /* Taylor asin(s) dimana s kecil → konvergensi cepat */
        double s2     = s * s;
        double term   = s;
        double result = s;
        for (int i = 1; i <= 25; i++) {
            term *= s2 * (2.0 * i - 1.0) / (2.0 * i);
            result += term / (2.0 * i + 1.0);
        }
        if (x > 0.0) return 2.0 * result;
        else          return PI - 2.0 * result;
    }
}

#endif /* SOFT_MATH_H */
