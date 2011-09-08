#ifndef IMANAGER_H
#define IMANAGER_H

#include "iomanagers.h"
#include "threading.h"

typedef struct snet_buffer snet_buffer_t;

void SNetInputManagerInit(void);
void SNetInputManagerStart(void);
void SNetInputManagerNewIn(snet_dest_t *dest, snet_stream_t *stream);
void SNetInputManagerUpdate(void);
#endif
