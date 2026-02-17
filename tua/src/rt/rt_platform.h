#ifndef _TUA_RT_PLATFORM_H_
#define _TUA_RT_PLATFORM_H_

/*
 * Prefer these macros over raw compiler/OS macros in tua_rt code.
 * Build systems may also define them explicitly.
 */

#if !defined(TUA_OS_DARWIN) && defined(__APPLE__)
#define TUA_OS_DARWIN 1
#endif

#if !defined(TUA_OS_LINUX) && defined(__linux__)
#define TUA_OS_LINUX 1
#endif

#if !defined(TUA_OS_WINDOWS) && defined(_WIN32)
#define TUA_OS_WINDOWS 1
#endif

#if !defined(TUA_OS_POSIX) && (defined(TUA_OS_DARWIN) || defined(TUA_OS_LINUX))
#define TUA_OS_POSIX 1
#endif

#endif

