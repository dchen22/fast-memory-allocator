#ifndef TLSF_H
#define TLSF_H

#include <stdint.h>

int tlsf_init(void);
void *tlsf_malloc(uint32_t n);
void tlsf_free(void *ptr);

#endif
