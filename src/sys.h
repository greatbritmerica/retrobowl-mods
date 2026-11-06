// sys.h - C library declarations. Uses the iOS SDK headers when available; otherwise (Linux
// cross-build, -DRB_FREESTANDING) declares the handful of libSystem functions we use.
#pragma once
#ifndef RB_FREESTANDING
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#else
#include <stdarg.h>
#include <stddef.h>
typedef struct __sFILE FILE;
typedef long time_t;
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
int fprintf(FILE *f, const char *fmt, ...);
int fputs(const char *s, FILE *f);
int fflush(FILE *f);
char *fgets(char *s, int n, FILE *f);
int sscanf(const char *s, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
char *getenv(const char *name);
time_t time(time_t *t);
void *memset(void *p, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strstr(const char *h, const char *n);
double pow(double x, double y);
void *malloc(size_t n);
void free(void *p);
unsigned int arc4random_uniform(unsigned int upper);
double atan2(double y, double x);
double sqrt(double x);
double sin(double x);
double cos(double x);
double fabs(double x);
double floor(double x);
double hypot(double x, double y);
#endif
