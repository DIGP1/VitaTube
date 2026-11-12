#ifndef HISTORY_H
#define HISTORY_H

#include "youtube.h"

#define HISTORY_MAX 48

int history_init(void);
void history_shutdown(void);
int history_add(const YoutubeVideo *video);
unsigned int history_count(void);
const YoutubeVideo *history_get(unsigned int index);
void history_clear(void);

#endif
