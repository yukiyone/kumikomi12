#ifndef _LIB_H_INCLUDE
#define _LIB_H_INCLUDE

int putc(char c); /*show 1 char*/
int puts(char *str); /*show some chars*/
int putxval(unsigned long value, int column);

void *memset(void *b, int c, long len);
void *memcpy(void *dst, const void *src, long len);
int memcmp(const void *b1, const void *b2, long len);
int strlen(const char *s);
char *strcpy(char *dst, const char *src);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, int len);

#endif
