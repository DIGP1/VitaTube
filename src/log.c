#include "log.h"
#include <debugnet.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

static int log_ready = 0;
static FILE *s_log_file = NULL;

int vt_log_init(void)
{
    int result = debugNetInit("192.168.31.8", 18194, DEBUG);
    log_ready = (result >= 0);

    mkdir("ux0:data", 0777);
    mkdir("ux0:data/VitaTube", 0777);
    s_log_file = fopen("ux0:data/VitaTube/vitatube.log", "w");
    if (s_log_file) {
        fprintf(s_log_file, "=== VitaTube Log Iniciado ===\n");
        fflush(s_log_file);
    }

    return 0;
}

void vt_log(int level, const char *format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (log_ready) {
        debugNetPrintf(level, "%s\n", message);
    }

    if (s_log_file) {
        fprintf(s_log_file, "%s\n", message);
        fflush(s_log_file);
    }
}

void vt_log_finish(void)
{
    if (s_log_file) {
        fprintf(s_log_file, "=== VitaTube Log Finalizado ===\n");
        fclose(s_log_file);
        s_log_file = NULL;
    }
    if (log_ready) {
        debugNetFinish();
        log_ready = 0;
    }
}
