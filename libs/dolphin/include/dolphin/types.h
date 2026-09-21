#ifndef _DOLPHIN_TYPES_H_
#define _DOLPHIN_TYPES_H_

typedef signed char s8;
typedef unsigned char u8;
typedef signed short int s16;
typedef unsigned short int u16;
#ifdef __MWERKS__
typedef signed long s32;
typedef unsigned long u32;
#else
typedef signed int s32;
typedef unsigned int u32;
#endif
typedef signed long long int s64;
typedef unsigned long long int u64;

typedef float f32;
typedef double f64;
typedef volatile f32 vf32;
typedef volatile f64 vf64;

typedef char* Ptr;

typedef int BOOL;

#define FALSE 0
#define TRUE 1

#ifndef ATTRIBUTE_ALIGN
#if defined(_MSC_VER) && !defined(__clang__)
// MSVC has no __attribute__: empty keeps layout declarations compiling
// (alignment is best-effort on the PC port; the decomp path is untouched).
#define ATTRIBUTE_ALIGN(num)
#else
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#endif
#endif

#ifndef NULL
#ifdef __cplusplus
// ((void*)0) is not a valid null pointer constant in C++ and poisons
// every header included afterwards (MSVC C2440 cascade); plain 0 is.
#define NULL 0
#else
#define NULL ((void*) 0)
#endif
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cmath.h"

#endif
