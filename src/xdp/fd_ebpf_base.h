#define asm    __asm__
#define typeof __typeof__

typedef   signed char  schar;
typedef unsigned char  uchar;
typedef unsigned short ushort;
typedef unsigned int   uint;
typedef unsigned long  ulong;

#define FD_LIKELY(c)   __builtin_expect( !!(c), 1L )
#define FD_UNLIKELY(c) __builtin_expect( !!(c), 0L )
