/* Copyright (c) 2010-2018 the corto developers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef CORTO_OS_H
#define CORTO_OS_H

/* UNSTABLE API */

#ifdef __cplusplus
extern "C" {
#endif

#if INTPTR_MAX == INT32_MAX
#define CORTO_CPU_32BIT
#elif INTPTR_MAX == INT64_MAX
#define CORTO_CPU_64BIT
#else
#warning "corto is not supported on platforms which are neither 32- nor 64-bit."
#endif

#if defined(WIN32) || defined(WIN64)
#define CORTO_OS_WINDOWS
#elif defined(__linux__)
#define CORTO_OS_LINUX
#elif defined(__APPLE__) && defined(__MACH__)
#define CORTO_OS_OSX
#else
#warning "corto is not supported on non-unix or windows operating systems."
#endif

#ifdef __i386__
#define CORTO_CPU_STRING "x86"
#elif __x86_64__
#define CORTO_CPU_STRING "x64"
#elif defined(__arm__) && defined(CORTO_CPU_32BIT)
#define CORTO_CPU_STRING "arm"
#elif defined(__arm__) && defined(CORTO_CPU_64BIT)
#define CORTO_CPU_STRING "arm64"
#endif

#ifdef CORTO_OS_WINDOWS
#define CORTO_OS_STRING "windows"
#elif defined(CORTO_OS_LINUX)
#define CORTO_OS_STRING "linux"
#elif defined(CORTO_OS_OSX)
#define CORTO_OS_STRING "darwin"
#endif

#define CORTO_PLATFORM_STRING CORTO_CPU_STRING "-" CORTO_OS_STRING

/* Get hostname of current machine */
CORTO_EXPORT
char* corto_hostname(void);

/** Test whether string matches with current operating system.
 * This function tests for the most common occurances to denotate operating
 * systems and cpu architectures.
 *
 * @param operating system identifier.
 * @return true if matches, false if no match.
 */
CORTO_EXPORT
bool corto_os_match(char *os);

#ifdef __cplusplus
}
#endif

#endif
