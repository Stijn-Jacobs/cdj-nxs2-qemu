/* The two macros tic6x.h needs from libiberty's symcat.h. */
#ifndef SYMCAT_LITE_H
#define SYMCAT_LITE_H
#define CONCAT2(a,b)	 a##b
#define CONCAT3(a,b,c)	 a##b##c
#define CONCAT4(a,b,c,d) a##b##c##d
#define CONCAT6(a,b,c,d,e,f) a##b##c##d##e##f
#define XSTRING(s) #s
#define STRINGX(s) #s
#endif
