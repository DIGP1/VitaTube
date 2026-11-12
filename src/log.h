#ifndef VITATUBE_LOG_H
#define VITATUBE_LOG_H

#define VT_LOG_INFO 1
#define VT_LOG_ERROR 2
#define VT_LOG_WARN 2
#define VT_LOG_DEBUG 3

int vt_log_init(void);
void vt_log(int level, const char *format, ...);
void vt_log_finish(void);

#endif
