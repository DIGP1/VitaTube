#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include "app.h"

/* Asigna 176 MB de heap para FFmpeg 6.0 + libmpv GXM dejando margen para USER_RW/pthreads */
unsigned int _newlib_heap_size_user = 176 * 1024 * 1024;
/* Stack por defecto de pthreads de 512 KB para hilos de decodificación */
unsigned int _pthread_stack_default_user = 512 * 1024;

int main(void)
{
    /* Overclock seguro de fábrica: 444MHz CPU, 222MHz GPU */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    app_init();
    app_run();
    app_shutdown();

    sceKernelExitProcess(0);
    return 0;
}
