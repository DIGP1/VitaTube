#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/processmgr.h>
#include "app.h"
#include "log.h"
#include "home.h"
#include "http.h"
#include "youtube.h"
#include "history.h"
#include "audio.h"
#include "video_player.h"

static int running = 1;

void app_init(void)
{
    vita2d_init();
    vita2d_set_clear_color(RGBA8(20, 20, 28, 255));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    int http_result = http_init();
    int log_result = vt_log_init();

    if (log_result >= 0) {
        vt_log(VT_LOG_INFO, "[VitaTube] Inicio de la aplicacion");
        vt_log(VT_LOG_INFO, "[HTTP] Inicializacion: %d", http_result);
    }

    if (http_result < 0) {
        vt_log(VT_LOG_ERROR, "[HTTP] Fallo al inicializar: %d", http_result);
    }

    audio_init();
    video_player_init();
    history_init();
    youtube_init();
    home_init();
}

void app_run(void)
{
    SceCtrlData pad;
    SceTouchData touch;
    SceTouchData touch_back;
    unsigned int previous_buttons = 0;

    while (running) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch_back, 1);
        unsigned int pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;

        if (pad.buttons & SCE_CTRL_START) {
            running = 0;
        }

        home_update(pressed, &touch, &touch_back);

        vita2d_start_drawing();
        vita2d_clear_screen();

        home_draw();

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
}

void app_shutdown(void)
{
    vt_log(VT_LOG_INFO, "[VitaTube] Cerrando aplicacion");
    video_player_shutdown();
    home_shutdown();
    youtube_shutdown();
    history_shutdown();
    audio_shutdown();
    vt_log_finish();
    http_shutdown();
    vita2d_fini();
}
