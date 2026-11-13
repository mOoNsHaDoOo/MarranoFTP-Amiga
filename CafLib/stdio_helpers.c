/*
 * STDIO_HELPERS.C, Copyright (C) Stefano Crosara 2006
 *
 * Small, bounded vsnprintf()/snprintf() replacement.
 * Needed by SAS/C 6.x whose sc.lib has no snprintf(), usable everywhere.
 *
 * Supports: flags '-' '0' ' ' '+' '#', width and precision (also '*'),
 *           length modifiers 'h' 'l', conversions d i u x X o c s p %
 *
*/

#include <stdarg.h>

#include <caf/caf_types.h>

#define PUTC(c) do { if (pos+1 < size) buf[pos] = (char)(c); pos++; } while (0)

int caf_vsnprintf(char *buf, int size, const char *fmt, va_list ap)
{
	int pos = 0;

	if (size < 0)
		size = 0;

	while (*fmt) {
		int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
		int width = 0, prec = -1, is_long = 0;
		char numbuf[40], *str = 0, sign = 0;
		int len = 0, i, base = 10, upper = 0, is_signed = 0, is_num = 0;
		unsigned long uval = 0;

		if (*fmt != '%') {
			PUTC(*fmt);
			fmt++;
			continue;
		}
		fmt++;

		/* Flags */
		for (;;) {
			if (*fmt == '-') left = 1;
			else if (*fmt == '0') zero = 1;
			else if (*fmt == '+') plus = 1;
			else if (*fmt == ' ') space = 1;
			else if (*fmt == '#') alt = 1;
			else break;
			fmt++;
		}

		/* Width */
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				left = 1;
				width = -width;
			}
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = width*10 + (*fmt++ - '0');
		}

		/* Precision */
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec*10 + (*fmt++ - '0');
			}
		}

		/* Length */
		while (*fmt == 'l' || *fmt == 'h') {
			if (*fmt == 'l')
				is_long = 1;
			fmt++;
		}

		switch (*fmt) {
			case 'd':
			case 'i': {
				long v = is_long ? va_arg(ap, long) : (long) va_arg(ap, int);
				is_num = is_signed = 1;
				if (v < 0) {
					sign = '-';
					uval = (unsigned long)(-(v+1)) + 1;
				} else {
					uval = (unsigned long) v;
					if (plus) sign = '+';
					else if (space) sign = ' ';
				}
				break;
			}

			case 'u':
				uval = is_long ? va_arg(ap, unsigned long) : (unsigned long) va_arg(ap, unsigned int);
				is_num = 1;
				break;

			case 'X':
				upper = 1;
				/* fall through */
			case 'x':
				uval = is_long ? va_arg(ap, unsigned long) : (unsigned long) va_arg(ap, unsigned int);
				base = 16;
				is_num = 1;
				break;

			case 'o':
				uval = is_long ? va_arg(ap, unsigned long) : (unsigned long) va_arg(ap, unsigned int);
				base = 8;
				is_num = 1;
				break;

			case 'p':
				uval = (unsigned long) va_arg(ap, void *);
				base = 16;
				is_num = 1;
				alt = 1;
				break;

			case 'c':
				numbuf[0] = (char) va_arg(ap, int);
				str = numbuf;
				len = 1;
				break;

			case 's':
				str = va_arg(ap, char *);
				if (!str)
					str = "(null)";
				for (len = 0; str[len] && (prec < 0 || len < prec); len++)
					;
				break;

			case '%':
				PUTC('%');
				fmt++;
				continue;

			case 0:
				/* Truncated format string */
				continue;

			default:
				/* Unknown conversion, print it as is */
				PUTC('%');
				PUTC(*fmt);
				fmt++;
				continue;
		}
		fmt++;

		if (is_num) {
			/* Convert the number, right to left */
			char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
			int nlen = 0, pad_zero, total, prefix_len;
			char prefix[3];

			do {
				numbuf[sizeof(numbuf)-1-nlen] = digits[uval % base];
				uval /= base;
				nlen++;
			} while (uval && nlen < (int)sizeof(numbuf)-1);

			if (prec == 0 && nlen == 1 && numbuf[sizeof(numbuf)-1] == '0')
				nlen = 0;

			prefix_len = 0;
			if (sign)
				prefix[prefix_len++] = sign;
			if (alt && base == 16) {
				prefix[prefix_len++] = '0';
				prefix[prefix_len++] = upper ? 'X' : 'x';
			}

			pad_zero = (prec > nlen) ? prec-nlen : 0;
			if (zero && !left && prec < 0 && width > nlen+prefix_len)
				pad_zero = width-nlen-prefix_len;
			if (alt && base == 8 && pad_zero == 0)
				pad_zero = 1;

			total = prefix_len + pad_zero + nlen;
			if (!left)
				for (i = total; i < width; i++)
					PUTC(' ');
			for (i = 0; i < prefix_len; i++)
				PUTC(prefix[i]);
			for (i = 0; i < pad_zero; i++)
				PUTC('0');
			for (i = 0; i < nlen; i++)
				PUTC(numbuf[sizeof(numbuf)-nlen+i]);
			if (left)
				for (i = total; i < width; i++)
					PUTC(' ');
			(void) is_signed;
		} else {
			if (!left)
				for (i = len; i < width; i++)
					PUTC(' ');
			for (i = 0; i < len; i++)
				PUTC(str[i]);
			if (left)
				for (i = len; i < width; i++)
					PUTC(' ');
		}
	}

	/* Always terminate (C99 semantics), returns the untruncated length */
	if (size > 0)
		buf[(pos < size) ? pos : size-1] = 0;

	return pos;
}

int caf_snprintf(char *buf, int size, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = caf_vsnprintf(buf, size, fmt, ap);
	va_end(ap);

	return ret;
}
