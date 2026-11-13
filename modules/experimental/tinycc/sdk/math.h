#ifndef SOLAR_OS_C_MATH_H
#define SOLAR_OS_C_MATH_H
#include <stdint.h>
#define INFINITY (((union {uint32_t bits;float value;}){.bits=0x7f800000U}).value)
#define NAN (((union {uint32_t bits;float value;}){.bits=0x7fc00000U}).value)
#define HUGE_VAL ((double)INFINITY)
#define HUGE_VALF INFINITY
double floor(double);
double fabs(double);
double ldexp(double,int);
float expf(float);
float floorf(float);
float fmodf(float,float);
float logf(float);
float powf(float,float);
float sqrtf(float);
float fabsf(float);
int solar_os_c_isnan(double);
int solar_os_c_isinf(double);
int solar_os_c_isfinite(double);
int solar_os_c_signbit(double);
#define isnan(value) solar_os_c_isnan(value)
#define isinf(value) solar_os_c_isinf(value)
#define isfinite(value) solar_os_c_isfinite(value)
#define signbit(value) solar_os_c_signbit(value)
#endif
