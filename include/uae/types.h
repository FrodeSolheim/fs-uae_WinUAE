/*
 * Common types used throughout the UAE source code
 * Copyright (C) 2014 Frode Solheim
 *
 * Licensed under the terms of the GNU General Public License version 2.
 * See the file 'COPYING' for full license text.
 */

#ifndef UAE_TYPES_H
#define UAE_TYPES_H

/* This file is intended to be included by external libraries as well,
 * so don't pull in too much UAE-specific stuff. */

#if 0
#include "config.h"
#endif

/* Define uae_ integer types. Prefer long long int for uae_x64 since we can
 * then use the %lld format specifier for both 32-bit and 64-bit instead of
 * the ugly PRIx64 macros. */

#include <stdint.h>

typedef int8_t uae_s8;
typedef uint8_t uae_u8;

typedef int16_t uae_s16;
typedef uint16_t uae_u16;

typedef int32_t uae_s32;
typedef uint32_t uae_u32;

typedef int64_t uae_s64;
typedef uint64_t uae_u64;

//typedef long long int uae_s64;
//typedef unsigned long long int uae_u64;

#ifdef HAVE___UINT128_T
#define HAVE_UAE_U128
typedef __uint128_t uae_u128;
#endif

/* Parts of the UAE/WinUAE code uses the bool type (from C++).
 * Including stdbool.h lets this be valid also when compiling with C. */

#ifndef __cplusplus
#include <stdbool.h>
#endif

/* Use uaecptr to represent 32-bit (or 24-bit) addresses into the Amiga
 * address space. This is a 32-bit unsigned int regardless of host arch. */

typedef uae_u32 uaecptr;

/* Define UAE character types. WinUAE uses TCHAR (typedef for wchar_t) for
 * many strings. FS-UAE always uses char-based strings in UTF-8 format.
 * Defining SIZEOF_TCHAR lets us determine whether to include overloaded
 * functions in some cases or not. */

typedef char uae_char;

#include "uae/tchar.h"

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE (!FALSE)
#endif

typedef signed long long evt_t;

#ifdef _WIN32
// Presumable these type names already exists on win32
#else
typedef int8_t INT8;
typedef int16_t INT16;
typedef int32_t INT32;
typedef int64_t INT64;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
#endif

#endif /* UAE_TYPES_H */
