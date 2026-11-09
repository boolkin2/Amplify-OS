// ============================================================================
// math.c — минимальный libm (этап 39). Полная замена — этап 43 (3D/GL).
// Все функции — на double (SSE2 либо x87 — компилятор сам выберет, libc
// собирается с -mno-sse только для kernel; userspace может использовать SSE).
// ============================================================================
#include <math.h>

double fabs(double x)  { return x < 0 ? -x : x; }
double floor(double x) { long i = (long)x; if ((double)i > x) i--; return (double)i; }
double ceil(double x)  { long i = (long)x; if ((double)i < x) i++; return (double)i; }

// Простой sqrt через Ньютоновские итерации (хватит для 39-го).
double sqrt(double x) {
    if (x < 0) return 0.0 / 0.0;     /* NaN */
    if (x == 0) return 0;
    double r = x;
    for (int i = 0; i < 40; i++) r = 0.5 * (r + x / r);
    return r;
}

// Тейлор для sin/cos, ограничение диапазона через mod-2π.
static double mod2pi(double x) {
    const double TWO_PI = 6.283185307179586;
    while (x >  3.141592653589793) x -= TWO_PI;
    while (x < -3.141592653589793) x += TWO_PI;
    return x;
}

double sin(double x) {
    x = mod2pi(x);
    double term = x, sum = x;
    double x2 = x * x;
    for (int n = 1; n < 15; n++) {
        term *= -x2 / ((double)(2*n) * (double)(2*n + 1));
        sum  += term;
    }
    return sum;
}

double cos(double x) {
    x = mod2pi(x);
    double term = 1.0, sum = 1.0;
    double x2 = x * x;
    for (int n = 1; n < 15; n++) {
        term *= -x2 / ((double)(2*n - 1) * (double)(2*n));
        sum  += term;
    }
    return sum;
}

double tan(double x) { double c = cos(x); if (c == 0) return 0.0; return sin(x) / c; }

// exp/log — упрощённые, на этапе 43/54 заменим точными.
double exp(double x) {
    double sum = 1.0, term = 1.0;
    for (int n = 1; n < 25; n++) { term *= x / n; sum += term; }
    return sum;
}

double log(double x) {
    if (x <= 0) return 0.0 / 0.0;
    // Range-reduction: x = 2^k * m, m in [1,2). log(x) = k*ln2 + log(m).
    double k = 0;
    while (x > 2) { x /= 2; k += 1; }
    while (x < 1) { x *= 2; k -= 1; }
    double y = (x - 1) / (x + 1);
    double y2 = y * y;
    double r = 0;
    for (int n = 0; n < 15; n++) {
        r += (1.0 / (2 * n + 1)) * y;
        y *= y2;
    }
    return 2 * r + k * 0.6931471805599453;
}

double log2 (double x) { return log(x) / 0.6931471805599453; }
double log10(double x) { return log(x) / 2.302585092994046; }
double pow  (double base, double exp_) { return exp(exp_ * log(base)); }

// atan / asin / acos — простые приближения через ряд для |x| ≤ 0.5,
// reflection для прочих.
double atan(double x) {
    int neg = 0; if (x < 0) { neg = 1; x = -x; }
    int inv = 0; if (x > 1) { inv = 1; x = 1 / x; }
    double sum = 0, term = x; double x2 = x * x;
    for (int n = 0; n < 30; n++) {
        sum += term / (2*n + 1);
        term *= -x2;
    }
    if (inv) sum = 1.5707963267948966 - sum;
    return neg ? -sum : sum;
}

double atan2(double y, double x) {
    if (x > 0) return atan(y / x);
    if (x < 0 && y >= 0) return atan(y / x) + 3.141592653589793;
    if (x < 0 && y <  0) return atan(y / x) - 3.141592653589793;
    if (x == 0 && y >  0) return 1.5707963267948966;
    if (x == 0 && y <  0) return -1.5707963267948966;
    return 0;
}

double asin(double x) { return atan(x / sqrt(1 - x*x)); }
double acos(double x) { return 1.5707963267948966 - asin(x); }

double fmod(double x, double y) {
    if (y == 0) return 0.0 / 0.0;
    double n = floor(x / y);
    return x - n * y;
}

float  fabsf (float x)  { return x < 0 ? -x : x; }
float  floorf(float x)  { return (float)floor((double)x); }
float  ceilf (float x)  { return (float)ceil((double)x); }
float  sqrtf (float x)  { return (float)sqrt((double)x); }
float  sinf  (float x)  { return (float)sin((double)x); }
float  cosf  (float x)  { return (float)cos((double)x); }
