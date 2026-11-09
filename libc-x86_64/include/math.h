// ============================================================================
// math.h — стандартная математика. Этап 39: минимальный набор. Полный libm
// (комплексные, специальные функции) — этап 54 (когда понадобится Doom/Quake).
// ============================================================================
#ifndef _MATH_H
#define _MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define M_E         2.7182818284590452354
#define M_LOG2E     1.4426950408889634074
#define M_LOG10E    0.43429448190325182765
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_1_PI      0.31830988618379067154
#define M_2_PI      0.63661977236758134308
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440

#define HUGE_VAL    (__builtin_huge_val())
#define INFINITY    (__builtin_inff())
#define NAN         (__builtin_nanf(""))

double fabs (double x);
double floor(double x);
double ceil (double x);
double sqrt (double x);
double pow  (double base, double exp);
double exp  (double x);
double log  (double x);
double log2 (double x);
double log10(double x);
double sin  (double x);
double cos  (double x);
double tan  (double x);
double asin (double x);
double acos (double x);
double atan (double x);
double atan2(double y, double x);
double fmod (double x, double y);

float  fabsf (float x);
float  floorf(float x);
float  ceilf (float x);
float  sqrtf (float x);
float  sinf  (float x);
float  cosf  (float x);

int    abs   (int x);

#ifdef __cplusplus
}
#endif
#endif
