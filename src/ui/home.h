#ifndef HOME_H
#define HOME_H

#include <psp2/touch.h>

void home_init(void);
void home_update(unsigned int pressed, const SceTouchData *touch_front, const SceTouchData *touch_back);
void home_draw(void);
void home_shutdown(void);

#endif
