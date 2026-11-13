/*
 * CAF_PLATFORM.H, Copyright (C) Stefano Crosara 2006
 *
 * Platform glue for AROS, MorphOS and AmigaOS 3.x (SAS/C 6.x).
 * Include this header AFTER the system/MUI/network headers.
 *
*/

#ifndef CAF_PLATFORM_H
#define CAF_PLATFORM_H

/*
 * Byte order
 * The config file is always stored little endian (see MemReadDword()),
 * big endian CPUs must swap before writing.
 */
#if defined(__SASC) || defined(__MORPHOS__) || defined(__mc68000__) || defined(__PPC__)
	#ifndef CPU_BIG_ENDIAN
	#define CPU_BIG_ENDIAN
	#endif
#elif defined(__AROS__) && (defined(__i386__) || defined(__x86_64__))
	#ifndef CPU_LITTLE_ENDIAN
	#define CPU_LITTLE_ENDIAN
	#endif
#endif

/* BSD errno values used by every bsdsocket.library implementation */
#define CAF_EWOULDBLOCK		35
#define CAF_EINPROGRESS		36

#ifdef __SASC
	/*
	 * NDK 3.x has no IPTR, defining it as a macro is harmless even if some
	 * header (newer MUI SDKs) already typedef'd it.
	 */
	#define IPTR ULONG

	/* sc.lib (SAS/C 6.x) has no snprintf()/vsnprintf(), use CafLib ones */
	#include <stdarg.h>
	int caf_snprintf(char *buf, int size, const char *fmt, ...);
	int caf_vsnprintf(char *buf, int size, const char *fmt, va_list ap);
	#define snprintf  caf_snprintf
	#define vsnprintf caf_vsnprintf

	/* AmiTCP SDK: FD_ZERO() uses bzero(), SAS/C <string.h> maps it to memset() */
	#include <string.h>

	/*
	 * Hook entry points: on 68k MUI calls hooks with the hook in A0, the object
	 * in A2 and the message in A1. __saveds reloads A4 (near data model),
	 * harmless with DATA=FAR.
	 */
	#define CAF_HOOK(rettype, name, objtype, obj, msgtype, msg) \
		rettype __saveds __asm name(register __a0 struct Hook *hook, \
									register __a2 objtype obj, \
									register __a1 msgtype msg)
#endif

#endif
