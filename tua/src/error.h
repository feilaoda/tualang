#ifndef _ERROR_H_
#define _ERROR_H_
#include <stdio.h>

#define error(...) fprintf(stdout,"ERROR: (%s:%d) ", __FILE__, __LINE__); printf(__VA_ARGS__)

#endif