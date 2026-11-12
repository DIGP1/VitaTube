#include "video_player.h"
#include "http.h"
#include "log.h"
#include "../audio.h"

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gxm.h>
#include <mpv/stream_cb.h>

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <vita2d.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <locale.h>
#include <malloc.h>
#include <sys/stat.h>

#define CACHE_STREAM_PATH "ux0:data/VitaTube/stream.mp4"
#define INITIAL_BUFFER_BYTES (1536 * 1024) /* 1.5 MB de buffer para inicio inmediato */

typedef enum PlayerStage {
    PLAYER_STAGE_IDLE = 0,
    PLAYER_STAGE_DOWNLOADING,
    PLAYER_STAGE_PLAYING,
    PLAYER_STAGE_PAUSED,
    PLAYER_STAGE_FAILED,
    PLAYER_STAGE_FINISHED
} PlayerStage;

/* Instancias de libmpv y render GXM */
static mpv_handle *s_mpv = NULL;
static mpv_render_context *s_mpv_context = NULL;
static vita2d_texture *s_render_texture = NULL;
static mpv_gxm_fbo s_fbo;
static int s_flip_y = 1;
static mpv_render_param s_render_params[3];

/* Estados de control */
static VideoPlayerStatus s_status = VPLAYER_IDLE;
static PlayerStage s_stage = PLAYER_STAGE_IDLE;
static char s_current_url[4096] = {0};
static char s_error_msg[160] = {0};
static char s_mpv_fatal_err[160] = {0};
static char s_notice_msg[96] = {0};
static unsigned int s_notice_timer = 0;
static volatile int s_redraw_needed = 0;
static volatile int s_has_rendered_frame = 0;
static volatile int s_first_frame_logged = 0;
static volatile int s_file_loaded = 0;
static volatile int s_ui_audio_suspended = 0;
static unsigned int s_seek_grace_frames = 0;
static int s_has_queued_seek = 0;
static uint64_t s_queued_seek_ms = 0;
static unsigned int s_seek_debounce_frames = 0;

static uint64_t s_current_time_ms = 0;
static uint64_t s_duration_ms = 0;
static uint64_t s_pending_seek_ms = 0;
static unsigned int s_current_volume = 80;

/* Hilo de descarga progresiva */
static SceUID s_download_thread_id = -1;
static volatile int s_download_stop = 0;
static volatile int s_download_completed = 0;
static volatile int s_download_result = 0;
static volatile unsigned int s_bytes_downloaded = 0;

/* Estado del lector de streaming progresivo para protocolo vts:// */
static SceUID s_io_mutex = -1;
static SceUID s_reader_fd = -1;
static int64_t s_read_pos = 0;

static void lock_io(void)
{
    if (s_io_mutex >= 0) {
        sceKernelLockMutex(s_io_mutex, 1, NULL);
    }
}

static void unlock_io(void)
{
    if (s_io_mutex >= 0) {
        sceKernelUnlockMutex(s_io_mutex, 1);
    }
}

static void set_player_error(const char *msg)
{
    if (msg && msg[0]) {
        strncpy(s_error_msg, msg, sizeof(s_error_msg) - 1);
        s_error_msg[sizeof(s_error_msg) - 1] = '\0';
    } else {
        snprintf(s_error_msg, sizeof(s_error_msg), "Error al reproducir el video");
    }
    s_stage = PLAYER_STAGE_FAILED;
    s_status = VPLAYER_ERROR;
}

static void set_player_notice(const char *msg, unsigned int frames)
{
    if (msg && msg[0]) {
        strncpy(s_notice_msg, msg, sizeof(s_notice_msg) - 1);
        s_notice_msg[sizeof(s_notice_msg) - 1] = '\0';
        s_notice_timer = frames;
    } else {
        s_notice_msg[0] = '\0';
        s_notice_timer = 0;
    }
}

static void log_mem_stats(const char *tag)
{
    struct mallinfo mi = mallinfo();
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) < 0) {
        memset(&info, 0, sizeof(info));
    }
    vt_log(VT_LOG_DEBUG,
           "[Mem:%s] arena=%u used=%u free=%u | user=%d cdram=%d",
           tag ? tag : "?",
           (unsigned int)mi.arena,
           (unsigned int)mi.uordblks,
           (unsigned int)mi.fordblks,
           info.size_user,
           info.size_cdram);
}

static int64_t vts_read_fn(void *cookie, char *buf, uint64_t nbytes)
{
    (void)cookie;
    if (!buf || nbytes == 0) return 0;
    if (s_download_stop) return -1;

    /* Si el lector alcanza el frente de bytes descargados, esperar datos nuevos */
    while (1) {
        lock_io();
        int64_t pos = s_read_pos;
        unlock_io();

        unsigned int avail_bytes = s_bytes_downloaded;
        if (pos < (int64_t)avail_bytes) {
            break;
        }
        if (s_download_completed) {
            if (s_download_result <= 0 || avail_bytes == 0) {
                return -1;
            }
            return 0; /* Fin de archivo real */
        }
        if (s_download_stop) {
            return -1;
        }
        sceKernelDelayThread(15 * 1000); /* 15 ms */
    }

    lock_io();
    if (s_reader_fd < 0) {
        s_reader_fd = sceIoOpen(CACHE_STREAM_PATH, SCE_O_RDONLY, 0);
        if (s_reader_fd < 0) {
            unlock_io();
            return -1;
        }
    }

    int64_t avail = (int64_t)s_bytes_downloaded - s_read_pos;
    if (avail <= 0) {
        unlock_io();
        return 0;
    }

    size_t to_read = (nbytes < (uint64_t)avail) ? (size_t)nbytes : (size_t)avail;

    if (sceIoLseek(s_reader_fd, (SceOff)s_read_pos, SCE_SEEK_SET) < 0) {
        unlock_io();
        return -1;
    }
    int r = sceIoRead(s_reader_fd, buf, to_read);
    if (r > 0) {
        s_read_pos += (int64_t)r;
        unlock_io();
        return (int64_t)r;
    }
    unlock_io();

    return (r < 0) ? -1 : 0;
}

static int64_t vts_seek_fn(void *cookie, int64_t offset)
{
    (void)cookie;
    if (offset < 0) return MPV_ERROR_UNSUPPORTED;

    lock_io();
    if (offset == 0) {
        s_read_pos = 0;
        unlock_io();
        return 0;
    }
    if (offset <= (int64_t)s_bytes_downloaded) {
        s_read_pos = offset;
        unlock_io();
        return offset;
    }
    unlock_io();
    return MPV_ERROR_UNSUPPORTED;
}

static int64_t vts_size_fn(void *cookie)
{
    (void)cookie;
    if (s_download_completed && s_bytes_downloaded > 0) {
        return (int64_t)s_bytes_downloaded;
    }
    return MPV_ERROR_UNSUPPORTED;
}

static void vts_close_fn(void *cookie)
{
    (void)cookie;
    lock_io();
    if (s_reader_fd >= 0) {
        sceIoClose(s_reader_fd);
        s_reader_fd = -1;
    }
    unlock_io();
}

static int vts_open_fn(void *user_data, char *uri, mpv_stream_cb_info *info)
{
    (void)user_data;
    (void)uri;
    info->cookie = NULL;
    info->read_fn = vts_read_fn;
    info->seek_fn = vts_seek_fn;
    info->size_fn = vts_size_fn;
    info->close_fn = vts_close_fn;
    info->cancel_fn = NULL;

    lock_io();
    s_read_pos = 0;
    if (s_reader_fd >= 0) {
        sceIoClose(s_reader_fd);
        s_reader_fd = -1;
    }
    unlock_io();
    return 0;
}

/* Callback de actualización de cuadro invocado por libmpv */
static void on_mpv_render_update(void *ctx)
{
    (void)ctx;
    s_redraw_needed = 1;
}

static int downloader_thread(SceSize args, void *arg)
{
    (void)args;
    (void)arg;

    mkdir("ux0:data/VitaTube", 0777);
    remove(CACHE_STREAM_PATH);

    int total = http_download_file(s_current_url, CACHE_STREAM_PATH,
                                   &s_download_stop, &s_bytes_downloaded);
    s_download_result = total;
    s_download_completed = 1;

    vt_log(VT_LOG_INFO, "[VideoPlayer] Descarga de stream completada (%d bytes)", total);
    return 0;
}

static int ensure_mpv_initialized(void)
{
    if (s_mpv && s_mpv_context && s_render_texture) {
        return 0;
    }

    setlocale(LC_ALL, "C");
    setlocale(LC_NUMERIC, "C");

    vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 1: Inicializando libmpv...");
    log_mem_stats("mpv_init_before");

    if (!s_mpv) {
        vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 1.1: mpv_create()...");
        s_mpv = mpv_create();
        if (!s_mpv) {
            vt_log(VT_LOG_ERROR, "[VideoPlayer] Fallo al crear contexto mpv");
            return -1;
        }
        vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 1.2: mpv_create() OK: %p", s_mpv);

        /* Opciones de decodificación y rendimiento optimizadas para PS Vita */
        mpv_set_option_string(s_mpv, "vd-lavc-threads", "2");
        mpv_set_option_string(s_mpv, "framedrop", "yes");
        mpv_set_option_string(s_mpv, "video-sync", "audio");
        mpv_set_option_string(s_mpv, "video-latency-hacks", "yes");
        mpv_set_option_string(s_mpv, "demuxer-lavf-probesize", "524288");
        mpv_set_option_string(s_mpv, "demuxer-lavf-analyzeduration", "1.5");
        mpv_set_option_string(s_mpv, "demuxer-lavf-probescore", "24");
        mpv_set_option_string(s_mpv, "keep-open", "yes");
        /* Desactivar OSD y subtitulos internos de libmpv (evita crash GXM/libass al hacer seek) */
        mpv_set_option_string(s_mpv, "osd-level", "0");
        mpv_set_option_string(s_mpv, "osd-bar", "no");
        mpv_set_option_string(s_mpv, "sub", "no");
        /* Cache acotado en RAM (vts:// ya cachea en disco) para evitar OOM al hacer seek */
        mpv_set_option_string(s_mpv, "cache", "yes");
        mpv_set_option_string(s_mpv, "demuxer-max-bytes", "8MiB");
        mpv_set_option_string(s_mpv, "demuxer-max-back-bytes", "2MiB");
        mpv_set_option_string(s_mpv, "cache-secs", "60");
        mpv_set_option_string(s_mpv, "demuxer-readahead-secs", "10");
        mpv_set_option_string(s_mpv, "video-reversal-buffer", "0");
        mpv_set_option_string(s_mpv, "audio-reversal-buffer", "0");
        mpv_set_option_string(s_mpv, "hr-seek", "no");
        vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 1.3: opciones configuradas");

        vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 1.4: mpv_initialize()...");
        int r = mpv_initialize(s_mpv);
        if (r < 0) {
            vt_log(VT_LOG_ERROR, "[VideoPlayer] Fallo mpv_initialize: %d (%s)",
                   r, mpv_error_string(r));
            mpv_destroy(s_mpv);
            s_mpv = NULL;
            return -1;
        }
        mpv_request_log_messages(s_mpv, "warn");
        vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 1.4: mpv_initialize() OK");

        int cb_ret = mpv_stream_cb_add_ro(s_mpv, "vts", NULL, vts_open_fn);
        if (cb_ret < 0) {
            vt_log(VT_LOG_WARN, "[VideoPlayer] mpv_stream_cb_add_ro: %d (%s)",
                   cb_ret, mpv_error_string(cb_ret));
        } else {
            vt_log(VT_LOG_INFO, "[VideoPlayer] Protocolo vts:// registrado con exito");
        }

        SceGxmContext *gxm_ctx = vita2d_get_context();
        SceGxmShaderPatcher *gxm_sp = vita2d_get_shader_patcher();
        vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 1.5: vita2d context=%p, sp=%p", gxm_ctx, gxm_sp);

        static mpv_gxm_init_params gxm_params;
        memset(&gxm_params, 0, sizeof(gxm_params));
        gxm_params.context = gxm_ctx;
        gxm_params.shader_patcher = gxm_sp;
        gxm_params.buffer_index = 0;
        gxm_params.msaa = SCE_GXM_MULTISAMPLE_NONE;

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_GXM},
            {MPV_RENDER_PARAM_GXM_INIT_PARAMS, &gxm_params},
            {MPV_RENDER_PARAM_INVALID, NULL}
        };

        vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 1.6: mpv_render_context_create()...");
        r = mpv_render_context_create(&s_mpv_context, s_mpv, params);
        if (r < 0) {
            vt_log(VT_LOG_ERROR, "[VideoPlayer] Fallo mpv_render_context_create: %d (%s)",
                   r, mpv_error_string(r));
            mpv_destroy(s_mpv);
            s_mpv = NULL;
            return -1;
        }
        vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 1.6: mpv_render_context_create OK: %p", s_mpv_context);

        mpv_render_context_set_update_callback(s_mpv_context, on_mpv_render_update, NULL);
        vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 1.7: callback de render configurado");
    }

    if (!s_render_texture) {
        vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 2: creando render target texture 960x544...");
        s_render_texture = vita2d_create_empty_texture_rendertarget(960, 544, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
        if (!s_render_texture) {
            vt_log(VT_LOG_ERROR, "[VideoPlayer] Fallo al crear render target texture vita2d");
            return -1;
        }
        vita2d_texture_set_filters(s_render_texture, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        vt_log(VT_LOG_INFO, "[VideoPlayer] Paso 2.1: render target texture OK: %p (rtgt=%p, sfc=%p, sfd=%p)",
               s_render_texture, s_render_texture->gxm_rtgt, &s_render_texture->gxm_sfc, &s_render_texture->gxm_sfd);

        s_fbo.render_target = s_render_texture->gxm_rtgt;
        s_fbo.color_surface = &s_render_texture->gxm_sfc;
        s_fbo.depth_stencil_surface = &s_render_texture->gxm_sfd;
        s_fbo.w = 960;
        s_fbo.h = 544;
        s_fbo.format = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_RGBA;

        s_render_params[0].type = MPV_RENDER_PARAM_FLIP_Y;
        s_render_params[0].data = &s_flip_y;
        s_render_params[1].type = MPV_RENDER_PARAM_GXM_FBO;
        s_render_params[1].data = &s_fbo;
        s_render_params[2].type = MPV_RENDER_PARAM_INVALID;
        s_render_params[2].data = NULL;
    }

    vt_log(VT_LOG_INFO, "[VideoPlayer] libmpv GXM inicializado correctamente");
    log_mem_stats("mpv_init_after");
    return 0;
}

int video_player_init(void)
{
    int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
    if (ret < 0) {
        vt_log(VT_LOG_WARN, "[VideoPlayer] Modulo AVCDEC ya cargado o ret=0x%08X", (unsigned int)ret);
    }
    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
    if (ret < 0) {
        vt_log(VT_LOG_WARN, "[VideoPlayer] Modulo AVPLAYER ya cargado o ret=0x%08X", (unsigned int)ret);
    }
    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIOCODEC);
    if (ret < 0) {
        vt_log(VT_LOG_WARN, "[VideoPlayer] Modulo AUDIOCODEC ya cargado o ret=0x%08X", (unsigned int)ret);
    }

    if (s_io_mutex < 0) {
        s_io_mutex = sceKernelCreateMutex("VitaTubeVtsMutex", 0, 0, NULL);
    }

    s_status = VPLAYER_IDLE;
    s_stage = PLAYER_STAGE_IDLE;
    s_error_msg[0] = '\0';
    s_mpv_fatal_err[0] = '\0';
    s_notice_msg[0] = '\0';
    s_notice_timer = 0;
    s_has_queued_seek = 0;
    s_queued_seek_ms = 0;
    s_seek_debounce_frames = 0;
    vt_log(VT_LOG_INFO, "[VideoPlayer] video_player_init completado (standby)");
    log_mem_stats("init");
    return 0;
}

void video_player_shutdown(void)
{
    video_player_close();

    if (s_mpv_context) {
        mpv_render_context_free(s_mpv_context);
        s_mpv_context = NULL;
    }
    if (s_mpv) {
        mpv_terminate_destroy(s_mpv);
        s_mpv = NULL;
    }
    if (s_render_texture) {
        vita2d_free_texture(s_render_texture);
        s_render_texture = NULL;
    }
    if (s_io_mutex >= 0) {
        sceKernelDeleteMutex(s_io_mutex);
        s_io_mutex = -1;
    }

    sceSysmoduleUnloadModule(SCE_SYSMODULE_AUDIOCODEC);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_AVPLAYER);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_AVCDEC);
    vt_log(VT_LOG_INFO, "[VideoPlayer] Shutdown completado");
}

int video_player_open(const char *stream_url)
{
    if (!stream_url || !stream_url[0]) {
        set_player_error("URL de stream vacia o invalida");
        return -1;
    }

    video_player_close();

    if (ensure_mpv_initialized() < 0) {
        set_player_error("No se pudo inicializar el motor de video");
        return -1;
    }

    lock_io();
    if (s_reader_fd >= 0) {
        sceIoClose(s_reader_fd);
        s_reader_fd = -1;
    }
    s_read_pos = 0;
    unlock_io();

    s_download_stop = 0;
    s_download_completed = 0;
    s_download_result = 0;
    s_bytes_downloaded = 0;
    s_status = VPLAYER_BUFFERING;
    s_stage = PLAYER_STAGE_DOWNLOADING;
    s_current_time_ms = 0;
    s_duration_ms = 0;
    s_has_rendered_frame = 0;
    s_first_frame_logged = 0;
    s_file_loaded = 0;
    s_seek_grace_frames = 0;
    s_has_queued_seek = 0;
    s_queued_seek_ms = 0;
    s_seek_debounce_frames = 0;
    s_error_msg[0] = '\0';
    s_mpv_fatal_err[0] = '\0';
    s_notice_msg[0] = '\0';
    s_notice_timer = 0;

    strncpy(s_current_url, stream_url, sizeof(s_current_url) - 1);
    s_current_url[sizeof(s_current_url) - 1] = '\0';

    vt_log(VT_LOG_INFO, "[VideoPlayer] Abriendo stream URL (buffer progresivo)...");

    s_download_thread_id = sceKernelCreateThread("VitaTubeDownloader",
                                                 downloader_thread,
                                                 0x10000100,
                                                 64 * 1024,
                                                 0, 0, NULL);
    if (s_download_thread_id >= 0) {
        sceKernelStartThread(s_download_thread_id, 0, NULL);
    } else {
        vt_log(VT_LOG_ERROR, "[VideoPlayer] Error al crear hilo de descarga");
        set_player_error("No se pudo iniciar el hilo de descarga");
        return -1;
    }

    return 0;
}

void video_player_close(void)
{
    if (s_download_thread_id >= 0) {
        s_download_stop = 1;
        sceKernelWaitThreadEnd(s_download_thread_id, NULL, NULL);
        sceKernelDeleteThread(s_download_thread_id);
        s_download_thread_id = -1;
    }

    if (s_mpv) {
        const char *cmd[] = {"stop", NULL};
        mpv_command(s_mpv, cmd);
    }

    lock_io();
    if (s_reader_fd >= 0) {
        sceIoClose(s_reader_fd);
        s_reader_fd = -1;
    }
    s_read_pos = 0;
    unlock_io();

    if (s_ui_audio_suspended) {
        s_ui_audio_suspended = 0;
        audio_init();
    }

    s_status = VPLAYER_IDLE;
    s_stage = PLAYER_STAGE_IDLE;
    s_has_rendered_frame = 0;
    s_first_frame_logged = 0;
    s_file_loaded = 0;
    s_seek_grace_frames = 0;
    s_has_queued_seek = 0;
    s_queued_seek_ms = 0;
    s_seek_debounce_frames = 0;
    s_current_time_ms = 0;
    s_duration_ms = 0;
    s_pending_seek_ms = 0;
    s_bytes_downloaded = 0;
    s_download_completed = 0;
    s_redraw_needed = 0;
    s_error_msg[0] = '\0';
    s_mpv_fatal_err[0] = '\0';
    s_notice_msg[0] = '\0';
    s_notice_timer = 0;
}

void video_player_play(void)
{
    if (s_mpv && (s_stage == PLAYER_STAGE_PLAYING || s_stage == PLAYER_STAGE_PAUSED)) {
        int pause_flag = 0;
        mpv_set_property_async(s_mpv, 0, "pause", MPV_FORMAT_FLAG, &pause_flag);
        s_status = VPLAYER_PLAYING;
        s_stage = PLAYER_STAGE_PLAYING;
    }
}

void video_player_pause(void)
{
    if (s_mpv && (s_stage == PLAYER_STAGE_PLAYING || s_stage == PLAYER_STAGE_PAUSED)) {
        int pause_flag = 1;
        mpv_set_property_async(s_mpv, 0, "pause", MPV_FORMAT_FLAG, &pause_flag);
        s_status = VPLAYER_PAUSED;
        s_stage = PLAYER_STAGE_PAUSED;
    }
}

void video_player_toggle_pause(void)
{
    if (s_status == VPLAYER_PLAYING) {
        video_player_pause();
    } else if (s_status == VPLAYER_PAUSED) {
        video_player_play();
    }
}

static void perform_safe_seek_to_sec(double target_sec, const char *mode_label, unsigned int debounce_frames)
{
    if (!s_mpv || !s_file_loaded ||
        (s_stage != PLAYER_STAGE_PLAYING && s_stage != PLAYER_STAGE_PAUSED)) {
        vt_log(VT_LOG_INFO, "[VideoPlayer] Seek %s ignorado (aun cargando)", mode_label);
        set_player_notice("Espera a que inicie el video...", 90);
        return;
    }

    double real_cur_sec = (double)s_current_time_ms / 1000.0;
    double prop_pos = -1.0;
    if (mpv_get_property(s_mpv, "time-pos", MPV_FORMAT_DOUBLE, &prop_pos) >= 0 && prop_pos >= 0.0) {
        real_cur_sec = prop_pos;
    }

    if (target_sec < 0.0) {
        target_sec = 0.0;
    }

    if (!s_download_completed && target_sec > real_cur_sec) {
        double cache_end_sec = -1.0;
        if (mpv_get_property(s_mpv, "demuxer-cache-time", MPV_FORMAT_DOUBLE, &cache_end_sec) < 0 ||
            cache_end_sec < 0.0) {
            double cache_dur = 0.0;
            if (mpv_get_property(s_mpv, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &cache_dur) >= 0 &&
                cache_dur > 0.0) {
                cache_end_sec = real_cur_sec + cache_dur;
            }
        }

        double max_safe_sec = (cache_end_sec > 1.5) ? (cache_end_sec - 1.5) : real_cur_sec;
        if (max_safe_sec < real_cur_sec) {
            max_safe_sec = real_cur_sec;
        }
        if (target_sec > max_safe_sec) {
            if (max_safe_sec > real_cur_sec + 1.0) {
                target_sec = max_safe_sec;
                set_player_notice("Adelantando hasta el buffer disponible", 90);
            } else {
                vt_log(VT_LOG_INFO, "[VideoPlayer] Seek %s bloqueado: en el limite del buffer (%.1fs)",
                       mode_label, cache_end_sec);
                set_player_notice("Aun descargando esa parte del video...", 90);
                return;
            }
        }
    } else if (s_download_completed) {
        double dur_sec = (double)s_duration_ms / 1000.0;
        if (dur_sec > 1.0 && target_sec > dur_sec - 1.0) {
            target_sec = dur_sec - 1.0;
        }
    }

    int64_t target_ms = (int64_t)(target_sec * 1000.0);
    if (target_ms < 0) target_ms = 0;

    s_queued_seek_ms = (uint64_t)target_ms;
    s_has_queued_seek = 1;
    s_seek_debounce_frames = debounce_frames;
    s_current_time_ms = (uint64_t)target_ms;
    s_seek_grace_frames = 45;
}

void video_player_seek(int64_t delta_seconds)
{
    if (!s_mpv || !s_file_loaded ||
        (s_stage != PLAYER_STAGE_PLAYING && s_stage != PLAYER_STAGE_PAUSED)) {
        vt_log(VT_LOG_INFO, "[VideoPlayer] Seek relativo ignorado (aun cargando)");
        set_player_notice("Espera a que inicie el video...", 90);
        return;
    }

    double base_sec = s_has_queued_seek
                          ? ((double)s_queued_seek_ms / 1000.0)
                          : ((double)s_current_time_ms / 1000.0);
    if (s_seek_grace_frames == 0 && !s_has_queued_seek) {
        double prop_pos = 0.0;
        if (mpv_get_property(s_mpv, "time-pos", MPV_FORMAT_DOUBLE, &prop_pos) >= 0 && prop_pos >= 0.0) {
            base_sec = prop_pos;
        }
    }

    perform_safe_seek_to_sec(base_sec + (double)delta_seconds, "relativo", 10);
}

void video_player_seek_to(uint64_t target_ms)
{
    perform_safe_seek_to_sec((double)target_ms / 1000.0, "directo", 2);
}

void video_player_set_volume(unsigned int volume)
{
    if (volume > 100) volume = 100;
    s_current_volume = volume;
    if (s_mpv) {
        char vol_str[32];
        snprintf(vol_str, sizeof(vol_str), "%u", volume);
        mpv_set_property_string(s_mpv, "volume", vol_str);
    }
}

VideoPlayerStatus video_player_get_status(void)
{
    return s_status;
}

const char *video_player_get_error(void)
{
    return s_error_msg;
}

const char *video_player_get_notice(void)
{
    return (s_notice_timer > 0) ? s_notice_msg : "";
}

uint64_t video_player_get_current_time(void)
{
    return s_current_time_ms;
}

uint64_t video_player_get_duration(void)
{
    return s_duration_ms;
}

uint32_t video_player_get_buffered_bytes(void)
{
    return s_bytes_downloaded;
}

int video_player_has_frame(void)
{
    return (s_has_rendered_frame && (s_status == VPLAYER_PLAYING || s_status == VPLAYER_PAUSED));
}

void video_player_update(void)
{
    if (s_notice_timer > 0) {
        s_notice_timer--;
        if (s_notice_timer == 0) {
            s_notice_msg[0] = '\0';
        }
    }
    if (s_seek_grace_frames > 0) {
        s_seek_grace_frames--;
    }
    if (s_seek_debounce_frames > 0) {
        s_seek_debounce_frames--;
    }

    /* Prevenir que la pantalla se apague, se atenue o entre en suspension durante la reproduccion o buffering */
    if (s_status == VPLAYER_PLAYING || s_stage == PLAYER_STAGE_PLAYING || s_stage == PLAYER_STAGE_DOWNLOADING || s_status == VPLAYER_BUFFERING) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
    }

    /* 1. Comprobar si alcanzamos el buffer inicial o fin de descarga para iniciar reproducción */
    if (s_stage == PLAYER_STAGE_DOWNLOADING &&
        (s_bytes_downloaded >= INITIAL_BUFFER_BYTES || s_download_completed)) {

        if (s_download_completed && (s_download_result <= 0 || s_bytes_downloaded < 10000)) {
            vt_log(VT_LOG_ERROR, "[VideoPlayer] Fallo en descarga: ret=%d bytes=%u",
                   s_download_result, s_bytes_downloaded);
            set_player_error("No se pudo descargar el stream del servidor");
            return;
        }

        vt_log(VT_LOG_INFO, "[VideoPlayer] Buffer inicial alcanzado (%u bytes descargados). Arrancando stream vts://...",
               s_bytes_downloaded);
        log_mem_stats("before_loadfile");

        if (!s_ui_audio_suspended) {
            audio_shutdown();
            s_ui_audio_suspended = 1;
        }

        char vol_str[32];
        snprintf(vol_str, sizeof(vol_str), "%u", s_current_volume);
        mpv_set_property_string(s_mpv, "volume", vol_str);

        const char *cmd[] = {"loadfile", "vts://stream", "replace", NULL};
        int err = mpv_command(s_mpv, cmd);
        if (err < 0) {
            vt_log(VT_LOG_ERROR, "[VideoPlayer] Error loadfile vts: %s", mpv_error_string(err));
            set_player_error("No se pudo abrir el flujo de video");
            return;
        }

        s_stage = PLAYER_STAGE_PLAYING;
        s_status = VPLAYER_PLAYING;
    }

    if (!s_mpv || !s_mpv_context) return;

    /* 2. Procesar eventos asíncronos de MPV */
    while (1) {
        mpv_event *event = mpv_wait_event(s_mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE) break;

        switch (event->event_id) {
            case MPV_EVENT_LOG_MESSAGE: {
                mpv_event_log_message *msg = (mpv_event_log_message *)event->data;
                if (msg && msg->text && msg->text[0]) {
                    char clean[256];
                    strncpy(clean, msg->text, sizeof(clean) - 1);
                    clean[sizeof(clean) - 1] = '\0';
                    clean[strcspn(clean, "\r\n")] = '\0';
                    if (clean[0]) {
                        vt_log(VT_LOG_WARN, "[MPV:%s] %s",
                               msg->prefix ? msg->prefix : "core", clean);
                        if (strstr(clean, "Not enough space")) {
                            snprintf(s_mpv_fatal_err, sizeof(s_mpv_fatal_err),
                                     "Sin memoria suficiente para continuar el video");
                            log_mem_stats("oom_err");
                        } else if (strstr(clean, "treating it as fatal error")) {
                            if (!s_mpv_fatal_err[0]) {
                                snprintf(s_mpv_fatal_err, sizeof(s_mpv_fatal_err),
                                         "Error al leer paquetes del stream");
                            }
                            log_mem_stats("fatal_demux");
                        } else if (strstr(clean, "Failed to initialize a decoder") ||
                                   strstr(clean, "Error on resampling")) {
                            snprintf(s_mpv_fatal_err, sizeof(s_mpv_fatal_err),
                                     "Error al decodificar el formato del video");
                            log_mem_stats("decode_err");
                        }
                    }
                }
                break;
            }
            case MPV_EVENT_FILE_LOADED:
                vt_log(VT_LOG_INFO, "[VideoPlayer] MPV_EVENT_FILE_LOADED exitoso");
                log_mem_stats("file_loaded");
                s_file_loaded = 1;
                s_status = VPLAYER_PLAYING;
                s_pending_seek_ms = 0;
                break;
            case MPV_EVENT_END_FILE: {
                mpv_event_end_file *end_event = (mpv_event_end_file *)event->data;
                vt_log(VT_LOG_INFO, "[VideoPlayer] MPV_EVENT_END_FILE (reason=%d, error=%d)",
                       end_event ? end_event->reason : -1,
                       end_event ? end_event->error : 0);
                if (end_event && end_event->reason == MPV_END_FILE_REASON_EOF) {
                    if (s_mpv_fatal_err[0]) {
                        set_player_error(s_mpv_fatal_err);
                    } else if (!s_file_loaded || !s_has_rendered_frame) {
                        set_player_error("El stream no contiene video valido o se corto");
                    } else {
                        s_status = VPLAYER_COMPLETED;
                        s_stage = PLAYER_STAGE_FINISHED;
                    }
                } else if (end_event && end_event->reason == MPV_END_FILE_REASON_ERROR) {
                    if (s_mpv_fatal_err[0]) {
                        set_player_error(s_mpv_fatal_err);
                    } else {
                        char msg_buf[160];
                        snprintf(msg_buf, sizeof(msg_buf), "Error al reproducir el video (%s)",
                                 mpv_error_string(end_event->error));
                        set_player_error(msg_buf);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    /* 3. Actualizar tiempo, despachar seek coalescido y verificar estado */
    if (s_stage == PLAYER_STAGE_PLAYING || s_stage == PLAYER_STAGE_PAUSED) {
        int seeking = 0;
        if (mpv_get_property(s_mpv, "seeking", MPV_FORMAT_FLAG, &seeking) < 0) {
            seeking = 0;
        }

        if (s_has_queued_seek && s_seek_debounce_frames == 0 && !seeking && s_file_loaded) {
            uint64_t target_ms = s_queued_seek_ms;
            s_has_queued_seek = 0;

            char pos_str[32];
            snprintf(pos_str, sizeof(pos_str), "%llu.%03u",
                     (unsigned long long)(target_ms / 1000), (unsigned int)(target_ms % 1000));
            const char *cmd[] = {"seek", pos_str, "absolute+keyframes", NULL};
            int r = mpv_command_async(s_mpv, 0, cmd);
            if (r < 0) {
                vt_log(VT_LOG_WARN, "[VideoPlayer] Error al enviar seek (%s): %s",
                       pos_str, mpv_error_string(r));
            } else {
                s_seek_grace_frames = 35;
                vt_log(VT_LOG_INFO, "[VideoPlayer] Seek -> %s s", pos_str);
                log_mem_stats("seek");
            }
        }

        int eof_reached = 0;
        if (mpv_get_property(s_mpv, "eof-reached", MPV_FORMAT_FLAG, &eof_reached) >= 0 && eof_reached) {
            if (s_mpv_fatal_err[0]) {
                set_player_error(s_mpv_fatal_err);
                return;
            } else if (!s_has_rendered_frame || (s_download_completed && s_download_result <= 0)) {
                set_player_error("El stream no contiene video valido o se corto");
                return;
            }
        }

        if (!seeking && s_seek_grace_frames == 0 && !s_has_queued_seek) {
            double time_pos = 0.0;
            if (mpv_get_property(s_mpv, "time-pos", MPV_FORMAT_DOUBLE, &time_pos) >= 0 && time_pos >= 0.0) {
                s_current_time_ms = (uint64_t)(time_pos * 1000.0);
            }
        }

        double duration = 0.0;
        if (mpv_get_property(s_mpv, "duration", MPV_FORMAT_DOUBLE, &duration) >= 0 && duration > 0.0) {
            s_duration_ms = (uint64_t)(duration * 1000.0);
        }

        int paused = 0;
        if (mpv_get_property(s_mpv, "pause", MPV_FORMAT_FLAG, &paused) >= 0) {
            if (paused && s_status == VPLAYER_PLAYING) {
                s_status = VPLAYER_PAUSED;
                s_stage = PLAYER_STAGE_PAUSED;
            } else if (!paused && s_status == VPLAYER_PAUSED) {
                s_status = VPLAYER_PLAYING;
                s_stage = PLAYER_STAGE_PLAYING;
            }
        }
    }

    /* 4. Renderizar cuadro al FBO si hay nueva imagen lista */
    if (s_redraw_needed || (mpv_render_context_update(s_mpv_context) & MPV_RENDER_UPDATE_FRAME)) {
        s_redraw_needed = 0;
        mpv_render_context_render(s_mpv_context, s_render_params);
        mpv_render_context_report_swap(s_mpv_context);
        s_has_rendered_frame = 1;
        if (!s_first_frame_logged) {
            s_first_frame_logged = 1;
            vt_log(VT_LOG_INFO, "[VideoPlayer] Primer cuadro GXM renderizado correctamente");
        }
    }
}

void video_player_draw(int x, int y, int width, int height)
{
    vita2d_draw_rectangle(x, y, width, height, RGBA8(0, 0, 0, 255));

    if (s_has_rendered_frame && s_render_texture) {
        float scale_x = (float)width / 960.0f;
        float scale_y = (float)height / 544.0f;
        vita2d_draw_texture_scale(s_render_texture, (float)x, (float)y, scale_x, scale_y);
    }
}
