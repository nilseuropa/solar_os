/* SPDX-License-Identifier: Apache-2.0
 * Private compiler support, linked from the target toolchain at build time.
 * No firmware imports: generated programs receive only the required bundle. */
#include <stdint.h>
int64_t __solar_cc_divdi3(int64_t a,int64_t b) { return a/b; }
uint64_t __solar_cc_udivdi3(uint64_t a,uint64_t b) { return a/b; }
int64_t __solar_cc_moddi3(int64_t a,int64_t b) { return a%b; }
uint64_t __solar_cc_umoddi3(uint64_t a,uint64_t b) { return a%b; }
int64_t __solar_cc_ashrdi3(int64_t a,int b) { return a>>b; }
uint64_t __solar_cc_lshrdi3(uint64_t a,int b) { return a>>b; }
uint64_t __solar_cc_ashldi3(uint64_t a,int b) { return a<<b; }
uint64_t __solar_cc_u64_mul(uint64_t a,uint64_t b) {return a*b;}
#define WIDE_COMPARE(kind,type) \
int __solar_cc_##kind##_eq(type a,type b){return a==b;} \
int __solar_cc_##kind##_ne(type a,type b){return a!=b;} \
int __solar_cc_##kind##_lt(type a,type b){return a<b;} \
int __solar_cc_##kind##_le(type a,type b){return a<=b;} \
int __solar_cc_##kind##_gt(type a,type b){return a>b;} \
int __solar_cc_##kind##_ge(type a,type b){return a>=b;}
WIDE_COMPARE(u64,uint64_t)
WIDE_COMPARE(i64,int64_t)
#define FP_BINARY(kind,type) \
type __solar_cc_##kind##_add(type a,type b){return a+b;} \
type __solar_cc_##kind##_sub(type a,type b){return a-b;} \
type __solar_cc_##kind##_mul(type a,type b){return a*b;} \
type __solar_cc_##kind##_div(type a,type b){return a/b;} \
int __solar_cc_##kind##_eq(type a,type b){return a==b;} \
int __solar_cc_##kind##_ne(type a,type b){return a!=b;} \
int __solar_cc_##kind##_lt(type a,type b){return a<b;} \
int __solar_cc_##kind##_le(type a,type b){return a<=b;} \
int __solar_cc_##kind##_gt(type a,type b){return a>b;} \
int __solar_cc_##kind##_ge(type a,type b){return a>=b;}
FP_BINARY(f32,float)
FP_BINARY(f64,double)
#define CONVERT(name,from,to) to __solar_cc_##name(from value){return (to)value;}
CONVERT(i32_f32,int32_t,float) CONVERT(u32_f32,uint32_t,float)
CONVERT(i64_f32,int64_t,float) CONVERT(u64_f32,uint64_t,float)
CONVERT(i32_f64,int32_t,double) CONVERT(u32_f64,uint32_t,double)
CONVERT(i64_f64,int64_t,double) CONVERT(u64_f64,uint64_t,double)
CONVERT(f32_i32,float,int32_t) CONVERT(f32_u32,float,uint32_t)
CONVERT(f32_i64,float,int64_t) CONVERT(f32_u64,float,uint64_t)
CONVERT(f64_i32,double,int32_t) CONVERT(f64_u32,double,uint32_t)
CONVERT(f64_i64,double,int64_t) CONVERT(f64_u64,double,uint64_t)
CONVERT(f32_f64,float,double) CONVERT(f64_f32,double,float)
