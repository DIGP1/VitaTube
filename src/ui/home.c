#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include "log.h"
#include "home.h"
#include "youtube.h"
#include "history.h"
#include "audio.h"
#include "video_player.h"

#define CARD_COLUMNS 3
#define CARD_ROWS 2
#define SIDEBAR_ITEMS 6
#define ONLINE_VIDEO_CAPACITY 24
#define THUMBNAIL_BUFFER_SIZE 131072

static vita2d_pgf *font;
static unsigned int selected_row;
static unsigned int selected_column;
static unsigned int content_offset;
static unsigned int selected_sidebar;
static unsigned int active_view;
static unsigned int selected_setting = 0;
static unsigned int player_active;
static volatile int player_state;
static int player_thread = -1;
static char player_response[HTTP_RESPONSE_MAX];
static char player_url[4096];
static char player_video_id[32];
static char player_video_title[128];
static char player_video_author[64];
static char player_video_published[48];
static unsigned int player_video_duration;
static unsigned int player_quality;
static unsigned int player_volume = 80;
static unsigned int player_fullscreen = 1;
static unsigned int player_overlay_timer = 0;
static unsigned int player_side_tab = 0;

/* Secciones y controles del reproductor estilo YouTube TV */
enum PlayerFocusSection {
    PLAYER_FOCUS_SEEKBAR = 0,
    PLAYER_FOCUS_CONTROLS = 1,
    PLAYER_FOCUS_CAROUSEL = 2
};

enum PlayerCtrlButton {
    PLAYER_BTN_AVATAR = 0,
    PLAYER_BTN_DESC = 1,
    PLAYER_BTN_SUBS = 2,
    PLAYER_BTN_PREV = 3,
    PLAYER_BTN_PLAY = 4,   /* Boton central enfocado por defecto */
    PLAYER_BTN_NEXT = 5,
    PLAYER_BTN_LIKE = 6,
    PLAYER_BTN_DISLIKE = 7,
    PLAYER_BTN_COMMENTS = 8,
    PLAYER_BTN_SAVE = 9,
    PLAYER_BTN_THANKS = 10,
    PLAYER_BTN_SETTINGS = 11,
    PLAYER_BTN_COUNT = 12
};

static int player_focus_section = PLAYER_FOCUS_CONTROLS;
static int player_focus_btn = PLAYER_BTN_PLAY;
static int player_carousel_idx = 0;
static float player_carousel_scroll = 0.0f;
static int player_show_desc = 0;
static int player_show_comments = 0;
static int player_show_settings = 0;
static int player_settings_selected = 0;
static uint64_t player_resume_ms = 0;
static int player_is_subscribed = 0;
static int player_like_state = 0;
static unsigned long player_video_views = 0;
static int player_video_is_live = 0;
static char player_error_msg[160] = {0};
static YoutubeVideo player_related_videos[12];
static unsigned int player_related_count = 0;
static unsigned int search_results_active;
static unsigned int sidebar_active;
static const char *status_message;
static int previous_touch_x = -1;
static int previous_touch_y = -1;
static int keyboard_active;
static unsigned int keyboard_row;
static unsigned int keyboard_column;
static unsigned int keyboard_shift;
static unsigned int keyboard_touch_latched;
static unsigned int keyboard_target;
static char keyboard_text[128];
static char search_label[128];
static char account_username[64];
static char account_password[64];
static unsigned int account_view;
static unsigned int account_field;
static char account_confirm_password[64];
static const char *account_status;
static char search_response[HTTP_RESPONSE_MAX];
static unsigned char search_thumbnail_buffer[THUMBNAIL_BUFFER_SIZE];
static YoutubeVideo online_videos[ONLINE_VIDEO_CAPACITY];
static vita2d_texture *online_thumbnails[ONLINE_VIDEO_CAPACITY];
static unsigned int online_video_count;
static unsigned char startup_thumbnail_buffer[THUMBNAIL_BUFFER_SIZE];
static volatile unsigned int startup_thumbnail_index;
static volatile int startup_worker_state;
static volatile int startup_thumbnail_bytes;
static int startup_thread = -1;
static unsigned int startup_frame;
static YoutubeVideo search_videos[ONLINE_VIDEO_CAPACITY];
static vita2d_texture *search_thumbnails[ONLINE_VIDEO_CAPACITY];
static unsigned int search_video_count;
static unsigned int search_thumbnail_index;
static volatile int search_worker_state;
static volatile int search_thumbnail_bytes;
static int search_thread = -1;
static char search_worker_query[128];
static unsigned int loading_frame;
static int previous_back_touch_y = -1;
static unsigned int anim_tick = 0;
static YoutubeVideo subs_videos[ONLINE_VIDEO_CAPACITY];
static vita2d_texture *subs_thumbnails[ONLINE_VIDEO_CAPACITY];
static unsigned int subs_video_count = 0;
static volatile unsigned int subs_thumbnail_index = 0;
static volatile int subs_thumbnail_bytes = 0;
static unsigned char subs_thumbnail_buffer[THUMBNAIL_BUFFER_SIZE];
static volatile int subs_worker_state = 0;
static int subs_thread = -1;

/* Miniaturas de la Biblioteca (Historial) */
static vita2d_texture *history_thumbnails[ONLINE_VIDEO_CAPACITY];
static char history_thumb_ids[ONLINE_VIDEO_CAPACITY][32];
static volatile unsigned int history_thumbnail_index = 0;
static volatile int history_thumbnail_bytes = 0;
static unsigned char history_thumbnail_buffer[THUMBNAIL_BUFFER_SIZE];
static volatile int history_worker_state = 0;
static int history_thread = -1;

#define HOME_SHORTS_SHELF_COLS 5
#define HOME_SHORTS_CAPACITY 12
static YoutubeVideo home_shorts_videos[HOME_SHORTS_CAPACITY];
static vita2d_texture *home_shorts_thumbnails[ONLINE_VIDEO_CAPACITY];
static unsigned int home_shorts_count = 0;
static volatile unsigned int home_shorts_thumb_idx = 0;
static int home_shorts_thumbs_done = 0;

/* Estado de la vista vertical de YouTube Shorts (active_view == 1) */
enum ShortsFocusButton {
    SHORTS_BTN_PLAY = 0,
    SHORTS_BTN_SUBSCRIBE = 1,
    SHORTS_BTN_LIKE = 2,
    SHORTS_BTN_DISLIKE = 3,
    SHORTS_BTN_COMMENTS = 4,
    SHORTS_BTN_SHARE = 5,
    SHORTS_BTN_COUNT = 6
};

static int loaded_feed_view = -1;
static unsigned int shorts_index = 0;
static int shorts_playing_index = -1;
static int shorts_focus_btn = SHORTS_BTN_LIKE;
static int shorts_show_comments = 0;
static unsigned int shorts_comment_scroll = 0;
static int shorts_muted = 0;
static int shorts_phone_mode = 0;      /* 0=normal horizontal, 1=modo telefono rotado 90 grados */
static int shorts_phone_clockwise = 0; /* 0=giro antihorario (-90), 1=giro horario (+90) */
static vita2d_texture *s_phone_ui_tex = NULL;
static int s_phone_ui_dirty = 1;
static int shorts_touch_start_x = -1;
static int shorts_touch_start_y = -1;
static int shorts_swipe_handled = 0;

static YoutubeComment shorts_comments[12];
static unsigned int shorts_comment_count = 0;
static volatile int shorts_meta_state = 0; /* 0=idle, 1=cargando, 2=listo */
static int shorts_meta_thread = -1;
static char shorts_meta_video_id[32] = {0};
static char shorts_meta_channel[64] = {0};
static char shorts_meta_avatar_url[512] = {0};
static unsigned char shorts_meta_avatar_buf[65536];
static volatile int shorts_meta_avatar_bytes = 0;
static vita2d_texture *player_author_avatar_tex = NULL;

/* Estado del flujo OAuth 2.0 Device Flow en segundo plano */
static AuthDeviceCode oauth_device_code;
static volatile int oauth_worker_state = 0; /* 0=idle, 1=solicitando codigo, 2=polling token, 3=exito, -1=error */
static volatile int oauth_cancel_flag = 0;
static int oauth_thread = -1;

enum {
    SEARCH_SECTION_KEYBOARD = 0,
    SEARCH_SECTION_HISTORY = 1,
    SEARCH_SECTION_CAROUSEL = 2
};

#define SEARCH_HISTORY_MAX 5
static char recent_searches[SEARCH_HISTORY_MAX][64];
static unsigned int recent_search_count = 0;
static unsigned int search_section = SEARCH_SECTION_KEYBOARD;
static unsigned int search_history_selected = 0;
static unsigned int search_carousel_selected = 0;
static unsigned int search_symbols_mode = 0;

static const char *search_keys_abc[4][8] = {
    { "A", "B", "C", "D", "E", "F", "G", "DEL" },
    { "H", "I", "J", "K", "L", "M", "N", "&123" },
    { "\xC3\x91", "O", "P", "Q", "R", "S", "T", "." },
    { "U", "V", "W", "X", "Y", "Z", "-", "/" }
};

static const char *search_keys_123[4][8] = {
    { "1", "2", "3", "4", "5", "6", "7", "DEL" },
    { "8", "9", "0", "!", "?", ":", ",", "ABC" },
    { "(", ")", "+", "=", "\"", "'", "@", "." },
    { "[", "]", "{", "}", "_", "*", "-", "/" }
};

static volatile int suggestions_worker_state = 0;
static int suggestions_thread = -1;
static char suggestions_query[128] = {0};
static char suggestions_last_fetched[128] = {0};
static char live_suggestions[5][64];
static unsigned int live_suggestions_count = 0;

static int suggestions_worker(SceSize args, void *arg)
{
    (void)args;
    (void)arg;
    while (suggestions_worker_state != -2) {
        if (suggestions_worker_state == 1) {
            char query_copy[128];
            strncpy(query_copy, suggestions_query, sizeof(query_copy) - 1);
            query_copy[sizeof(query_copy) - 1] = 0;

            if (query_copy[0] != 0) {
                char temp[5][64];
                int count = youtube_suggestions(query_copy, temp, 5);
                if (count > 0 && suggestions_worker_state == 1) {
                    for (int i = 0; i < count; i++) {
                        strncpy(live_suggestions[i], temp[i], sizeof(live_suggestions[i]) - 1);
                        live_suggestions[i][sizeof(live_suggestions[i]) - 1] = 0;
                    }
                    live_suggestions_count = (unsigned int)count;
                } else if (count <= 0 && suggestions_worker_state == 1) {
                    live_suggestions_count = 0;
                }
            } else {
                live_suggestions_count = 0;
            }
            strcpy(suggestions_last_fetched, query_copy);
            suggestions_worker_state = 0;
        }
        sceKernelDelayThread(40000);
    }
    return 0;
}

static unsigned int thumb_nav_cooldown = 0;
static unsigned int thumb_decoded_this_frame = 0;

static void start_startup_request(void);
static void start_subs_request(void);
static void start_history_thumbnails(void);
static void start_short_playback(unsigned int idx);
static void switch_to_sidebar_view(unsigned int new_view);

static int can_decode_thumbnail_this_frame(void)
{
    if (player_active) return 0;
    if (active_view == 1 && !search_results_active) return 0;
    if (thumb_nav_cooldown > 0) return 0;
    if (thumb_decoded_this_frame > 0) return 0;
    if ((anim_tick % 3) != 0) return 0;
    return 1;
}

static void log_event(const char *format, ...)
{
    char buf[384];
    va_list args;

    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    vt_log(VT_LOG_INFO, "[Event] %s", buf);
}

static const char *keyboard_rows[] = {
    "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"
};

static const char *row_titles[] = {
    "Recomendados",
    "Tendencias",
    "Gaming y tecnologia"
};

static const char *sidebar_items[] = {
    "Inicio",
    "Shorts",
    "Tendencias",
    "Suscripciones",
    "Biblioteca",
    "Configuracion"
};

static const char *video_titles[CARD_ROWS][CARD_COLUMNS] = {
    {
        "Bienvenido a VitaTube",
        "Novedades de PS Vita",
        "Como crear homebrew"
    },
    {
        "Los videos mas vistos",
        "Noticias de videojuegos",
        "El futuro de las consolas"
    }
};

static char search_result_titles[CARD_ROWS][CARD_COLUMNS][96] = {
    {
        "Resultado relacionado 1", "Resultado relacionado 2",
        "Resultado relacionado 3"
    },
    {
        "Videos recomendados",
        "Lo mas visto de la busqueda",
        "Nuevos resultados"
    }
};

static void clear_thumbnails(vita2d_texture **textures)
{
    unsigned int i;
    int any_freed = 0;

    for (i = 0; i < ONLINE_VIDEO_CAPACITY; ++i) {
        if (textures[i]) {
            any_freed = 1;
            break;
        }
    }
    if (any_freed) {
        vita2d_wait_rendering_done();
        for (i = 0; i < ONLINE_VIDEO_CAPACITY; ++i) {
            if (textures[i]) {
                vita2d_free_texture(textures[i]);
                textures[i] = NULL;
            }
        }
        vita2d_wait_rendering_done();
    }
}

static vita2d_texture *safe_load_jpeg_thumbnail(const unsigned char *buf, int bytes)
{
    if (!buf || bytes < 64) return NULL;
    vita2d_texture *tex = NULL;
    if (buf[0] == 0xFF && buf[1] == 0xD8) {
        vita2d_wait_rendering_done();
        tex = vita2d_load_JPEG_buffer(buf, (unsigned long)bytes);
        vita2d_wait_rendering_done();
        thumb_decoded_this_frame = 1;
    } else if (buf[0] == 0x89 && buf[1] == 0x50 && buf[2] == 0x4E && buf[3] == 0x47) {
        vita2d_wait_rendering_done();
        tex = vita2d_load_PNG_buffer(buf);
        vita2d_wait_rendering_done();
        thumb_decoded_this_frame = 1;
    }
    return tex;
}

static int oauth_worker(SceSize args, void *arg)
{
    (void)args;
    (void)arg;

    oauth_worker_state = 1;
    if (youtube_oauth_start(&oauth_device_code) != 0) {
        oauth_worker_state = -1;
        return 0;
    }

    oauth_worker_state = 2;
    int poll_res = youtube_oauth_poll(&oauth_device_code, &oauth_cancel_flag);
    if (poll_res == 0) {
        oauth_worker_state = 3;
    } else if (oauth_cancel_flag) {
        oauth_worker_state = 0;
    } else {
        oauth_worker_state = -1;
    }
    return 0;
}

static void stop_oauth_worker(void)
{
    if (oauth_thread >= 0) {
        oauth_cancel_flag = 1;
        sceKernelWaitThreadEnd(oauth_thread, NULL, NULL);
        sceKernelDeleteThread(oauth_thread);
        oauth_thread = -1;
    }
    oauth_cancel_flag = 0;
}

static void start_oauth_flow(void)
{
    stop_oauth_worker();
    memset(&oauth_device_code, 0, sizeof(oauth_device_code));
    oauth_worker_state = 1;
    account_status = "Solicitando codigo de vinculacion a Google...";

    oauth_thread = sceKernelCreateThread("VitaTubeOAuth", oauth_worker,
                                         0x10000100, 128 * 1024, 0, 0, NULL);
    if (oauth_thread < 0 || sceKernelStartThread(oauth_thread, 0, NULL) < 0) {
        oauth_worker_state = -1;
        account_status = "Error al iniciar el hilo de autenticacion";
        if (oauth_thread >= 0) {
            sceKernelDeleteThread(oauth_thread);
            oauth_thread = -1;
        }
    }
}

static int search_worker(SceSize args, void *arg)
{
    int result;

    (void)args;
    (void)arg;
    result = youtube_search_videos(search_worker_query, search_videos,
                                   ONLINE_VIDEO_CAPACITY);
    int sh_res = youtube_search_shorts(search_worker_query, home_shorts_videos,
                                       HOME_SHORTS_CAPACITY);
    home_shorts_count = (sh_res > 0) ? (unsigned int)sh_res : 0;
    home_shorts_thumb_idx = 0;
    home_shorts_thumbs_done = (home_shorts_count == 0) ? 1 : 0;

    if (result > 0) {
        search_video_count = (unsigned int)result;
        search_worker_state = 2;
    } else {
        search_video_count = 0;
        search_worker_state = -1;
    }
    while (search_worker_state != -2) {
        if (search_worker_state == 3 && search_thumbnail_index < search_video_count) {
            int bytes = youtube_download_thumbnail(
                &search_videos[search_thumbnail_index],
                search_thumbnail_buffer, sizeof(search_thumbnail_buffer));
            search_thumbnail_bytes = bytes;
            search_worker_state = bytes > 0 ? 4 : 5;
        } else if (search_worker_state == 6 &&
                   home_shorts_thumb_idx < HOME_SHORTS_SHELF_COLS &&
                   home_shorts_thumb_idx < home_shorts_count) {
            int bytes = youtube_download_thumbnail(
                &home_shorts_videos[home_shorts_thumb_idx],
                search_thumbnail_buffer, sizeof(search_thumbnail_buffer));
            search_thumbnail_bytes = bytes;
            search_worker_state = bytes > 0 ? 7 : 8;
        } else {
            sceKernelDelayThread(15000);
        }
    }
    return 0;
}

static int has_home_shorts_shelf(void)
{
    if (content_offset != 0 || home_shorts_count == 0) return 0;
    if (search_results_active) return 1;
    return (active_view == 0 || active_view == 2);
}

static int startup_worker(SceSize args, void *arg)
{
    int result;
    unsigned int target_view = active_view;

    (void)args;
    (void)arg;
    if (target_view == 1) {
        result = youtube_shorts(online_videos, ONLINE_VIDEO_CAPACITY);
    } else if (target_view == 0) {
        result = youtube_home(online_videos, ONLINE_VIDEO_CAPACITY);
        int sh_res = youtube_shorts(home_shorts_videos, HOME_SHORTS_CAPACITY);
        home_shorts_count = (sh_res > 0) ? (unsigned int)sh_res : 0;
        home_shorts_thumb_idx = 0;
        home_shorts_thumbs_done = (home_shorts_count == 0) ? 1 : 0;
    } else {
        result = youtube_trending(online_videos, ONLINE_VIDEO_CAPACITY);
        int sh_res = youtube_shorts(home_shorts_videos, HOME_SHORTS_CAPACITY);
        home_shorts_count = (sh_res > 0) ? (unsigned int)sh_res : 0;
        home_shorts_thumb_idx = 0;
        home_shorts_thumbs_done = (home_shorts_count == 0) ? 1 : 0;
    }
    if (result <= 0) {
        startup_worker_state = -1;
        return 0;
    }
    loaded_feed_view = (int)target_view;
    online_video_count = (unsigned int)result;
    log_event("home_trending view=%u videos=%u shorts=%u region=%s",
              target_view, online_video_count, home_shorts_count, youtube_region());
    startup_thumbnail_index = 0;
    if (target_view == 1) {
        /* En Shorts no descargamos las 24 miniaturas en segundo plano para dejar la red y GPU libres al reproductor */
        startup_worker_state = 0;
        return 0;
    }
    startup_worker_state = 3;
    while (startup_worker_state != -2) {
        if (startup_worker_state == 3 &&
            startup_thumbnail_index < online_video_count) {
            vt_log(VT_LOG_DEBUG, "[Inicio] Descargando miniatura %u",
                   startup_thumbnail_index);
            startup_thumbnail_bytes = youtube_download_thumbnail(
                &online_videos[startup_thumbnail_index],
                startup_thumbnail_buffer, sizeof(startup_thumbnail_buffer));
            vt_log(VT_LOG_DEBUG, "[Inicio] Descarga terminada %u bytes=%d",
                   startup_thumbnail_index, startup_thumbnail_bytes);
            startup_worker_state = startup_thumbnail_bytes > 0 ? 4 : 5;
        } else if (startup_worker_state == 6 &&
                   home_shorts_thumb_idx < HOME_SHORTS_SHELF_COLS &&
                   home_shorts_thumb_idx < home_shorts_count) {
            startup_thumbnail_bytes = youtube_download_thumbnail(
                &home_shorts_videos[home_shorts_thumb_idx],
                startup_thumbnail_buffer, sizeof(startup_thumbnail_buffer));
            startup_worker_state = startup_thumbnail_bytes > 0 ? 7 : 8;
        } else {
            sceKernelDelayThread(15000);
        }
    }
    return 0;
}

static void stop_startup_worker(void)
{
    if (startup_thread >= 0) {
        startup_worker_state = -2;
        sceKernelWaitThreadEnd(startup_thread, NULL, NULL);
        sceKernelDeleteThread(startup_thread);
        startup_thread = -1;
    }
    startup_worker_state = 0;
}

static void start_startup_request(void)
{
    stop_startup_worker();
    startup_worker_state = 1;
    startup_frame = 0;
    startup_thread = sceKernelCreateThread("VitaTubeStartup",
                                           startup_worker, 0x10000100,
                                           256 * 1024, 0, 0, NULL);
    if (startup_thread < 0 ||
        sceKernelStartThread(startup_thread, 0, NULL) < 0) {
        startup_worker_state = -1;
        if (startup_thread >= 0) {
            sceKernelDeleteThread(startup_thread);
            startup_thread = -1;
        }
    }
}

static void start_search_request(void)
{
    if (search_thread >= 0) {
        search_worker_state = -2;
        sceKernelWaitThreadEnd(search_thread, NULL, NULL);
        sceKernelDeleteThread(search_thread);
        search_thread = -1;
    }
    search_worker_state = 1;
    search_thumbnail_index = 0;
    loading_frame = 0;
    strncpy(search_worker_query, search_label, sizeof(search_worker_query) - 1);
    search_worker_query[sizeof(search_worker_query) - 1] = 0;
    search_thread = sceKernelCreateThread("VitaTubeSearch", search_worker,
                                          0x10000100, 256 * 1024, 0, 0, NULL);
    if (search_thread < 0 ||
        sceKernelStartThread(search_thread, 0, NULL) < 0) {
        search_worker_state = -1;
        if (search_thread >= 0) {
            sceKernelDeleteThread(search_thread);
            search_thread = -1;
        }
    }
}

static int subs_worker(SceSize args, void *arg)
{
    int result;
    (void)args;
    (void)arg;

    result = youtube_subscriptions(subs_videos, ONLINE_VIDEO_CAPACITY);
    if (result <= 0) {
        subs_worker_state = -1;
        subs_video_count = 0;
        return 0;
    }
    subs_video_count = (unsigned int)result;
    subs_thumbnail_index = 0;
    subs_worker_state = 2;
    log_event("subs_loaded count=%u", subs_video_count);

    while (subs_worker_state != -2) {
        if (subs_worker_state == 3 && subs_thumbnail_index < subs_video_count) {
            int bytes = youtube_download_thumbnail(
                &subs_videos[subs_thumbnail_index],
                subs_thumbnail_buffer, sizeof(subs_thumbnail_buffer));
            subs_thumbnail_bytes = bytes;
            subs_worker_state = bytes > 0 ? 4 : 5;
        } else {
            sceKernelDelayThread(15000);
        }
    }
    return 0;
}

static void start_subs_request(void)
{
    if (!youtube_has_subscriptions()) {
        subs_worker_state = -1;
        subs_video_count = 0;
        return;
    }
    if (subs_thread >= 0) {
        subs_worker_state = -2;
        sceKernelWaitThreadEnd(subs_thread, NULL, NULL);
        sceKernelDeleteThread(subs_thread);
        subs_thread = -1;
    }
    subs_worker_state = 1;
    subs_thread = sceKernelCreateThread("VitaTubeSubs", subs_worker,
                                        0x10000100, 256 * 1024, 0, 0, NULL);
    if (subs_thread < 0 ||
        sceKernelStartThread(subs_thread, 0, NULL) < 0) {
        subs_worker_state = -1;
        if (subs_thread >= 0) {
            sceKernelDeleteThread(subs_thread);
            subs_thread = -1;
        }
    }
}

static void update_subs_loading(void)
{
    if (subs_worker_state == 2) {
        clear_thumbnails(subs_thumbnails);
        subs_thumbnail_index = 0;
        subs_worker_state = subs_video_count > 0 ? 3 : 0;
    } else if (subs_worker_state == 4) {
        if (!can_decode_thumbnail_this_frame()) {
            return;
        }
        if (subs_thumbnail_index < ONLINE_VIDEO_CAPACITY) {
            if (subs_thumbnails[subs_thumbnail_index]) {
                vita2d_wait_rendering_done();
                vita2d_free_texture(subs_thumbnails[subs_thumbnail_index]);
                subs_thumbnails[subs_thumbnail_index] = NULL;
            }
            subs_thumbnails[subs_thumbnail_index] =
                safe_load_jpeg_thumbnail(subs_thumbnail_buffer, subs_thumbnail_bytes);
        }
        ++subs_thumbnail_index;
        if (subs_thumbnail_index >= subs_video_count) {
            subs_worker_state = 0;
            status_message = "Suscripciones listas";
        } else {
            subs_worker_state = 3;
        }
    } else if (subs_worker_state == 5) {
        ++subs_thumbnail_index;
        if (subs_thumbnail_index >= subs_video_count) {
            subs_worker_state = 0;
            status_message = "Suscripciones listas";
        } else {
            subs_worker_state = 3;
        }
    }
}

static int history_worker(SceSize args, void *arg)
{
    (void)args;
    (void)arg;

    while (history_worker_state != -2) {
        if (history_worker_state == 3) {
            unsigned int hcnt = history_count();
            if (history_thumbnail_index < hcnt && history_thumbnail_index < ONLINE_VIDEO_CAPACITY) {
                const YoutubeVideo *hv = history_get(history_thumbnail_index);
                if (hv && hv->id[0]) {
                    int bytes = youtube_download_thumbnail(
                        hv, history_thumbnail_buffer, sizeof(history_thumbnail_buffer));
                    history_thumbnail_bytes = bytes;
                    history_worker_state = bytes > 0 ? 4 : 5;
                } else {
                    history_worker_state = 5;
                }
            } else {
                history_worker_state = 0;
            }
        } else {
            sceKernelDelayThread(15000);
        }
    }
    return 0;
}

static void start_history_thumbnails(void)
{
    if (history_count() == 0) return;
    if (history_thread < 0) {
        history_worker_state = 0;
        history_thread = sceKernelCreateThread("VitaTubeHistThumb", history_worker,
                                               0x10000100, 128 * 1024, 0, 0, NULL);
        if (history_thread >= 0) {
            sceKernelStartThread(history_thread, 0, NULL);
        }
    }
    if (history_worker_state == 0) {
        history_thumbnail_index = 0;
        history_worker_state = 3;
    }
}

static void update_history_loading(void)
{
    unsigned int hcnt = history_count();
    if (history_worker_state == 3) {
        /* Saltar los que ya tienen miniatura cargada para ese mismo video ID */
        while (history_thumbnail_index < hcnt && history_thumbnail_index < ONLINE_VIDEO_CAPACITY) {
            const YoutubeVideo *hv = history_get(history_thumbnail_index);
            if (hv && history_thumbnails[history_thumbnail_index] &&
                strcmp(history_thumb_ids[history_thumbnail_index], hv->id) == 0) {
                history_thumbnail_index++;
            } else {
                break;
            }
        }
        if (history_thumbnail_index >= hcnt || history_thumbnail_index >= ONLINE_VIDEO_CAPACITY) {
            history_worker_state = 0;
        }
    } else if (history_worker_state == 4) {
        if (!can_decode_thumbnail_this_frame()) {
            return;
        }
        if (history_thumbnail_index < ONLINE_VIDEO_CAPACITY) {
            if (history_thumbnails[history_thumbnail_index]) {
                vita2d_wait_rendering_done();
                vita2d_free_texture(history_thumbnails[history_thumbnail_index]);
                history_thumbnails[history_thumbnail_index] = NULL;
            }
            const YoutubeVideo *hv = history_get(history_thumbnail_index);
            if (hv) {
                snprintf(history_thumb_ids[history_thumbnail_index],
                         sizeof(history_thumb_ids[0]), "%s", hv->id);
            }
            history_thumbnails[history_thumbnail_index] =
                safe_load_jpeg_thumbnail(history_thumbnail_buffer, history_thumbnail_bytes);
        }
        ++history_thumbnail_index;
        if (history_thumbnail_index >= hcnt || history_thumbnail_index >= ONLINE_VIDEO_CAPACITY) {
            history_worker_state = 0;
        } else {
            history_worker_state = 3;
        }
    } else if (history_worker_state == 5) {
        ++history_thumbnail_index;
        if (history_thumbnail_index >= hcnt || history_thumbnail_index >= ONLINE_VIDEO_CAPACITY) {
            history_worker_state = 0;
        } else {
            history_worker_state = 3;
        }
    }
}

static int shorts_meta_worker(SceSize args, void *arg)
{
    (void)args;
    (void)arg;
    char ch_buf[64] = {0};
    char av_url[512] = {0};
    YoutubeComment temp_comments[12];
    memset(temp_comments, 0, sizeof(temp_comments));
    shorts_meta_avatar_bytes = 0;

    int cnt = youtube_get_comments(shorts_meta_video_id,
                                   ch_buf, sizeof(ch_buf),
                                   av_url, sizeof(av_url),
                                   temp_comments, 12);
    if (ch_buf[0] != '\0') {
        snprintf(shorts_meta_channel, sizeof(shorts_meta_channel), "%s", ch_buf);
    }
    if (av_url[0] != '\0') {
        snprintf(shorts_meta_avatar_url, sizeof(shorts_meta_avatar_url), "%s", av_url);
        int abytes = http_get_binary(av_url, shorts_meta_avatar_buf, sizeof(shorts_meta_avatar_buf));
        if (abytes > 64) {
            shorts_meta_avatar_bytes = abytes;
        }
    }
    if (cnt > 0) {
        for (int i = 0; i < cnt && i < 12; i++) {
            shorts_comments[i] = temp_comments[i];
        }
        shorts_comment_count = (unsigned int)cnt;
    }
    shorts_meta_state = 2;
    return 0;
}

static void start_shorts_meta_request(const char *video_id)
{
    if (!video_id || !video_id[0]) return;
    if (shorts_meta_thread >= 0) {
        sceKernelWaitThreadEnd(shorts_meta_thread, NULL, NULL);
        sceKernelDeleteThread(shorts_meta_thread);
        shorts_meta_thread = -1;
    }
    snprintf(shorts_meta_video_id, sizeof(shorts_meta_video_id), "%s", video_id);
    shorts_meta_channel[0] = '\0';
    shorts_meta_avatar_url[0] = '\0';
    shorts_meta_avatar_bytes = 0;
    shorts_comment_count = 0;
    shorts_meta_state = 1;
    shorts_meta_thread = sceKernelCreateThread("VitaTubeShortsMeta", shorts_meta_worker,
                                               0x10000100, 256 * 1024, 0, 0, NULL);
    if (shorts_meta_thread >= 0) {
        sceKernelStartThread(shorts_meta_thread, 0, NULL);
    } else {
        shorts_meta_state = 0;
    }
}

static int player_worker(SceSize args, void *arg)
{
    (void)args;
    (void)arg;
    int res = youtube_get_stream_url(player_video_id, player_url, sizeof(player_url));
    if (res == 0 && player_url[0] != '\0') {
        player_quality = youtube_max_quality();
        player_state = 2;
    } else {
        player_state = -1;
    }
    return 0;
}

static void free_author_avatar_texture(void)
{
    if (player_author_avatar_tex) {
        vita2d_wait_rendering_done();
        vita2d_free_texture(player_author_avatar_tex);
        player_author_avatar_tex = NULL;
        vita2d_wait_rendering_done();
    }
}

static void start_short_playback(unsigned int idx)
{
    if (idx >= online_video_count) return;
    YoutubeVideo *v = &online_videos[idx];

    shorts_index = idx;
    shorts_playing_index = (int)idx;
    shorts_show_comments = 0;
    shorts_comment_scroll = 0;
    shorts_comment_count = 0;
    shorts_meta_state = 0;

    history_add(v);
    video_player_close();
    free_author_avatar_texture();

    if (player_thread >= 0) {
        player_state = -2;
        sceKernelWaitThreadEnd(player_thread, NULL, NULL);
        sceKernelDeleteThread(player_thread);
        player_thread = -1;
    }

    snprintf(player_video_id, sizeof(player_video_id), "%s", v->id);
    snprintf(player_video_title, sizeof(player_video_title), "%s", v->title);
    snprintf(player_video_author, sizeof(player_video_author), "%s", v->channel);
    player_video_duration = v->duration > 0 ? v->duration : 45;
    player_video_views = v->views > 0 ? v->views : 37200;
    player_video_is_live = 0;

    player_is_subscribed = youtube_is_channel_subscribed(player_video_author);
    player_like_state = youtube_get_video_rating(player_video_id);
    s_phone_ui_dirty = 1;

    /* Siempre solicitar nombre real del canal, imagen del canal y comentarios en segundo plano */
    start_shorts_meta_request(player_video_id);

    player_url[0] = '\0';
    player_error_msg[0] = '\0';
    player_quality = youtube_max_quality();
    player_state = 1;
    /* En Shorts no activamos player_active de pantalla completa; se dibuja en el marco 9:16 de Shorts */
    player_active = 0;

    player_thread = sceKernelCreateThread("VitaTubeStreams", player_worker,
                                          0x10000100, 256 * 1024, 0, 0, NULL);
    if (player_thread < 0 || sceKernelStartThread(player_thread, 0, NULL) < 0) {
        player_state = -1;
    }
}

static void player_switch_quality(unsigned int new_q)
{
    if (player_video_id[0] == '\0') return;
    youtube_set_max_quality(new_q);
    player_quality = new_q;
    player_resume_ms = 0; /* Reiniciar al inicio de la nueva calidad sin provocar cuelgue por seek en datos no descargados */

    video_player_close();

    if (player_thread >= 0) {
        player_state = -2;
        sceKernelWaitThreadEnd(player_thread, NULL, NULL);
        sceKernelDeleteThread(player_thread);
        player_thread = -1;
    }

    player_url[0] = 0;
    player_error_msg[0] = 0;
    player_state = 1;
    player_overlay_timer = 240;

    player_thread = sceKernelCreateThread("VitaTubeStreams", player_worker,
                                          0x10000100, 256 * 1024, 0, 0, NULL);
    if (player_thread < 0 ||
        sceKernelStartThread(player_thread, 0, NULL) < 0) {
        player_state = -1;
    }
}

static void open_player_for_video(const YoutubeVideo *video)
{
    if (!video) return;
    history_add(video);

    video_player_close();

    if (player_thread >= 0) {
        player_state = -2;
        sceKernelWaitThreadEnd(player_thread, NULL, NULL);
        sceKernelDeleteThread(player_thread);
        player_thread = -1;
    }

    strncpy(player_video_id, video->id, sizeof(player_video_id) - 1);
    player_video_id[sizeof(player_video_id) - 1] = 0;
    strncpy(player_video_title, video->title, sizeof(player_video_title) - 1);
    player_video_title[sizeof(player_video_title) - 1] = 0;
    strncpy(player_video_author, video->channel, sizeof(player_video_author) - 1);
    player_video_author[sizeof(player_video_author) - 1] = 0;
    strncpy(player_video_published, video->published, sizeof(player_video_published) - 1);
    player_video_published[sizeof(player_video_published) - 1] = 0;
    player_video_duration = video->duration;
    player_video_views = video->views;
    player_video_is_live = (video->is_live || video->duration == 0);

    player_is_subscribed = youtube_is_channel_subscribed(player_video_author);
    player_like_state = youtube_get_video_rating(player_video_id);

    /* En los directos, solicitar 480p por defecto para máxima fluidez y evitar lag */
    if (player_video_is_live) {
        youtube_set_max_quality(480);
    }

    player_url[0] = 0;
    player_error_msg[0] = 0;
    player_quality = youtube_max_quality();
    player_state = 1;
    player_active = 1;
    player_fullscreen = 1;
    player_overlay_timer = 240;
    player_side_tab = 0;
    player_focus_section = PLAYER_FOCUS_CONTROLS;
    player_focus_btn = PLAYER_BTN_PLAY;
    player_carousel_idx = 0;
    player_carousel_scroll = 0.0f;
    player_show_desc = 0;
    player_show_comments = 0;
    shorts_comment_scroll = 0;
    player_show_settings = 0;
    player_settings_selected = 0;
    player_resume_ms = 0;

    free_author_avatar_texture();
    start_shorts_meta_request(player_video_id);

    /* Cargar videos relacionados inmediatamente desde los disponibles en linea */
    player_related_count = 0;
    for (unsigned int o = 0; o < online_video_count && player_related_count < 10; ++o) {
        if (strcmp(online_videos[o].id, video->id) != 0) {
            player_related_videos[player_related_count] = online_videos[o];
            player_related_count++;
        }
    }

    player_thread = sceKernelCreateThread("VitaTubeStreams", player_worker,
                                          0x10000100, 256 * 1024, 0, 0, NULL);
    if (player_thread < 0 ||
        sceKernelStartThread(player_thread, 0, NULL) < 0) {
        player_state = -1;
    }
}

static void update_player(void)
{
    if (shorts_meta_state == 2) {
        shorts_meta_state = 0;
        if (strcmp(player_video_id, shorts_meta_video_id) == 0) {
            if (shorts_meta_channel[0] != '\0') {
                snprintf(player_video_author, sizeof(player_video_author), "%s", shorts_meta_channel);
                player_is_subscribed = youtube_is_channel_subscribed(player_video_author);
                if (active_view == 1 && shorts_index < online_video_count &&
                    strcmp(online_videos[shorts_index].id, shorts_meta_video_id) == 0) {
                    snprintf(online_videos[shorts_index].channel,
                             sizeof(online_videos[shorts_index].channel), "%s", shorts_meta_channel);
                }
            }
            if (shorts_meta_avatar_bytes > 64) {
                free_author_avatar_texture();
                player_author_avatar_tex = safe_load_jpeg_thumbnail(
                    shorts_meta_avatar_buf, shorts_meta_avatar_bytes);
                shorts_meta_avatar_bytes = 0;
            }
        }
        s_phone_ui_dirty = 1;
    }

    if (player_state == 2) {
        player_state = 0;
        player_error_msg[0] = '\0';
        status_message = "Reproduciendo video";
        log_event("player_stream video=%s quality=%up",
                  player_video_id, player_quality);
        if (video_player_open(player_url) < 0) {
            const char *verr = video_player_get_error();
            snprintf(player_error_msg, sizeof(player_error_msg), "%s",
                     (verr && verr[0]) ? verr : "No se pudo abrir el reproductor");
            status_message = player_error_msg;
            player_overlay_timer = 240;
        } else {
            player_resume_ms = 0;
            video_player_set_volume(shorts_muted && active_view == 1 && !player_active ? 0 : player_volume);
            player_overlay_timer = 180;
        }
    } else if (player_state == -1) {
        snprintf(player_error_msg, sizeof(player_error_msg), "%s",
                 player_video_is_live
                     ? "No se pudo cargar este directo. Revisa tu conexion o intenta de nuevo."
                     : "No se pudo obtener el stream del video.");
        status_message = player_error_msg;
        log_event("player_stream_failed video=%s", player_video_id);
        player_state = 0;
        player_overlay_timer = 240;
    }

    if ((player_active || (active_view == 1 && !search_results_active)) &&
        player_error_msg[0] == '\0' &&
        video_player_get_status() == VPLAYER_ERROR) {
        const char *verr = video_player_get_error();
        snprintf(player_error_msg, sizeof(player_error_msg), "%s",
                 (verr && verr[0]) ? verr : "Error al cargar o reproducir el video");
        status_message = player_error_msg;
        log_event("player_playback_error video=%s msg=%s", player_video_id, player_error_msg);
        player_overlay_timer = 240;
    }
}

static void update_search_loading(void)
{
    if (search_worker_state == 2) {
        clear_thumbnails(search_thumbnails);
        clear_thumbnails(home_shorts_thumbnails);
        search_thumbnail_index = 0;
        search_worker_state = search_video_count > 0 ? 3 : (home_shorts_count > 0 ? 6 : 0);
        status_message = "Cargando miniaturas...";
        log_event("youtube_search videos=%u shorts=%u", search_video_count, home_shorts_count);
    }

    if (search_worker_state == 4) {
        if (!can_decode_thumbnail_this_frame()) {
            return;
        }
        if (search_thumbnail_index < ONLINE_VIDEO_CAPACITY) {
            if (search_thumbnails[search_thumbnail_index]) {
                vita2d_wait_rendering_done();
                vita2d_free_texture(search_thumbnails[search_thumbnail_index]);
                search_thumbnails[search_thumbnail_index] = NULL;
            }
            search_thumbnails[search_thumbnail_index] =
                safe_load_jpeg_thumbnail(search_thumbnail_buffer,
                                         search_thumbnail_bytes);
        }
        ++search_thumbnail_index;
        if (!home_shorts_thumbs_done &&
            (search_thumbnail_index == 3 || search_thumbnail_index >= search_video_count)) {
            home_shorts_thumb_idx = 0;
            search_worker_state = 6;
        } else if (search_thumbnail_index >= search_video_count) {
            search_worker_state = 0;
            status_message = "Resultados de InnerTube listos";
            log_event("youtube_search thumbnails_loaded=%u", search_video_count);
        } else {
            search_worker_state = 3;
        }
    } else if (search_worker_state == 5) {
        ++search_thumbnail_index;
        if (!home_shorts_thumbs_done &&
            (search_thumbnail_index == 3 || search_thumbnail_index >= search_video_count)) {
            home_shorts_thumb_idx = 0;
            search_worker_state = 6;
        } else if (search_thumbnail_index >= search_video_count) {
            search_worker_state = 0;
            status_message = "Resultados de InnerTube listos";
        } else {
            search_worker_state = 3;
        }
    } else if (search_worker_state == 7) {
        if (!can_decode_thumbnail_this_frame()) {
            return;
        }
        if (home_shorts_thumb_idx < HOME_SHORTS_SHELF_COLS &&
            home_shorts_thumb_idx < ONLINE_VIDEO_CAPACITY) {
            if (home_shorts_thumbnails[home_shorts_thumb_idx]) {
                vita2d_wait_rendering_done();
                vita2d_free_texture(home_shorts_thumbnails[home_shorts_thumb_idx]);
                home_shorts_thumbnails[home_shorts_thumb_idx] = NULL;
            }
            home_shorts_thumbnails[home_shorts_thumb_idx] =
                safe_load_jpeg_thumbnail(search_thumbnail_buffer, search_thumbnail_bytes);
        }
        ++home_shorts_thumb_idx;
        unsigned int max_sh = home_shorts_count < HOME_SHORTS_SHELF_COLS ? home_shorts_count : HOME_SHORTS_SHELF_COLS;
        if (home_shorts_thumb_idx >= max_sh) {
            home_shorts_thumbs_done = 1;
            search_worker_state = (search_thumbnail_index < search_video_count) ? 3 : 0;
        } else {
            search_worker_state = 6;
        }
    } else if (search_worker_state == 8) {
        ++home_shorts_thumb_idx;
        unsigned int max_sh = home_shorts_count < HOME_SHORTS_SHELF_COLS ? home_shorts_count : HOME_SHORTS_SHELF_COLS;
        if (home_shorts_thumb_idx >= max_sh) {
            home_shorts_thumbs_done = 1;
            search_worker_state = (search_thumbnail_index < search_video_count) ? 3 : 0;
        } else {
            search_worker_state = 6;
        }
    } else if (search_worker_state == -1) {
        search_worker_state = 0;
        status_message = "Sin conexion; mostrando resultados locales";
        log_event("youtube_search failed");
    }
    ++loading_frame;
}

static void update_startup_loading(void)
{
    if (active_view == 1 && !search_results_active &&
        startup_worker_state == 0 &&
        loaded_feed_view == 1 && online_video_count > 0 && shorts_playing_index < 0) {
        start_short_playback(shorts_index < online_video_count ? shorts_index : 0);
        return;
    }

    if (startup_worker_state == 4) {
        if (!can_decode_thumbnail_this_frame()) {
            return;
        }
        if (startup_thumbnail_index < ONLINE_VIDEO_CAPACITY) {
            vt_log(VT_LOG_DEBUG, "[Inicio] Decodificando miniatura %u bytes=%d",
                   startup_thumbnail_index, startup_thumbnail_bytes);
            if (online_thumbnails[startup_thumbnail_index]) {
                vita2d_wait_rendering_done();
                vita2d_free_texture(online_thumbnails[startup_thumbnail_index]);
                online_thumbnails[startup_thumbnail_index] = NULL;
            }
            online_thumbnails[startup_thumbnail_index] =
                safe_load_jpeg_thumbnail(startup_thumbnail_buffer,
                                         startup_thumbnail_bytes);
            vt_log(VT_LOG_DEBUG, "[Inicio] Miniatura %u textura=%s",
                   startup_thumbnail_index,
                   online_thumbnails[startup_thumbnail_index] ? "ok" : "null");
        }
        ++startup_thumbnail_index;
        /* En Inicio y Tendencias, apenas cargan las 3 primeras miniaturas de arriba, cargar las 5 de la fila de Shorts */
        if ((active_view == 0 || active_view == 2) && !home_shorts_thumbs_done &&
            (startup_thumbnail_index == 3 || startup_thumbnail_index >= online_video_count)) {
            home_shorts_thumb_idx = 0;
            startup_worker_state = 6;
        } else if (startup_thumbnail_index >= online_video_count) {
            startup_worker_state = 0;
            status_message = "Selecciona un video con X";
            log_event("home_thumbnails_loaded=%u", online_video_count);
        } else {
            startup_worker_state = 3;
        }
    } else if (startup_worker_state == 5) {
        ++startup_thumbnail_index;
        if ((active_view == 0 || active_view == 2) && !home_shorts_thumbs_done &&
            (startup_thumbnail_index == 3 || startup_thumbnail_index >= online_video_count)) {
            home_shorts_thumb_idx = 0;
            startup_worker_state = 6;
        } else if (startup_thumbnail_index >= online_video_count) {
            startup_worker_state = 0;
            status_message = "Selecciona un video con X";
        } else {
            startup_worker_state = 3;
        }
    } else if (startup_worker_state == 7) {
        if (!can_decode_thumbnail_this_frame()) {
            return;
        }
        if (home_shorts_thumb_idx < HOME_SHORTS_SHELF_COLS &&
            home_shorts_thumb_idx < ONLINE_VIDEO_CAPACITY) {
            if (home_shorts_thumbnails[home_shorts_thumb_idx]) {
                vita2d_wait_rendering_done();
                vita2d_free_texture(home_shorts_thumbnails[home_shorts_thumb_idx]);
                home_shorts_thumbnails[home_shorts_thumb_idx] = NULL;
            }
            home_shorts_thumbnails[home_shorts_thumb_idx] =
                safe_load_jpeg_thumbnail(startup_thumbnail_buffer, startup_thumbnail_bytes);
        }
        ++home_shorts_thumb_idx;
        unsigned int max_sh = home_shorts_count < HOME_SHORTS_SHELF_COLS ? home_shorts_count : HOME_SHORTS_SHELF_COLS;
        if (home_shorts_thumb_idx >= max_sh) {
            home_shorts_thumbs_done = 1;
            startup_worker_state = (startup_thumbnail_index < online_video_count) ? 3 : 0;
        } else {
            startup_worker_state = 6;
        }
    } else if (startup_worker_state == 8) {
        ++home_shorts_thumb_idx;
        unsigned int max_sh = home_shorts_count < HOME_SHORTS_SHELF_COLS ? home_shorts_count : HOME_SHORTS_SHELF_COLS;
        if (home_shorts_thumb_idx >= max_sh) {
            home_shorts_thumbs_done = 1;
            startup_worker_state = (startup_thumbnail_index < online_video_count) ? 3 : 0;
        } else {
            startup_worker_state = 6;
        }
    } else if (startup_worker_state == -1) {
        startup_worker_state = 0;
        status_message = (active_view == 1) ? "No se pudieron cargar los Shorts"
                       : (active_view == 2) ? "No se pudieron cargar las Tendencias"
                       : "Sin conexion; mostrando inicio local";
        log_event("home_feed_failed view=%u", active_view);
        vt_log(VT_LOG_ERROR, "[Feed] No se pudo cargar la seccion (view=%u)", active_view);
    }
    ++startup_frame;
}

static const unsigned int thumbnail_colors[CARD_COLUMNS] = {
    RGBA8(70, 105, 165, 255),
    RGBA8(160, 70, 80, 255),
    RGBA8(70, 135, 105, 255)
};

static void save_recent_searches(void)
{
    mkdir("ux0:data/VitaTube", 0777);
    FILE *f = fopen("ux0:data/VitaTube/searches.txt", "w");
    if (!f) return;
    for (unsigned int i = 0; i < recent_search_count; i++) {
        fprintf(f, "%s\n", recent_searches[i]);
    }
    fclose(f);
}

static void load_recent_searches(void)
{
    FILE *f = fopen("ux0:data/VitaTube/searches.txt", "r");
    if (!f) return;
    char line[128];
    unsigned int count = 0;
    while (fgets(line, sizeof(line), f) && count < SEARCH_HISTORY_MAX) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
            line[len - 1] = 0;
            len--;
        }
        if (len > 0) {
            strncpy(recent_searches[count], line, sizeof(recent_searches[0]) - 1);
            recent_searches[count][sizeof(recent_searches[0]) - 1] = 0;
            count++;
        }
    }
    if (count > 0) {
        recent_search_count = count;
    }
    fclose(f);
}

static void add_recent_search(const char *query)
{
    if (!query || !query[0]) return;
    for (unsigned int i = 0; i < recent_search_count; i++) {
        if (strcmp(recent_searches[i], query) == 0) {
            char tmp[64];
            strncpy(tmp, recent_searches[i], sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = 0;
            for (unsigned int j = i; j > 0; j--) {
                strcpy(recent_searches[j], recent_searches[j - 1]);
            }
            strcpy(recent_searches[0], tmp);
            save_recent_searches();
            return;
        }
    }
    for (unsigned int j = SEARCH_HISTORY_MAX - 1; j > 0; j--) {
        strcpy(recent_searches[j], recent_searches[j - 1]);
    }
    strncpy(recent_searches[0], query, sizeof(recent_searches[0]) - 1);
    recent_searches[0][sizeof(recent_searches[0]) - 1] = 0;
    if (recent_search_count < SEARCH_HISTORY_MAX) recent_search_count++;
    save_recent_searches();
}

static void keyboard_backspace(void)
{
    size_t len = strlen(keyboard_text);
    if (len == 0) return;
    if ((unsigned char)keyboard_text[len - 1] >= 0x80 && (unsigned char)keyboard_text[len - 1] <= 0xBF) {
        if (len >= 2) {
            keyboard_text[len - 2] = 0;
            return;
        }
    }
    keyboard_text[len - 1] = 0;
}

static void open_search_dialog(void)
{
    if (keyboard_active) {
        return;
    }

    keyboard_text[0] = 0;
    keyboard_row = 0;
    keyboard_column = 0;
    keyboard_shift = 0;
    keyboard_touch_latched = 0;
    keyboard_active = 1;
    keyboard_target = 0;
    search_section = SEARCH_SECTION_KEYBOARD;
    search_history_selected = 0;
    search_carousel_selected = 0;
    search_symbols_mode = 0;
    live_suggestions_count = 0;
    suggestions_last_fetched[0] = 0;
    suggestions_query[0] = 0;
    suggestions_worker_state = 0;
    log_event("keyboard_open");
}

static void open_account_keyboard(unsigned int field)
{
    keyboard_text[0] = 0;
    keyboard_row = 0;
    keyboard_column = 0;
    keyboard_shift = 0;
    keyboard_touch_latched = 0;
    keyboard_target = field + 1;
    keyboard_active = 1;
}

static void submit_account(void)
{
    int result;
    unsigned int account_operation = account_view;

    if (!account_username[0] || !account_password[0] ||
        (account_view == 2 &&
         strcmp(account_password, account_confirm_password) != 0)) {
        account_status = "Completa los campos y confirma la contrasena";
        return;
    }
    account_status = account_view == 1 ? "Iniciando sesion..." :
        "Registrando cuenta...";
    result = account_view == 1
        ? youtube_login(account_username, account_password)
        : youtube_register(account_username, account_password);
    if (result == 0) {
        account_status = "Sesion iniciada correctamente";
        account_view = 0;
        active_view = 0;
        content_offset = 0;
        selected_row = 0;
        selected_column = 0;
        clear_thumbnails(online_thumbnails);
        online_video_count = 0;
        subs_video_count = 0;
        subs_worker_state = 0;
        start_startup_request();
        log_event("account_success mode=%u user=%s", account_operation,
                  account_username);
    } else {
        account_status = account_view == 1
            ? "No se pudo iniciar sesion"
            : "No se pudo registrar la cuenta";
        log_event("account_failed mode=%u result=%d", account_operation, result);
    }
}

static void draw_text(float x, float y, unsigned int color, float scale,
                      const char *text)
{
    if (font) {
        vita2d_pgf_draw_text(font, x, y, color, scale, text);
    }
}

static void keyboard_add_char(char value)
{
    size_t length = strlen(keyboard_text);

    if (length + 1 < sizeof(keyboard_text)) {
        keyboard_text[length] = keyboard_shift
            && value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value;
        keyboard_text[length + 1] = 0;
    }
}

static void keyboard_confirm(void)
{
    if (keyboard_target != 0) {
        if (keyboard_target == 1) {
            strncpy(account_username, keyboard_text,
                    sizeof(account_username) - 1);
            account_username[sizeof(account_username) - 1] = 0;
        } else if (keyboard_target == 2) {
            strncpy(account_password, keyboard_text,
                    sizeof(account_password) - 1);
            account_password[sizeof(account_password) - 1] = 0;
        } else {
            strncpy(account_confirm_password, keyboard_text,
                    sizeof(account_confirm_password) - 1);
            account_confirm_password[sizeof(account_confirm_password) - 1] = 0;
        }
        keyboard_active = 0;
        account_field = keyboard_target;
        account_status = "Campo actualizado";
        return;
    }
    strcpy(search_label, keyboard_text);
    search_results_active = search_label[0] != 0;
    if (search_results_active) {
        add_recent_search(search_label);
    }
    keyboard_active = 0;
    active_view = 0;
    selected_row = 0;
    selected_column = 0;
    content_offset = 0;
    if (search_results_active) {
        clear_thumbnails(search_thumbnails);
        search_video_count = 0;
        status_message = "Buscando...";
        start_search_request();
        log_event("youtube_search_started value=%s", search_label);
    } else {
        search_video_count = 0;
        status_message = "Escribe algo para buscar";
    }
    log_event("keyboard_confirm value=%s", search_label);
}

static void keyboard_press(void)
{
    unsigned int length = keyboard_row < 4
        ? (unsigned int)strlen(keyboard_rows[keyboard_row]) : 0;

    if (keyboard_row < 4 && keyboard_column < length) {
        keyboard_add_char(keyboard_rows[keyboard_row][keyboard_column]);
        return;
    }

    if (keyboard_row == 4 && keyboard_column == 0) {
        if (strlen(keyboard_text) > 0) {
            keyboard_text[strlen(keyboard_text) - 1] = 0;
        }
    } else if (keyboard_row == 4 && keyboard_column == 1) {
        keyboard_add_char(' ');
    } else if (keyboard_row == 4 && keyboard_column == 2) {
        keyboard_shift = !keyboard_shift;
    } else if (keyboard_row == 4 && keyboard_column == 3) {
        keyboard_confirm();
    } else if (keyboard_row == 4 && keyboard_column == 4) {
        keyboard_active = 0;
        account_status = keyboard_target != 0 ? "Edicion cancelada" :
            account_status;
        status_message = keyboard_target == 0 ? "Busqueda cancelada" : status_message;
    }
}

static void draw_keyboard(void)
{
    unsigned int row;

    vita2d_draw_rectangle(0, 270, 960, 274, RGBA8(20, 20, 20, 250));
    const char *keyboard_title = keyboard_target == 0 ? "Buscar en VitaTube"
        : keyboard_target == 1 ? "Escribe tu usuario"
        : keyboard_target == 2 ? "Escribe tu contrasena"
        : "Confirma tu contrasena";
    const char *keyboard_action = keyboard_target == 0 ? "BUSCAR" : "ACEPTAR";
    draw_text(35, 278, RGBA8(235, 238, 245, 255), 0.72f,
              keyboard_title);
    vita2d_draw_rectangle(35, 285, 890, 38, RGBA8(50, 53, 64, 255));
    draw_text(52, 311, RGBA8(245, 245, 248, 255), 0.8f,
              keyboard_text[0] ? keyboard_text :
              (keyboard_target == 0 ? "Escribe tu busqueda" : "Escribe aqui"));

    for (row = 0; row < 4; ++row) {
        unsigned int column;
        unsigned int length = (unsigned int)strlen(keyboard_rows[row]);
        float width = row == 0 ? 75.0f : 82.0f;
        float start = row == 0 ? 105.0f : 70.0f;

        for (column = 0; column < length; ++column) {
            char label[2] = {keyboard_rows[row][column], 0};
            float x = start + column * width;
            unsigned int color = row == keyboard_row &&
                                 column == keyboard_column
                ? RGBA8(235, 35, 50, 255) : RGBA8(58, 61, 72, 255);
            vita2d_draw_rectangle(x, 325 + row * 36, width - 6, 30, color);
            draw_text(x + 27, 347 + row * 36, RGBA8(245, 245, 248, 255),
                      0.78f, label);
        }
    }

    vita2d_draw_rectangle(35, 470, 175, 42, RGBA8(58, 61, 72, 255));
    vita2d_draw_rectangle(220, 470, 175, 42, RGBA8(58, 61, 72, 255));
    vita2d_draw_rectangle(405, 470, 175, 42, RGBA8(58, 61, 72, 255));
    vita2d_draw_rectangle(590, 470, 175, 42, RGBA8(205, 35, 48, 255));
    vita2d_draw_rectangle(775, 470, 150, 42, RGBA8(58, 61, 72, 255));
    draw_text(70, 497, RGBA8(220, 223, 230, 255), 0.7f, "Borrar");
    draw_text(260, 497, RGBA8(220, 223, 230, 255), 0.7f, "Espacio");
    draw_text(460, 497, RGBA8(220, 223, 230, 255), 0.7f,
              keyboard_shift ? "Mayus: SI" : "Mayus: NO");
    draw_text(650, 497, RGBA8(255, 255, 255, 255), 0.7f, keyboard_action);
    draw_text(820, 497, RGBA8(220, 223, 230, 255), 0.7f, "CERRAR");
}

static void draw_rounded_rectangle(float x, float y, float w, float h, float r, unsigned int color)
{
    if (r <= 1.0f || w < 6.0f || h < 6.0f) {
        vita2d_draw_rectangle(x, y, w, h, color);
        return;
    }
    if (r * 2.0f > w) r = w * 0.5f;
    if (r * 2.0f > h) r = h * 0.5f;

    float step1 = r * 0.65f;
    float step2 = r * 0.30f;
    vita2d_draw_rectangle(x + step1, y, w - 2.0f * step1, step2, color);
    vita2d_draw_rectangle(x + step2, y + step2, w - 2.0f * step2, step1 - step2, color);
    vita2d_draw_rectangle(x, y + step1, w, h - 2.0f * step1, color);
    vita2d_draw_rectangle(x + step2, y + h - step1, w - 2.0f * step2, step1 - step2, color);
    vita2d_draw_rectangle(x + step1, y + h - step2, w - 2.0f * step1, step2, color);
}

static void draw_rounded_outline(float x, float y, float w, float h, float r, float thickness, unsigned int color)
{
    if (r * 2.0f > w) r = w * 0.5f;
    if (r * 2.0f > h) r = h * 0.5f;
    float c = r * 0.55f;
    vita2d_draw_rectangle(x + c, y, w - 2.0f * c, thickness, color);
    vita2d_draw_rectangle(x + c, y + h - thickness, w - 2.0f * c, thickness, color);
    vita2d_draw_rectangle(x, y + c, thickness, h - 2.0f * c, color);
    vita2d_draw_rectangle(x + w - thickness, y + c, thickness, h - 2.0f * c, color);

    vita2d_draw_line(x + c, y, x, y + c, color);
    vita2d_draw_line(x + w - c, y, x + w, y + c, color);
    vita2d_draw_line(x, y + h - c, x + c, y + h, color);
    vita2d_draw_line(x + w - c, y + h, x + w, y + h - c, color);
}

static void draw_rounded_corner_mask(float x, float y, float w, float h, float r, unsigned int bg_color)
{
    float r2 = r * r;
    for (float dy = 0.0f; dy < r; dy += 2.0f) {
        float val = r2 - (r - dy) * (r - dy);
        float dx = val > 0.0f ? r - sqrtf(val) : r;
        float strip_h = (dy + 2.0f <= r) ? 2.0f : (r - dy);
        if (dx > 0.5f && strip_h > 0.0f) {
            vita2d_draw_rectangle(x, y + dy, dx, strip_h, bg_color);
            vita2d_draw_rectangle(x + w - dx, y + dy, dx, strip_h, bg_color);
            vita2d_draw_rectangle(x, y + h - dy - strip_h, dx, strip_h, bg_color);
            vita2d_draw_rectangle(x + w - dx, y + h - dy - strip_h, dx, strip_h, bg_color);
        }
    }
}

static void draw_sidebar_icon(unsigned int index, float y, unsigned int color)
{
    float cx = 28.0f;
    float cy = y;

    if (index == 0) {
        /* Inicio / Home: Casa minimalista */
        vita2d_draw_line(cx - 9.0f, cy, cx, cy - 8.0f, color);
        vita2d_draw_line(cx + 9.0f, cy, cx, cy - 8.0f, color);
        vita2d_draw_line(cx - 9.0f, cy + 1.0f, cx, cy - 7.0f, color);
        vita2d_draw_line(cx + 9.0f, cy + 1.0f, cx, cy - 7.0f, color);
        vita2d_draw_rectangle(cx - 7.0f, cy, 14.0f, 9.0f, color);
        vita2d_draw_rectangle(cx - 2.0f, cy + 3.0f, 4.0f, 6.0f, RGBA8(15, 15, 15, 255));
    } else if (index == 1) {
        /* Shorts: Rectangulo redondeado estilizado */
        draw_rounded_rectangle(cx - 6.0f, cy - 8.0f, 13.0f, 16.0f, 3.5f, color);
        /* Play blanco en el centro */
        for (int p = 0; p <= 4; p++) {
            vita2d_draw_line(cx - 2.0f + p, cy - 3.0f + p * 0.75f, cx - 2.0f + p, cy + 3.0f - p * 0.75f, RGBA8(15, 15, 15, 255));
        }
    } else if (index == 2) {
        /* Tendencias / Explorar: Brujula limpia */
        vita2d_draw_line(cx - 7.0f, cy, cx + 7.0f, cy, color);
        vita2d_draw_line(cx, cy - 7.0f, cx, cy + 7.0f, color);
        vita2d_draw_line(cx - 5.0f, cy - 5.0f, cx + 5.0f, cy + 5.0f, color);
        vita2d_draw_line(cx - 5.0f, cy + 5.0f, cx + 5.0f, cy - 5.0f, color);
        vita2d_draw_fill_circle(cx, cy, 2.5f, color);
    } else if (index == 3) {
        /* Suscripciones: Capas superpuestas clasicas de YouTube */
        vita2d_draw_rectangle(cx - 6.0f, cy - 8.0f, 12.0f, 2.0f, color);
        vita2d_draw_rectangle(cx - 8.0f, cy - 4.0f, 16.0f, 2.0f, color);
        vita2d_draw_rectangle(cx - 9.0f, cy, 18.0f, 10.0f, color);
        for (int p = 0; p <= 4; p++) {
            vita2d_draw_line(cx - 2.0f + p, cy + 2.0f + p * 0.7f, cx - 2.0f + p, cy + 8.0f - p * 0.7f, RGBA8(15, 15, 15, 255));
        }
    } else if (index == 4) {
        /* Biblioteca / Historial: Carpeta con play */
        draw_rounded_rectangle(cx - 8.0f, cy - 6.0f, 16.0f, 13.0f, 2.0f, color);
        vita2d_draw_rectangle(cx - 5.0f, cy - 8.0f, 6.0f, 2.0f, color);
        for (int p = 0; p <= 3; p++) {
            vita2d_draw_line(cx - 1.0f + p, cy - 2.0f + p * 0.7f, cx - 1.0f + p, cy + 3.0f - p * 0.7f, RGBA8(15, 15, 15, 255));
        }
    } else {
        /* Configuracion: Engranaje minimalista */
        vita2d_draw_fill_circle(cx, cy, 7.0f, color);
        vita2d_draw_rectangle(cx - 8.0f, cy - 2.0f, 16.0f, 4.0f, color);
        vita2d_draw_rectangle(cx - 2.0f, cy - 8.0f, 4.0f, 16.0f, color);
        vita2d_draw_fill_circle(cx, cy, 3.0f, RGBA8(15, 15, 15, 255));
    }
}

static void shorten_title(const char *source, char *target,
                          unsigned int target_size)
{
    unsigned int length;

    if (target_size == 0) return;
    if (!source) {
        target[0] = 0;
        return;
    }

    length = (unsigned int)strlen(source);
    if (length + 1 < target_size) {
        strcpy(target, source);
        return;
    }
    if (target_size < 4) {
        strncpy(target, source, target_size - 1);
        target[target_size - 1] = 0;
        return;
    }

    unsigned int max_len = target_size - 4;
    while (max_len > 0 && ((unsigned char)source[max_len] & 0xC0) == 0x80) {
        max_len--;
    }
    if (max_len > 0 && ((unsigned char)source[max_len] & 0x80) != 0) {
        max_len--;
    }

    memcpy(target, source, max_len);
    target[max_len] = '.';
    target[max_len + 1] = '.';
    target[max_len + 2] = '.';
    target[max_len + 3] = 0;
}

static void draw_title_with_emojis(float x, float y, unsigned int color, float scale, const char *text)
{
    if (!font || !text || !text[0]) return;

    float cur_x = x;
    char buffer[128];
    unsigned int buf_len = 0;
    const unsigned char *p = (const unsigned char *)text;

    while (*p) {
        if (*p >= 0xF0 && *p <= 0xF4 && *(p + 1) && *(p + 2) && *(p + 3)) {
            unsigned int codepoint = (((unsigned int)(p[0] & 0x07)) << 18) |
                                     (((unsigned int)(p[1] & 0x3F)) << 12) |
                                     (((unsigned int)(p[2] & 0x3F)) << 6) |
                                     ((unsigned int)(p[3] & 0x3F));
            if (buf_len > 0) {
                buffer[buf_len] = 0;
                vita2d_pgf_draw_text(font, (int)cur_x, (int)y, color, scale, buffer);
                cur_x += (float)vita2d_pgf_text_width(font, scale, buffer);
                buf_len = 0;
            }

            p += 4;

            if (codepoint == 0x1F3AE) {
                /* 🎮 Control morado de videojuegos (estilo YouTube TV) */
                float w = 17.0f * (scale / 0.60f);
                float h = 11.5f * (scale / 0.60f);
                float ey = y - 10.5f * (scale / 0.60f);
                draw_rounded_rectangle(cur_x, ey, w, h, 3.5f, RGBA8(142, 68, 173, 255));
                vita2d_draw_rectangle(cur_x + 2.0f, ey - 1.0f, 3.0f, 1.0f, RGBA8(115, 50, 140, 255));
                vita2d_draw_rectangle(cur_x + w - 5.0f, ey - 1.0f, 3.0f, 1.0f, RGBA8(115, 50, 140, 255));
                vita2d_draw_rectangle(cur_x + 3.0f, ey + 4.0f, 5.0f, 2.0f, RGBA8(255, 255, 255, 255));
                vita2d_draw_rectangle(cur_x + 4.5f, ey + 2.5f, 2.0f, 5.0f, RGBA8(255, 255, 255, 255));
                vita2d_draw_fill_circle(cur_x + w - 4.5f, ey + 3.5f, 0.9f, RGBA8(255, 255, 255, 255));
                vita2d_draw_fill_circle(cur_x + w - 2.5f, ey + 5.5f, 0.9f, RGBA8(255, 255, 255, 255));
                vita2d_draw_fill_circle(cur_x + w - 6.5f, ey + 5.5f, 0.9f, RGBA8(255, 255, 255, 255));
                vita2d_draw_fill_circle(cur_x + w - 4.5f, ey + 7.5f, 0.9f, RGBA8(255, 255, 255, 255));
                cur_x += w + 4.0f;
            } else if (codepoint == 0x1F534) {
                /* 🔴 Circulo rojo directo */
                float r = 5.0f * (scale / 0.60f);
                vita2d_draw_fill_circle(cur_x + r, y - 5.0f, r, RGBA8(235, 30, 40, 255));
                cur_x += r * 2.0f + 4.0f;
            } else if (codepoint == 0x1F525) {
                /* 🔥 Fuego */
                draw_rounded_rectangle(cur_x + 1.0f, y - 10.0f, 9.0f, 10.0f, 3.0f, RGBA8(255, 100, 0, 255));
                vita2d_draw_fill_circle(cur_x + 5.5f, y - 4.0f, 2.5f, RGBA8(255, 220, 0, 255));
                cur_x += 13.0f;
            } else if (codepoint == 0x1F3C6 || codepoint == 0x1F31F) {
                /* 🏆 Trofeo / Estrella */
                draw_rounded_rectangle(cur_x + 1.0f, y - 10.0f, 9.0f, 10.0f, 2.0f, RGBA8(255, 200, 0, 255));
                cur_x += 13.0f;
            } else {
                draw_rounded_rectangle(cur_x, y - 9.0f, 10.0f, 9.0f, 2.0f, RGBA8(120, 140, 200, 255));
                cur_x += 14.0f;
            }
        } else {
            if (buf_len + 1 < sizeof(buffer)) {
                buffer[buf_len++] = (char)*p;
            }
            p++;
        }
    }

    if (buf_len > 0) {
        buffer[buf_len] = 0;
        vita2d_pgf_draw_text(font, (int)cur_x, (int)y, color, scale, buffer);
    }
}

static void format_duration(unsigned int seconds, char *output,
                            unsigned int output_size)
{
    if (seconds >= 3600) {
        snprintf(output, output_size, "%u:%02u:%02u", seconds / 3600,
                 (seconds / 60) % 60, seconds % 60);
    } else {
        snprintf(output, output_size, "%u:%02u", seconds / 60,
                 seconds % 60);
    }
}

static void format_views(unsigned long views, char *output,
                         unsigned int output_size)
{
    if (views >= 1000000) {
        snprintf(output, output_size, "%.1f M", views / 1000000.0);
    } else if (views >= 1000) {
        snprintf(output, output_size, "%.1f K", views / 1000.0);
    } else {
        snprintf(output, output_size, "%lu", views);
    }
}

static vita2d_texture *find_history_card_texture(unsigned int video_index, const YoutubeVideo *hv)
{
    if (!hv || !hv->id[0]) return NULL;
    if (video_index < ONLINE_VIDEO_CAPACITY &&
        history_thumbnails[video_index] &&
        strcmp(history_thumb_ids[video_index], hv->id) == 0) {
        return history_thumbnails[video_index];
    }
    for (unsigned int i = 0; i < online_video_count; i++) {
        if (online_thumbnails[i] && strcmp(online_videos[i].id, hv->id) == 0) {
            return online_thumbnails[i];
        }
    }
    for (unsigned int i = 0; i < subs_video_count; i++) {
        if (subs_thumbnails[i] && strcmp(subs_videos[i].id, hv->id) == 0) {
            return subs_thumbnails[i];
        }
    }
    for (unsigned int i = 0; i < search_video_count; i++) {
        if (search_thumbnails[i] && strcmp(search_videos[i].id, hv->id) == 0) {
            return search_thumbnails[i];
        }
    }
    return NULL;
}

static void open_home_short_at(unsigned int short_idx)
{
    if (short_idx >= home_shorts_count) return;
    stop_startup_worker();
    clear_thumbnails(online_thumbnails);
    for (unsigned int i = 0; i < home_shorts_count && i < ONLINE_VIDEO_CAPACITY; i++) {
        online_videos[i] = home_shorts_videos[i];
        online_thumbnails[i] = home_shorts_thumbnails[i];
        home_shorts_thumbnails[i] = NULL;
    }
    online_video_count = home_shorts_count;
    loaded_feed_view = 1;
    active_view = 1;
    selected_sidebar = 1;
    sidebar_active = 0;
    search_results_active = 0;
    status_message = "Reproduciendo Shorts";
    start_short_playback(short_idx);
}

static void draw_home_shorts_shelf(void)
{
    /* Cabecera "Shorts" con icono rojo de YouTube Shorts a la izquierda */
    float hx = 76.0f;
    float hy = 258.0f;
    draw_rounded_rectangle(hx, hy, 15.0f, 18.0f, 4.5f, RGBA8(255, 0, 35, 255));
    for (int p = 0; p <= 5; p++) {
        vita2d_draw_line(hx + 5.5f + (float)p, hy + 5.5f + (float)p * 0.7f,
                         hx + 5.5f + (float)p, hy + 12.5f - (float)p * 0.7f,
                         RGBA8(255, 255, 255, 255));
    }
    draw_text(hx + 22.0f, hy + 15.0f, RGBA8(255, 255, 255, 255), 0.68f, "Shorts");

    /* 3 puntos verticales a la derecha del encabezado Shorts */
    vita2d_draw_fill_circle(934.0f, hy + 5.0f, 1.5f, RGBA8(200, 200, 200, 255));
    vita2d_draw_fill_circle(934.0f, hy + 10.0f, 1.5f, RGBA8(200, 200, 200, 255));
    vita2d_draw_fill_circle(934.0f, hy + 15.0f, 1.5f, RGBA8(200, 200, 200, 255));

    unsigned int cols = home_shorts_count < HOME_SHORTS_SHELF_COLS
        ? home_shorts_count : HOME_SHORTS_SHELF_COLS;

    for (unsigned int c = 0; c < cols; c++) {
        float sx = 76.0f + (float)c * 174.0f;
        float sy = 282.0f;
        float sw = 162.0f;
        float sh = 190.0f;
        int is_sel = (!sidebar_active && selected_row == 1 && selected_column == c);

        if (is_sel) {
            draw_rounded_outline(sx - 3.0f, sy - 3.0f, sw + 6.0f, sh + 6.0f, 11.0f, 2.5f,
                                 RGBA8(255, 255, 255, 255));
        }

        const YoutubeVideo *sv = &home_shorts_videos[c];
        vita2d_texture *stex = home_shorts_thumbnails[c];

        if (stex) {
            float tw = (float)vita2d_texture_get_width(stex);
            float th = (float)vita2d_texture_get_height(stex);
            if (tw > 0.0f && th > 0.0f) {
                /* Recortar la franja central vertical 9:16 de la miniatura 16:9 para llenar la tarjeta */
                float target_aspect = sw / sh;
                float crop_w = th * target_aspect;
                if (crop_w > tw) crop_w = tw;
                float crop_x = (tw - crop_w) * 0.5f;
                vita2d_draw_texture_part_scale(stex, sx, sy, crop_x, 0.0f, crop_w, th,
                                               sw / crop_w, sh / th);
            }
        } else {
            draw_rounded_rectangle(sx, sy, sw, sh, 9.0f, RGBA8(32, 32, 36, 255));
            vita2d_draw_fill_circle(sx + sw * 0.5f, sy + sh * 0.5f, 16.0f, RGBA8(48, 48, 54, 255));
            for (int p = 0; p <= 7; p++) {
                vita2d_draw_line(sx + sw * 0.5f - 3.5f + (float)p, sy + sh * 0.5f - 5.5f + (float)p * 0.75f,
                                 sx + sw * 0.5f - 3.5f + (float)p, sy + sh * 0.5f + 5.5f - (float)p * 0.75f,
                                 RGBA8(140, 140, 145, 255));
            }
        }

        draw_rounded_corner_mask(sx, sy, sw, sh, 9.0f, RGBA8(15, 15, 15, 255));

        /* Titulo en 2 lineas compactas + 3 puntos a la derecha + visualizaciones */
        const char *raw_t = sv->title[0] ? sv->title : "YouTube Short";
        char l1[24] = {0};
        char l2[24] = {0};
        size_t tlen = strlen(raw_t);
        if (tlen <= 20) {
            snprintf(l1, sizeof(l1), "%s", raw_t);
        } else {
            size_t sp = 20;
            while (sp > 10 && raw_t[sp] != ' ') sp--;
            if (raw_t[sp] != ' ') sp = 20;
            memcpy(l1, raw_t, sp);
            l1[sp] = '\0';
            const char *rem = raw_t + sp;
            while (*rem == ' ') rem++;
            shorten_title(rem, l2, 21);
        }

        draw_title_with_emojis(sx + 2.0f, sy + sh + 15.0f, RGBA8(255, 255, 255, 255),
                               is_sel ? 0.50f : 0.48f, l1);
        /* Icono 3 puntos de cada tarjeta Short */
        vita2d_draw_fill_circle(sx + sw - 5.0f, sy + sh + 9.0f, 1.2f, RGBA8(190, 190, 190, 255));
        vita2d_draw_fill_circle(sx + sw - 5.0f, sy + sh + 13.0f, 1.2f, RGBA8(190, 190, 190, 255));
        vita2d_draw_fill_circle(sx + sw - 5.0f, sy + sh + 17.0f, 1.2f, RGBA8(190, 190, 190, 255));

        float v_y = sy + sh + 28.0f;
        if (l2[0] != '\0') {
            draw_title_with_emojis(sx + 2.0f, sy + sh + 28.0f, RGBA8(255, 255, 255, 255),
                                   is_sel ? 0.50f : 0.48f, l2);
            v_y = sy + sh + 41.0f;
        }

        char v_num[24];
        char v_line[44];
        format_views(sv->views > 0 ? sv->views : 37200, v_num, sizeof(v_num));
        snprintf(v_line, sizeof(v_line), "%s visualizaciones", v_num);
        draw_text(sx + 2.0f, v_y, RGBA8(165, 165, 165, 255), 0.43f, v_line);
    }
}

static void draw_card(unsigned int row, unsigned int column)
{
    int shelf_active = has_home_shorts_shelf();
    const float card_w = 274.0f;
    const float card_h = shelf_active ? 126.0f : 154.0f;
    const float x = 76.0f + column * 292.0f;
    const float y = shelf_active ? 80.0f : (88.0f + row * 216.0f);
    const unsigned int selected =
        row == selected_row && column == selected_column && !sidebar_active;

    /* Contorno blanco fino si está enfocado (YouTube TV style) */
    if (selected) {
        draw_rounded_outline(x - 3.0f, y - 3.0f, card_w + 6.0f, card_h + 6.0f, 13.0f, 2.5f,
                             RGBA8(255, 255, 255, 255));
    }

    unsigned int video_index = content_offset + row * CARD_COLUMNS + column;
    const unsigned int remote_view = active_view <= 2;
    const unsigned int is_subs_view = (active_view == 3 && !search_results_active);
    const unsigned int is_history_view = (active_view == 4 && !search_results_active);
    YoutubeVideo *videos = search_results_active ? search_videos : online_videos;
    vita2d_texture **textures = search_results_active
        ? search_thumbnails : online_thumbnails;
    unsigned int video_count = search_results_active
        ? search_video_count : (remote_view ? online_video_count : (is_subs_view ? subs_video_count : (is_history_view ? history_count() : 0)));
    const YoutubeVideo *hist_video = (is_history_view && video_index < video_count) ? history_get(video_index) : NULL;
    const YoutubeVideo *subs_video = (is_subs_view && video_index < video_count) ? &subs_videos[video_index] : NULL;
    const YoutubeVideo *card_video = hist_video ? hist_video : (subs_video ? subs_video : (video_index < video_count ? &videos[video_index] : NULL));
    const char *title = card_video
        ? card_video->title
        : (active_view == 3 ? "Suscribete a un canal o inicia sesion"
           : active_view == 4 ? "No hay video en esta posicion"
           : video_titles[row][column]);
    char visible_title[42];
    char duration_text[16] = {0};
    char views_text[24];
    char metadata[96];
    char visible_metadata[64];
    const char *channel = (card_video && card_video->channel[0])
        ? card_video->channel : "Canal desconocido";

    vita2d_texture *card_tex = NULL;
    if (is_history_view && hist_video) {
        card_tex = find_history_card_texture(video_index, hist_video);
    } else if (is_subs_view && video_index < subs_video_count) {
        card_tex = subs_thumbnails[video_index];
    } else if (!is_history_view && !is_subs_view && video_index < video_count) {
        card_tex = textures[video_index];
    }

    /* Dibujar miniatura con esquinas redondeadas */
    if (card_tex) {
        float texture_width = (float)vita2d_texture_get_width(card_tex);
        float texture_height = (float)vita2d_texture_get_height(card_tex);
        float scale_x = texture_width > 0.0f ? card_w / texture_width : 1.0f;
        float scale_y = texture_height > 0.0f ? card_h / texture_height : 1.0f;
        vita2d_draw_texture_scale(card_tex, x, y, scale_x, scale_y);
    } else {
        draw_rounded_rectangle(x, y, card_w, card_h, 10.0f, RGBA8(32, 32, 32, 255));
    }

    /* Redondear esquinas de la miniatura usando máscara del color de fondo (#0f0f0f) */
    draw_rounded_corner_mask(x, y, card_w, card_h, 10.0f, RGBA8(15, 15, 15, 255));

    if (!card_tex) {
        vita2d_draw_fill_circle(x + card_w * 0.5f, y + card_h * 0.5f, 18.0f, RGBA8(48, 48, 48, 255));
        for (int p = 0; p <= 8; p++) {
            vita2d_draw_line(x + card_w * 0.5f - 4.0f + p, y + card_h * 0.5f - 6.0f + p * 0.75f,
                             x + card_w * 0.5f - 4.0f + p, y + card_h * 0.5f + 6.0f - p * 0.75f,
                             RGBA8(120, 120, 120, 255));
        }
    }

    if (active_view == 1 && card_video) {
        draw_rounded_rectangle(x + 8.0f, y + 8.0f, 60.0f, 20.0f, 4.0f, RGBA8(204, 0, 0, 230));
        draw_text(x + 14.0f, y + 23.0f, RGBA8(255, 255, 255, 255), 0.48f, "SHORTS");
    }

    /* Badge de duracion o EN VIVO en la esquina inferior derecha */
    if (card_video) {
        if (card_video->is_live) {
            float badge_w = 78.0f;
            float badge_h = 20.0f;
            float badge_x = x + card_w - badge_w - 6.0f;
            float badge_y = y + card_h - badge_h - 6.0f;
            draw_rounded_rectangle(badge_x, badge_y, badge_w, badge_h, 4.0f,
                                  RGBA8(220, 10, 35, 255));
            /* Icono broadcast ((•)) */
            float icx = badge_x + 12.0f;
            float icy = badge_y + 10.0f;
            vita2d_draw_fill_circle(icx, icy, 1.8f, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx - 3.5f, icy - 2.5f, icx - 4.5f, icy, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx - 4.5f, icy, icx - 3.5f, icy + 2.5f, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx + 3.5f, icy - 2.5f, icx + 4.5f, icy, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx + 4.5f, icy, icx + 3.5f, icy + 2.5f, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx - 6.0f, icy - 4.5f, icx - 7.5f, icy, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx - 7.5f, icy, icx - 6.0f, icy + 4.5f, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx + 6.0f, icy - 4.5f, icx + 7.5f, icy, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx + 7.5f, icy, icx + 6.0f, icy + 4.5f, RGBA8(255, 255, 255, 255));
            draw_text(badge_x + 22.0f, badge_y + 14.5f, RGBA8(255, 255, 255, 255),
                      0.52f, "EN VIVO");
        } else if (card_video->duration > 0 && card_video->duration < 86400) {
            format_duration(card_video->duration, duration_text, sizeof(duration_text));
            float tw = (float)vita2d_pgf_text_width(font, 0.44f, duration_text);
            float badge_w = tw + 10.0f;
            float badge_h = 16.0f;
            float badge_x = x + card_w - badge_w - 6.0f;
            float badge_y = y + card_h - badge_h - 6.0f;
            draw_rounded_rectangle(badge_x, badge_y, badge_w, badge_h, 3.5f, RGBA8(0, 0, 0, 225));
            draw_text(badge_x + 5.0f, badge_y + 12.0f, RGBA8(255, 255, 255, 255),
                      0.44f, duration_text);
        }
    }

    /* Jerarquía tipográfica de YouTube TV: Título blanco puro, canal y vistas + tiempo de subida */
    char stats_line[96] = {0};
    char visible_stats[56] = {0};
    char ch_short[22] = {0};
    shorten_title(channel, ch_short, sizeof(ch_short));

    if (card_video) {
        format_views(card_video->views, views_text, sizeof(views_text));
        if (card_video->is_live) {
            snprintf(metadata, sizeof(metadata), "%s  *  %s espectadores", ch_short, views_text);
        } else if (card_video->published[0] != '\0') {
            snprintf(metadata, sizeof(metadata), "%s", ch_short);
            snprintf(stats_line, sizeof(stats_line), "%s vistas  *  %s", views_text, card_video->published);
        } else {
            snprintf(metadata, sizeof(metadata), "%s  *  %s vistas", ch_short, views_text);
        }
    } else {
        snprintf(metadata, sizeof(metadata), "%s", ch_short);
    }

    shorten_title(title, visible_title, sizeof(visible_title));
    float title_y = y + card_h + 17.0f;
    draw_title_with_emojis(x + 2.0f, title_y, RGBA8(255, 255, 255, 255),
                           selected ? 0.62f : 0.58f, visible_title);

    shorten_title(metadata, visible_metadata, sizeof(visible_metadata));
    float meta_y = title_y + 16.0f;
    draw_text(x + 2.0f, meta_y, RGBA8(170, 170, 170, 255),
              0.48f, visible_metadata);

    if (stats_line[0] != '\0') {
        shorten_title(stats_line, visible_stats, sizeof(visible_stats));
        draw_text(x + 2.0f, meta_y + 14.0f, RGBA8(142, 145, 155, 255),
                  0.45f, visible_stats);
    }
}

static void draw_account_view(void)
{
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(15, 15, 15, 255));

    draw_rounded_rectangle(140.0f, 54.0f, 680.0f, 436.0f, 16.0f, RGBA8(24, 24, 28, 255));
    draw_rounded_outline(140.0f, 54.0f, 680.0f, 436.0f, 16.0f, 2.0f, RGBA8(55, 55, 65, 255));

    /* Cabecera YouTube OAuth 2.0 */
    draw_rounded_rectangle(180.0f, 84.0f, 42.0f, 30.0f, 7.0f, RGBA8(255, 0, 0, 255));
    for (int p = 0; p <= 8; p++) {
        vita2d_draw_line(197.0f + (float)p, 93.0f + (float)p * 0.75f,
                         197.0f + (float)p, 105.0f - (float)p * 0.75f,
                         RGBA8(255, 255, 255, 255));
    }
    draw_text(236.0f, 108.0f, RGBA8(255, 255, 255, 255), 0.92f,
              "Cuenta de YouTube (Google OAuth 2.0)");

    if (youtube_is_logged_in()) {
        draw_text(180.0f, 165.0f, RGBA8(120, 230, 145, 255), 0.78f,
                  "Estado: Sesion activa y sincronizada con InnerTube");
        draw_text(180.0f, 205.0f, RGBA8(190, 193, 205, 255), 0.64f,
                  "Credenciales guardadas en: ux0:data/vitatube/auth.json");
        draw_text(180.0f, 240.0f, RGBA8(160, 163, 175, 255), 0.60f,
                  "Tus suscripciones y recomendaciones se cargan directamente de Google.");

        draw_rounded_rectangle(180.0f, 320.0f, 260.0f, 52.0f, 10.0f, RGBA8(220, 35, 45, 255));
        draw_text(215.0f, 354.0f, RGBA8(255, 255, 255, 255), 0.68f, "[X] Cerrar sesion");

        draw_rounded_rectangle(460.0f, 320.0f, 240.0f, 52.0f, 10.0f, RGBA8(48, 50, 60, 255));
        draw_text(500.0f, 354.0f, RGBA8(235, 238, 245, 255), 0.68f, "[TRIANGULO] Volver");
        return;
    }

    if (oauth_worker_state == 1) {
        draw_text(180.0f, 190.0f, RGBA8(235, 238, 245, 255), 0.80f,
                  "Conectando con Google OAuth 2.0...");
        draw_text(180.0f, 230.0f, RGBA8(170, 173, 183, 255), 0.64f,
                  "Generando codigo de vinculacion para TV / PS Vita...");
    } else if (oauth_worker_state == 2 && oauth_device_code.user_code[0] != '\0') {
        draw_text(180.0f, 155.0f, RGBA8(220, 223, 232, 255), 0.68f,
                  "1. En tu telefono o PC, abre el navegador y entra a:");
        draw_text(200.0f, 192.0f, RGBA8(95, 185, 255, 255), 0.86f,
                  oauth_device_code.verification_url[0]
                      ? oauth_device_code.verification_url
                      : "https://www.google.com/device");

        draw_text(180.0f, 240.0f, RGBA8(220, 223, 232, 255), 0.68f,
                  "2. Introduce el siguiente codigo de vinculacion:");

        draw_rounded_rectangle(260.0f, 258.0f, 440.0f, 68.0f, 12.0f, RGBA8(38, 40, 50, 255));
        draw_rounded_outline(260.0f, 258.0f, 440.0f, 68.0f, 12.0f, 2.0f, RGBA8(255, 255, 255, 255));
        draw_text(355.0f, 305.0f, RGBA8(255, 255, 255, 255), 1.22f,
                  oauth_device_code.user_code);

        static const char *wait_dots[] = {
            "Esperando autorizacion en tu telefono (polling cada 5s).",
            "Esperando autorizacion en tu telefono (polling cada 5s)..",
            "Esperando autorizacion en tu telefono (polling cada 5s)..."
        };
        unsigned int d = (anim_tick / 15) % 3;
        draw_text(180.0f, 368.0f, RGBA8(180, 220, 190, 255), 0.60f, wait_dots[d]);
    } else {
        draw_text(180.0f, 165.0f, RGBA8(230, 232, 240, 255), 0.72f,
                  "Inicia sesion con tu cuenta de Google usando tu telefono");
        draw_text(180.0f, 200.0f, RGBA8(165, 168, 178, 255), 0.60f,
                  "Sin escribir contrasenas en la consola (OAuth 2.0 Device Flow).");

        if (oauth_worker_state == -1) {
            draw_text(180.0f, 245.0f, RGBA8(255, 95, 95, 255), 0.64f,
                      "No se pudo completar la vinculacion. Revisa tu Wi-Fi e intenta de nuevo.");
        }

        draw_rounded_rectangle(180.0f, 295.0f, 340.0f, 54.0f, 10.0f, RGBA8(220, 30, 45, 255));
        draw_text(210.0f, 330.0f, RGBA8(255, 255, 255, 255), 0.70f,
                  "[X] Generar codigo de vinculacion");
    }

    draw_text(180.0f, 455.0f, RGBA8(145, 148, 158, 255), 0.58f,
              "X: Vincular / Accion     TRIANGULO o O: Cancelar y volver al inicio");
}

static void draw_history_icon(float cx, float cy, float r, unsigned int color)
{
    const int segs = 12;
    float start_a = 0.8f;
    float end_a = 5.6f;
    float prev_x = cx + cosf(start_a) * r;
    float prev_y = cy + sinf(start_a) * r;
    for (int s = 1; s <= segs; s++) {
        float a = start_a + (float)s * ((end_a - start_a) / (float)segs);
        float cur_x = cx + cosf(a) * r;
        float cur_y = cy + sinf(a) * r;
        vita2d_draw_line(prev_x, prev_y, cur_x, cur_y, color);
        vita2d_draw_line(prev_x + 0.5f, prev_y, cur_x + 0.5f, cur_y, color);
        prev_x = cur_x;
        prev_y = cur_y;
    }
    float ax = cx + cosf(start_a) * r;
    float ay = cy + sinf(start_a) * r;
    vita2d_draw_line(ax, ay, ax + 3.0f, ay - 3.0f, color);
    vita2d_draw_line(ax, ay, ax + 4.0f, ay + 1.0f, color);
}

static void draw_backspace_icon(float cx, float cy, unsigned int color)
{
    float lx = cx - 8.0f;
    float rx = cx + 8.0f;
    float ty = cy - 6.0f;
    float by = cy + 6.0f;

    vita2d_draw_line(lx, cy, lx + 5.0f, ty, color);
    vita2d_draw_line(lx, cy, lx + 5.0f, by, color);
    vita2d_draw_line(lx + 5.0f, ty, rx, ty, color);
    vita2d_draw_line(lx + 5.0f, by, rx, by, color);
    vita2d_draw_line(rx, ty, rx, by, color);

    vita2d_draw_line(cx - 0.5f, cy - 3.0f, cx + 4.5f, cy + 3.0f, color);
    vita2d_draw_line(cx - 0.5f, cy + 3.0f, cx + 4.5f, cy - 3.0f, color);
}

static void draw_search_view(void)
{
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(15, 15, 15, 255));

    /* 1. Barra superior: Pildora Buscar */
    draw_rounded_rectangle(34.0f, 14.0f, 280.0f, 34.0f, 17.0f, RGBA8(38, 38, 38, 255));
    if (keyboard_text[0] != 0) {
        char display_query[48];
        shorten_title(keyboard_text, display_query, sizeof(display_query));
        draw_text(52.0f, 39.0f, RGBA8(245, 245, 245, 255), 0.70f, display_query);
        if ((anim_tick / 20) % 2 == 0) {
            float cursor_x = 52.0f + (float)strlen(display_query) * 10.0f;
            if (cursor_x > 298.0f) cursor_x = 298.0f;
            vita2d_draw_rectangle(cursor_x, 21.0f, 2.0f, 20.0f, RGBA8(255, 255, 255, 255));
        }
    } else {
        draw_text(52.0f, 39.0f, RGBA8(140, 140, 140, 255), 0.70f, "Buscar");
    }

    /* 2. Columna izquierda: Sugerencias si el usuario escribe, o historial si esta vacio */
    int is_showing_suggestions = (keyboard_text[0] != 0 && live_suggestions_count > 0);
    unsigned int item_count = is_showing_suggestions ? live_suggestions_count : recent_search_count;
    if (item_count > 5) item_count = 5;

    for (unsigned int i = 0; i < item_count; ++i) {
        float hy = 58.0f + (float)i * 36.0f;
        int is_hist_focused = (search_section == SEARCH_SECTION_HISTORY && search_history_selected == i);

        if (is_hist_focused) {
            draw_rounded_rectangle(34.0f, hy, 280.0f, 32.0f, 16.0f, RGBA8(255, 255, 255, 255));
        }

        unsigned int icon_color = is_hist_focused ? RGBA8(15, 15, 15, 255) : RGBA8(160, 160, 160, 255);
        unsigned int text_color = is_hist_focused ? RGBA8(15, 15, 15, 255) : RGBA8(210, 210, 210, 255);
        unsigned int bg_color = is_hist_focused ? RGBA8(255, 255, 255, 255) : RGBA8(15, 15, 15, 255);

        if (is_showing_suggestions) {
            vita2d_draw_fill_circle(50.0f - 1.0f, hy + 16.0f - 1.0f, 4.5f, icon_color);
            vita2d_draw_fill_circle(50.0f - 1.0f, hy + 16.0f - 1.0f, 3.2f, bg_color);
            vita2d_draw_line(50.0f + 1.5f, hy + 16.0f + 1.5f, 50.0f + 5.5f, hy + 16.0f + 5.5f, icon_color);
            vita2d_draw_line(50.0f + 2.0f, hy + 16.0f + 1.5f, 50.0f + 6.0f, hy + 16.0f + 5.5f, icon_color);
        } else {
            draw_history_icon(50.0f, hy + 16.0f, 5.5f, icon_color);
        }

        const char *item_text = is_showing_suggestions ? live_suggestions[i] : recent_searches[i];
        char hist_disp[36];
        shorten_title(item_text, hist_disp, sizeof(hist_disp));
        draw_text(66.0f, hy + 22.0f, text_color, 0.58f, hist_disp);
    }

    /* 3. Teclado Leanback (8 columnas x 4 filas) */
    const char *(*keys)[8] = search_symbols_mode ? search_keys_123 : search_keys_abc;
    for (unsigned int r = 0; r < 4; ++r) {
        for (unsigned int c = 0; c < 8; ++c) {
            float kx = 340.0f + (float)c * 40.0f;
            float ky = 26.0f + (float)r * 38.0f;
            int is_key_focused = (search_section == SEARCH_SECTION_KEYBOARD &&
                                  keyboard_row == r && keyboard_column == c);
            const char *k = keys[r][c];

            if (is_key_focused) {
                draw_rounded_rectangle(kx, ky, 34.0f, 32.0f, 6.0f, RGBA8(255, 255, 255, 255));
            }

            unsigned int k_color = is_key_focused ? RGBA8(15, 15, 15, 255) : RGBA8(230, 230, 230, 255);

            if (strcmp(k, "DEL") == 0) {
                draw_backspace_icon(kx + 17.0f, ky + 16.0f, k_color);
            } else if (strcmp(k, "&123") == 0 || strcmp(k, "ABC") == 0) {
                draw_text(kx + 3.0f, ky + 21.0f, k_color, 0.50f, k);
            } else {
                draw_text(kx + 11.0f, ky + 23.0f, k_color, 0.72f, k);
            }
        }
    }

    /* 4. Fila 4: Botones de accion [ESPACIO] [BORRAR] [BUSCAR] */
    float by = 180.0f;
    float bh = 32.0f;
    float btn_x[3] = { 340.0f, 444.0f, 548.0f };
    float btn_w[3] = { 96.0f, 96.0f, 112.0f };
    const char *btn_labels[3] = { "ESPACIO", "BORRAR", "BUSCAR" };

    for (unsigned int b = 0; b < 3; ++b) {
        int is_btn_focused = (search_section == SEARCH_SECTION_KEYBOARD &&
                              keyboard_row == 4 && keyboard_column == b);

        if (is_btn_focused) {
            draw_rounded_rectangle(btn_x[b], by, btn_w[b], bh, 6.0f, RGBA8(255, 255, 255, 255));
        } else {
            draw_rounded_rectangle(btn_x[b], by, btn_w[b], bh, 6.0f, RGBA8(38, 38, 38, 255));
        }

        unsigned int b_color = is_btn_focused ? RGBA8(15, 15, 15, 255) : RGBA8(210, 210, 210, 255);
        float tx_off = (b == 0) ? 16.0f : (b == 1 ? 20.0f : 24.0f);
        draw_text(btn_x[b] + tx_off, by + 22.0f, b_color, 0.60f, btn_labels[b]);
    }

    /* 5. Seccion inferior: "Ver de nuevo" */
    draw_text(34.0f, 248.0f, RGBA8(255, 255, 255, 255), 0.80f, "Ver de nuevo");

    unsigned int hist_cnt = history_count();
    for (unsigned int c = 0; c < 3; ++c) {
        float cx = 34.0f + (float)c * 294.0f;
        float cy = 262.0f;
        float cw = 274.0f;
        float ch = 154.0f;
        int is_card_focused = (search_section == SEARCH_SECTION_CAROUSEL &&
                               search_carousel_selected == c);

        if (is_card_focused) {
            draw_rounded_outline(cx - 3.0f, cy - 3.0f, cw + 6.0f, ch + 6.0f, 12.0f, 2.5f,
                                 RGBA8(255, 255, 255, 255));
        }

        const YoutubeVideo *v = NULL;
        vita2d_texture *t = NULL;
        if (hist_cnt > c) {
            v = history_get(c);
            for (unsigned int o = 0; o < online_video_count; ++o) {
                if (strcmp(online_videos[o].id, v->id) == 0) {
                    t = online_thumbnails[o];
                    break;
                }
            }
        } else if (c < online_video_count) {
            v = &online_videos[c];
            t = online_thumbnails[c];
        }

        if (t) {
            float tw = (float)vita2d_texture_get_width(t);
            float th = (float)vita2d_texture_get_height(t);
            vita2d_draw_texture_scale(t, cx, cy,
                                      tw > 0.0f ? cw / tw : 1.0f,
                                      th > 0.0f ? ch / th : 1.0f);
        } else {
            draw_rounded_rectangle(cx, cy, cw, ch, 8.0f, RGBA8(32, 32, 32, 255));
            vita2d_draw_fill_circle(cx + cw * 0.5f, cy + ch * 0.5f, 18.0f, RGBA8(48, 48, 48, 255));
            for (int p = 0; p <= 8; ++p) {
                vita2d_draw_line(cx + cw * 0.5f - 4.0f + (float)p,
                                 cy + ch * 0.5f - 6.0f + (float)p * 0.75f,
                                 cx + cw * 0.5f - 4.0f + (float)p,
                                 cy + ch * 0.5f + 6.0f - (float)p * 0.75f,
                                 RGBA8(120, 120, 120, 255));
            }
        }

        draw_rounded_corner_mask(cx, cy, cw, ch, 8.0f, RGBA8(15, 15, 15, 255));

        if (v && v->is_live) {
            float bw = 78.0f, b_h = 20.0f;
            float bx = cx + cw - bw - 6.0f, b_y = cy + ch - b_h - 6.0f;
            draw_rounded_rectangle(bx, b_y, bw, b_h, 4.0f, RGBA8(220, 10, 35, 255));
            float icx = bx + 12.0f, icy = b_y + 10.0f;
            vita2d_draw_fill_circle(icx, icy, 1.8f, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx - 3.5f, icy - 2.5f, icx - 4.5f, icy, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx - 4.5f, icy, icx - 3.5f, icy + 2.5f, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx + 3.5f, icy - 2.5f, icx + 4.5f, icy, RGBA8(255, 255, 255, 255));
            vita2d_draw_line(icx + 4.5f, icy, icx + 3.5f, icy + 2.5f, RGBA8(255, 255, 255, 255));
            draw_text(bx + 22.0f, b_y + 14.5f, RGBA8(255, 255, 255, 255), 0.52f, "EN VIVO");
        } else if (v && v->duration > 0 && v->duration < 86400) {
            char dur[16] = {0};
            format_duration(v->duration, dur, sizeof(dur));
            float tw = (float)vita2d_pgf_text_width(font, 0.44f, dur);
            float bw = tw + 10.0f;
            float b_h = 16.0f;
            float bx = cx + cw - bw - 6.0f, b_y = cy + ch - b_h - 6.0f;
            draw_rounded_rectangle(bx, b_y, bw, b_h, 3.5f, RGBA8(0, 0, 0, 225));
            draw_text(bx + 5.0f, b_y + 12.0f, RGBA8(255, 255, 255, 255), 0.44f, dur);
        }

        const char *raw_title = v ? v->title : video_titles[0][c];
        const char *raw_chan = (v && v->channel[0]) ? v->channel : "VitaTube";
        char card_title[40];
        char card_chan[32];
        shorten_title(raw_title, card_title, sizeof(card_title));
        shorten_title(raw_chan, card_chan, sizeof(card_chan));
        draw_title_with_emojis(cx, cy + ch + 18.0f, RGBA8(255, 255, 255, 255), 0.62f, card_title);
        draw_text(cx, cy + ch + 36.0f, RGBA8(160, 160, 160, 255), 0.50f, card_chan);
    }

    /* Borde sutil de la 4ta tarjeta asomandose a la derecha */
    draw_rounded_rectangle(916.0f, 262.0f, 44.0f, 154.0f, 8.0f, RGBA8(28, 28, 28, 255));
    draw_rounded_corner_mask(916.0f, 262.0f, 44.0f, 154.0f, 8.0f, RGBA8(15, 15, 15, 255));

    /* 6. Barra inferior con atajos de botones */
    draw_text(34.0f, 526.0f, RGBA8(120, 120, 120, 255), 0.48f,
              "Cruceta: Navegar   X: Seleccionar   CUADRADO: Borrar   START: Buscar   TRIANGULO: Volver");
}

static void switch_to_sidebar_view(unsigned int new_view)
{
    unsigned int prev_view = active_view;
    if (prev_view == 1 && new_view != 1 && !player_active) {
        video_player_close();
        shorts_playing_index = -1;
        shorts_show_comments = 0;
        shorts_phone_mode = 0;
    }

    active_view = new_view;
    selected_sidebar = new_view;
    search_results_active = 0;
    search_label[0] = 0;
    sidebar_active = 0;
    content_offset = 0;
    selected_row = 0;
    selected_column = 0;

    if (new_view == 0 || new_view == 1 || new_view == 2) {
        if (loaded_feed_view != (int)new_view || online_video_count == 0) {
            stop_startup_worker();
            clear_thumbnails(online_thumbnails);
            if (new_view == 0 || new_view == 2) {
                clear_thumbnails(home_shorts_thumbnails);
                home_shorts_count = 0;
                home_shorts_thumbs_done = 0;
            }
            online_video_count = 0;
            shorts_index = 0;
            shorts_playing_index = -1;
            shorts_show_comments = 0;
            status_message = (new_view == 1) ? "Cargando Shorts..."
                           : (new_view == 2) ? "Cargando Tendencias..."
                           : "Cargando Inicio...";
            start_startup_request();
        } else if (new_view == 1 && online_video_count > 0) {
            status_message = "Reproduciendo Shorts";
            if (shorts_playing_index != (int)shorts_index) {
                start_short_playback(shorts_index < online_video_count ? shorts_index : 0);
            }
        } else {
            status_message = "Selecciona un video con X";
        }
    } else if (new_view == 3) {
        if (youtube_has_subscriptions()) {
            if (subs_video_count == 0 && subs_worker_state != 1) {
                status_message = "Cargando suscripciones...";
                start_subs_request();
            } else {
                status_message = "Suscripciones";
            }
        } else {
            status_message = "Suscribete a un canal o inicia sesion";
        }
    } else if (new_view == 4) {
        start_history_thumbnails();
        status_message = "Biblioteca y videos vistos";
    } else if (new_view == 5) {
        status_message = "Configuracion";
    }
}

void home_init(void)
{
    log_event("home_init");
    font = vita2d_load_default_pgf();
    selected_row = 0;
    selected_column = 0;
    content_offset = 0;
    selected_sidebar = 0;
    active_view = 0;
    player_active = 0;
    search_results_active = 0;
    sidebar_active = 0;
    status_message = "Selecciona un video con X";
    keyboard_active = 0;
    keyboard_text[0] = 0;
    search_label[0] = 0;
    strncpy(account_username, youtube_username(), sizeof(account_username) - 1);
    account_username[sizeof(account_username) - 1] = 0;
    account_password[0] = 0;
    account_view = 0;
    account_confirm_password[0] = 0;
    account_field = 0;
    account_status = youtube_is_logged_in()
        ? "Sesion guardada en esta consola"
        : "Inicia sesion con Google";
    online_video_count = 0;
    search_video_count = 0;
    subs_video_count = 0;
    subs_worker_state = 0;
    loaded_feed_view = -1;
    shorts_index = 0;
    shorts_playing_index = -1;
    shorts_focus_btn = SHORTS_BTN_LIKE;
    shorts_show_comments = 0;
    shorts_phone_mode = 0;
    shorts_phone_clockwise = 0;
    s_phone_ui_dirty = 1;
    home_shorts_count = 0;
    home_shorts_thumbs_done = 0;
    anim_tick = 0;
    thumb_nav_cooldown = 0;
    thumb_decoded_this_frame = 0;
    previous_back_touch_y = -1;
    memset(online_thumbnails, 0, sizeof(online_thumbnails));
    memset(home_shorts_thumbnails, 0, sizeof(home_shorts_thumbnails));
    memset(search_thumbnails, 0, sizeof(search_thumbnails));
    memset(subs_thumbnails, 0, sizeof(subs_thumbnails));
    memset(history_thumbnails, 0, sizeof(history_thumbnails));
    memset(history_thumb_ids, 0, sizeof(history_thumb_ids));
    load_recent_searches();
    start_startup_request();
    if (suggestions_thread < 0) {
        suggestions_worker_state = 0;
        suggestions_thread = sceKernelCreateThread("VitaTubeSuggestions", suggestions_worker,
                                                  0x10000100, 64 * 1024, 0, 0, NULL);
        if (suggestions_thread >= 0) {
            sceKernelStartThread(suggestions_thread, 0, NULL);
        }
    }
}

void home_update(unsigned int pressed, const SceTouchData *touch, const SceTouchData *touch_back)
{
    ++anim_tick;
    thumb_decoded_this_frame = 0;
    int back_touch_enabled = !(active_view == 1 && !search_results_active && shorts_phone_mode);
    if (pressed != 0 || (touch && touch->reportNum > 0) || (back_touch_enabled && touch_back && touch_back->reportNum > 0)) {
        thumb_nav_cooldown = 18;
    } else if (thumb_nav_cooldown > 0) {
        thumb_nav_cooldown--;
    }
    update_startup_loading();
    update_search_loading();
    update_subs_loading();
    update_history_loading();
    update_player();
    video_player_update();
    if (player_active || (active_view == 1 && !search_results_active)) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
    }
    if (player_overlay_timer > 0) player_overlay_timer--;

    if (keyboard_active) {
        if (keyboard_target == 0) {
            /* Actualizar sugerencias automaticas de Piped */
            if (keyboard_text[0] == 0) {
                live_suggestions_count = 0;
                suggestions_last_fetched[0] = 0;
                suggestions_query[0] = 0;
            } else if (strcmp(keyboard_text, suggestions_last_fetched) != 0 && suggestions_worker_state == 0) {
                strncpy(suggestions_query, keyboard_text, sizeof(suggestions_query) - 1);
                suggestions_query[sizeof(suggestions_query) - 1] = 0;
                suggestions_worker_state = 1;
            }

            /* Navegacion estilo YouTube TV para la pantalla de busqueda */
            if (pressed & SCE_CTRL_TRIANGLE) {
                audio_play_cancel();
                keyboard_active = 0;
                status_message = "Busqueda cancelada";
                return;
            }

            if (pressed & SCE_CTRL_START) {
                if (keyboard_text[0] != 0) {
                    audio_play_select();
                    keyboard_confirm();
                }
                return;
            }

            if (pressed & SCE_CTRL_SQUARE) {
                keyboard_backspace();
                audio_play_nav();
                return;
            }

            if (search_section == SEARCH_SECTION_KEYBOARD) {
                if (pressed & SCE_CTRL_LEFT) {
                    audio_play_nav();
                    if (keyboard_column > 0) {
                        keyboard_column--;
                    } else {
                        search_section = SEARCH_SECTION_HISTORY;
                        int is_showing_sugg = (keyboard_text[0] != 0 && live_suggestions_count > 0);
                        unsigned int cnt = is_showing_sugg ? live_suggestions_count : recent_search_count;
                        if (cnt > 5) cnt = 5;
                        search_history_selected = keyboard_row < cnt ? keyboard_row : (cnt > 0 ? cnt - 1 : 0);
                    }
                } else if (pressed & SCE_CTRL_RIGHT) {
                    audio_play_nav();
                    unsigned int max_col = (keyboard_row == 4) ? 2 : 7;
                    if (keyboard_column < max_col) {
                        keyboard_column++;
                    }
                } else if (pressed & SCE_CTRL_UP) {
                    audio_play_nav();
                    if (keyboard_row == 4) {
                        keyboard_row = 3;
                        keyboard_column = (keyboard_column == 0) ? 1 : (keyboard_column == 1 ? 4 : 6);
                    } else if (keyboard_row > 0) {
                        keyboard_row--;
                    }
                } else if (pressed & SCE_CTRL_DOWN) {
                    audio_play_nav();
                    if (keyboard_row < 3) {
                        keyboard_row++;
                    } else if (keyboard_row == 3) {
                        keyboard_row = 4;
                        if (keyboard_column <= 2) keyboard_column = 0;
                        else if (keyboard_column <= 4) keyboard_column = 1;
                        else keyboard_column = 2;
                    } else if (keyboard_row == 4) {
                        search_section = SEARCH_SECTION_CAROUSEL;
                        search_carousel_selected = keyboard_column < 3 ? keyboard_column : 2;
                    }
                } else if (pressed & SCE_CTRL_CROSS) {
                    audio_play_select();
                    if (keyboard_row < 4) {
                        const char *(*keys)[8] = search_symbols_mode ? search_keys_123 : search_keys_abc;
                        const char *k = keys[keyboard_row][keyboard_column];
                        if (strcmp(k, "DEL") == 0) {
                            keyboard_backspace();
                        } else if (strcmp(k, "&123") == 0) {
                            search_symbols_mode = 1;
                        } else if (strcmp(k, "ABC") == 0) {
                            search_symbols_mode = 0;
                        } else {
                            size_t cur_len = strlen(keyboard_text);
                            if (cur_len + strlen(k) < sizeof(keyboard_text)) {
                                strcat(keyboard_text, k);
                            }
                        }
                    } else if (keyboard_row == 4) {
                        if (keyboard_column == 0) {
                            size_t cur_len = strlen(keyboard_text);
                            if (cur_len + 1 < sizeof(keyboard_text)) {
                                keyboard_text[cur_len] = ' ';
                                keyboard_text[cur_len + 1] = 0;
                            }
                        } else if (keyboard_column == 1) {
                            keyboard_backspace();
                        } else if (keyboard_column == 2) {
                            if (keyboard_text[0] != 0) {
                                keyboard_confirm();
                            }
                        }
                    }
                }
            } else if (search_section == SEARCH_SECTION_HISTORY) {
                int is_showing_sugg = (keyboard_text[0] != 0 && live_suggestions_count > 0);
                unsigned int cnt = is_showing_sugg ? live_suggestions_count : recent_search_count;
                if (cnt > 5) cnt = 5;

                if (pressed & SCE_CTRL_UP) {
                    audio_play_nav();
                    if (search_history_selected > 0) {
                        search_history_selected--;
                    }
                } else if (pressed & SCE_CTRL_DOWN) {
                    audio_play_nav();
                    if (search_history_selected + 1 < cnt) {
                        search_history_selected++;
                    } else {
                        search_section = SEARCH_SECTION_CAROUSEL;
                        search_carousel_selected = 0;
                    }
                } else if (pressed & SCE_CTRL_RIGHT) {
                    audio_play_nav();
                    search_section = SEARCH_SECTION_KEYBOARD;
                    keyboard_row = search_history_selected < 4 ? search_history_selected : 3;
                    keyboard_column = 0;
                } else if (pressed & SCE_CTRL_CROSS) {
                    if (cnt > 0 && search_history_selected < cnt) {
                        audio_play_select();
                        const char *picked = is_showing_sugg
                            ? live_suggestions[search_history_selected]
                            : recent_searches[search_history_selected];
                        strncpy(keyboard_text, picked, sizeof(keyboard_text) - 1);
                        keyboard_text[sizeof(keyboard_text) - 1] = 0;
                        keyboard_confirm();
                    }
                }
            } else if (search_section == SEARCH_SECTION_CAROUSEL) {
                if (pressed & SCE_CTRL_LEFT) {
                    audio_play_nav();
                    if (search_carousel_selected > 0) {
                        search_carousel_selected--;
                    }
                } else if (pressed & SCE_CTRL_RIGHT) {
                    audio_play_nav();
                    if (search_carousel_selected < 2) {
                        search_carousel_selected++;
                    }
                } else if (pressed & SCE_CTRL_UP) {
                    audio_play_nav();
                    search_section = SEARCH_SECTION_KEYBOARD;
                    keyboard_row = 4;
                    keyboard_column = search_carousel_selected;
                } else if (pressed & SCE_CTRL_CROSS) {
                    audio_play_select();
                    const YoutubeVideo *target_vid = NULL;
                    if (history_count() > search_carousel_selected) {
                        target_vid = history_get(search_carousel_selected);
                    } else if (search_carousel_selected < online_video_count) {
                        target_vid = &online_videos[search_carousel_selected];
                    }
                    if (target_vid) {
                        keyboard_active = 0;
                        open_player_for_video(target_vid);
                        status_message = "Consultando streams...";
                    }
                }
            }

            /* Soporte tactil en busqueda */
            if (touch && touch->reportNum > 0 && !keyboard_touch_latched) {
                int tx = touch->report[0].x / 2;
                int ty = touch->report[0].y / 2;
                keyboard_touch_latched = 1;

                if (tx >= 34 && tx <= 314 && ty >= 58 && ty <= 238) {
                    unsigned int h_idx = (unsigned int)((ty - 58) / 36);
                    int is_showing_sugg = (keyboard_text[0] != 0 && live_suggestions_count > 0);
                    unsigned int cnt = is_showing_sugg ? live_suggestions_count : recent_search_count;
                    if (cnt > 5) cnt = 5;

                    if (h_idx < cnt) {
                        search_section = SEARCH_SECTION_HISTORY;
                        search_history_selected = h_idx;
                        const char *picked = is_showing_sugg
                            ? live_suggestions[h_idx]
                            : recent_searches[h_idx];
                        strncpy(keyboard_text, picked, sizeof(keyboard_text) - 1);
                        keyboard_text[sizeof(keyboard_text) - 1] = 0;
                        keyboard_confirm();
                        return;
                    }
                }

                if (tx >= 340 && tx < 660 && ty >= 26 && ty < 178) {
                    unsigned int r = (unsigned int)((ty - 26) / 38);
                    unsigned int c = (unsigned int)((tx - 340) / 40);
                    if (r < 4 && c < 8) {
                        search_section = SEARCH_SECTION_KEYBOARD;
                        keyboard_row = r;
                        keyboard_column = c;
                        const char *(*keys)[8] = search_symbols_mode ? search_keys_123 : search_keys_abc;
                        const char *k = keys[r][c];
                        if (strcmp(k, "DEL") == 0) {
                            keyboard_backspace();
                        } else if (strcmp(k, "&123") == 0) {
                            search_symbols_mode = 1;
                        } else if (strcmp(k, "ABC") == 0) {
                            search_symbols_mode = 0;
                        } else {
                            size_t cur_len = strlen(keyboard_text);
                            if (cur_len + strlen(k) < sizeof(keyboard_text)) {
                                strcat(keyboard_text, k);
                            }
                        }
                        return;
                    }
                }

                if (tx >= 340 && tx <= 660 && ty >= 180 && ty <= 212) {
                    search_section = SEARCH_SECTION_KEYBOARD;
                    keyboard_row = 4;
                    if (tx < 440) {
                        keyboard_column = 0;
                        size_t cur_len = strlen(keyboard_text);
                        if (cur_len + 1 < sizeof(keyboard_text)) {
                            keyboard_text[cur_len] = ' ';
                            keyboard_text[cur_len + 1] = 0;
                        }
                    } else if (tx < 544) {
                        keyboard_column = 1;
                        keyboard_backspace();
                    } else {
                        keyboard_column = 2;
                        if (keyboard_text[0] != 0) {
                            keyboard_confirm();
                        }
                    }
                    return;
                }

                if (ty >= 262 && ty <= 440) {
                    for (int c = 0; c < 3; c++) {
                        float cx = 34.0f + (float)c * 294.0f;
                        if (tx >= cx && tx <= cx + 274.0f) {
                            search_section = SEARCH_SECTION_CAROUSEL;
                            search_carousel_selected = (unsigned int)c;
                            const YoutubeVideo *target_vid = NULL;
                            if (history_count() > (unsigned int)c) {
                                target_vid = history_get((unsigned int)c);
                            } else if ((unsigned int)c < online_video_count) {
                                target_vid = &online_videos[(unsigned int)c];
                            }
                            if (target_vid) {
                                keyboard_active = 0;
                                open_player_for_video(target_vid);
                                status_message = "Consultando streams...";
                            }
                            return;
                        }
                    }
                }
            } else if (touch && touch->reportNum == 0) {
                keyboard_touch_latched = 0;
            }
            return;
        }

        if (pressed & SCE_CTRL_LEFT) {
            keyboard_column = keyboard_column == 0 ? 9 : keyboard_column - 1;
        } else if (pressed & SCE_CTRL_RIGHT) {
            keyboard_column = (keyboard_column + 1) % 10;
        } else if (pressed & SCE_CTRL_UP) {
            keyboard_row = keyboard_row == 0 ? 4 : keyboard_row - 1;
        } else if (pressed & SCE_CTRL_DOWN) {
            keyboard_row = (keyboard_row + 1) % 5;
        } else if (pressed & SCE_CTRL_CROSS) {
            keyboard_press();
        } else if (pressed & SCE_CTRL_TRIANGLE) {
            keyboard_active = 0;
            status_message = "Busqueda cancelada";
        }

        if (touch && touch->reportNum == 0) {
            keyboard_touch_latched = 0;
        } else if (touch && touch->reportNum > 0 &&
                   !keyboard_touch_latched) {
            int x = touch->report[0].x / 2;
            int y = touch->report[0].y / 2;
            int start;
            keyboard_touch_latched = 1;
            if (y >= 325 && y < 464) {
                keyboard_row = (unsigned int)((y - 325) / 36);
                start = keyboard_row == 0 ? 105 : 70;
                if (x < start) {
                    return;
                }

                keyboard_column = (unsigned int)((x - start) /
                    (keyboard_row == 0 ? 75 : 82));
                if (keyboard_row < 4) {
                    unsigned int row_length =
                        (unsigned int)strlen(keyboard_rows[keyboard_row]);
                    if (keyboard_column < row_length) {
                        keyboard_press();
                    }
                }
            } else if (y >= 470 && y < 530) {
                if (x < 220) {
                    keyboard_row = 4; keyboard_column = 0; keyboard_press();
                } else if (x < 410) {
                    keyboard_row = 4; keyboard_column = 1; keyboard_press();
                } else if (x < 600) {
                    keyboard_row = 4; keyboard_column = 2; keyboard_press();
                } else if (x < 790) {
                    keyboard_row = 4; keyboard_column = 3; keyboard_press();
                } else {
                    keyboard_active = 0;
                }
            }
        }
        return;
    }

    if (account_view) {
        if (oauth_worker_state == 3) {
            oauth_worker_state = 0;
            account_view = 0;
            status_message = "Cuenta de Google vinculada con exito";
            clear_thumbnails(online_thumbnails);
            online_video_count = 0;
            subs_video_count = 0;
            subs_worker_state = 0;
            start_startup_request();
            if (active_view == 3) {
                start_subs_request();
            }
            return;
        }

        if (pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE)) {
            stop_oauth_worker();
            oauth_worker_state = 0;
            account_view = 0;
            audio_play_cancel();
            return;
        }

        if (pressed & SCE_CTRL_CROSS) {
            audio_play_select();
            if (youtube_is_logged_in()) {
                youtube_logout();
                subs_video_count = 0;
                subs_worker_state = 0;
                status_message = "Sesion cerrada";
            } else if (oauth_worker_state != 1 && oauth_worker_state != 2) {
                start_oauth_flow();
            }
            return;
        }
        return;
    }

    if (pressed & SCE_CTRL_SELECT) {
        if (active_view == 1 && !search_results_active && !sidebar_active && shorts_phone_mode) {
            shorts_phone_clockwise = !shorts_phone_clockwise;
            s_phone_ui_dirty = 1;
            audio_play_nav();
            status_message = "Orientacion vertical invertida";
            return;
        }
        log_event("search_open_source=select");
        open_search_dialog();
        return;
    }

    if (player_active) {
        /* Si ocurrio un error al cargar el video/directo, permitir Reintentar o Volver */
        if (player_error_msg[0] != '\0' || video_player_get_status() == VPLAYER_ERROR) {
            player_show_settings = 0;
            player_show_desc = 0;
            if (pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE)) {
                video_player_close();
                player_active = 0;
                player_error_msg[0] = '\0';
                status_message = "Video detenido";
                audio_play_cancel();
                return;
            }
            if (pressed & SCE_CTRL_CROSS) {
                audio_play_select();
                player_switch_quality(player_quality);
                return;
            }
            if (touch && touch->reportNum > 0 && previous_touch_x < 0) {
                int tx = touch->report[0].x / 2;
                int ty = touch->report[0].y / 2;
                if (ty >= 235 && ty <= 280) {
                    if (tx >= 260 && tx <= 460) {
                        audio_play_select();
                        player_switch_quality(player_quality);
                    } else if (tx >= 500 && tx <= 700) {
                        video_player_close();
                        player_active = 0;
                        player_error_msg[0] = '\0';
                        status_message = "Video detenido";
                        audio_play_cancel();
                    }
                }
            }
            return;
        }

        /* Descartar overlay de configuracion si esta abierto */
        if (player_show_settings) {
            if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE)) {
                player_show_settings = 0;
                player_overlay_timer = 240;
                audio_play_cancel();
                return;
            }
            if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
                player_settings_selected = !player_settings_selected;
                audio_play_nav();
                player_overlay_timer = 240;
                return;
            }
            if (player_settings_selected == 0) {
                /* Calidad */
                if (pressed & SCE_CTRL_LEFT) {
                    unsigned int cur = youtube_max_quality();
                    if (cur == 720) cur = 480;
                    else if (cur == 480) cur = 360;
                    else if (cur == 360) cur = 240;
                    else cur = 720;
                    youtube_set_max_quality(cur);
                    player_switch_quality(cur);
                    audio_play_select();
                    player_overlay_timer = 240;
                    return;
                } else if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_RIGHT)) {
                    unsigned int cur = youtube_max_quality();
                    if (cur == 240) cur = 360;
                    else if (cur == 360) cur = 480;
                    else if (cur == 480) cur = 720;
                    else cur = 240;
                    youtube_set_max_quality(cur);
                    player_switch_quality(cur);
                    audio_play_select();
                    player_overlay_timer = 240;
                    return;
                }
            } else if (player_settings_selected == 1) {
                /* Tiempo de reproduccion */
                if (pressed & SCE_CTRL_LEFT) {
                    video_player_seek(-30);
                    audio_play_nav();
                    player_overlay_timer = 240;
                    return;
                } else if (pressed & SCE_CTRL_RIGHT) {
                    video_player_seek(30);
                    audio_play_nav();
                    player_overlay_timer = 240;
                    return;
                } else if (pressed & SCE_CTRL_CROSS) {
                    video_player_seek_to(0);
                    audio_play_select();
                    player_overlay_timer = 240;
                    return;
                }
            }
            return;
        }

        /* Descartar overlay de descripcion con Circulo, Triangulo o Cruz si esta abierto */
        if (player_show_desc) {
            if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE | SCE_CTRL_CROSS)) {
                player_show_desc = 0;
                player_overlay_timer = 240;
                audio_play_cancel();
                return;
            }
            return;
        }

        /* Navegar y descartar overlay de comentarios en el reproductor de video */
        if (player_show_comments) {
            if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE | SCE_CTRL_CROSS | SCE_CTRL_SQUARE)) {
                player_show_comments = 0;
                player_overlay_timer = 240;
                audio_play_cancel();
                return;
            }
            if ((pressed & SCE_CTRL_DOWN) && shorts_comment_scroll + 1 < shorts_comment_count) {
                shorts_comment_scroll++;
                audio_play_nav();
            } else if ((pressed & SCE_CTRL_UP) && shorts_comment_scroll > 0) {
                shorts_comment_scroll--;
                audio_play_nav();
            }
            player_overlay_timer = 240;
            return;
        }

        /* Salir del reproductor con Triangulo o Circulo */
        if (pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE)) {
            video_player_close();
            player_active = 0;
            player_error_msg[0] = '\0';
            status_message = "Video detenido";
            audio_play_cancel();
            return;
        }

        /* Toggle OSD manual con Cuadrado */
        if (pressed & SCE_CTRL_SQUARE) {
            if (player_overlay_timer > 0) player_overlay_timer = 0;
            else player_overlay_timer = 240;
            return;
        }

        /* Seek rapido con L / R Triggers */
        if (pressed & SCE_CTRL_LTRIGGER) {
            video_player_seek(-10);
            player_overlay_timer = 240;
        } else if (pressed & SCE_CTRL_RTRIGGER) {
            video_player_seek(10);
            player_overlay_timer = 240;
        }

        /* Si el overlay esta apagado, cualquier boton direccional o Cruz lo despierta primero */
        if (player_overlay_timer == 0 && (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS))) {
            player_overlay_timer = 240;
            return;
        }

        /* Navegacion por secciones */
        if (player_focus_section == PLAYER_FOCUS_SEEKBAR) {
            if (pressed & SCE_CTRL_LEFT) {
                video_player_seek(-10);
                audio_play_nav();
                player_overlay_timer = 240;
            } else if (pressed & SCE_CTRL_RIGHT) {
                video_player_seek(10);
                audio_play_nav();
                player_overlay_timer = 240;
            } else if (pressed & SCE_CTRL_DOWN) {
                player_focus_section = PLAYER_FOCUS_CONTROLS;
                audio_play_nav();
                player_overlay_timer = 240;
            } else if (pressed & SCE_CTRL_UP) {
                player_overlay_timer = 0;
            } else if (pressed & SCE_CTRL_CROSS) {
                video_player_toggle_pause();
                audio_play_select();
                player_overlay_timer = 240;
            }
        } else if (player_focus_section == PLAYER_FOCUS_CONTROLS) {
            if (pressed & SCE_CTRL_LEFT) {
                if (player_focus_btn > 0) {
                    player_focus_btn--;
                    audio_play_nav();
                }
                player_overlay_timer = 240;
            } else if (pressed & SCE_CTRL_RIGHT) {
                if (player_focus_btn < PLAYER_BTN_COUNT - 1) {
                    player_focus_btn++;
                    audio_play_nav();
                }
                player_overlay_timer = 240;
            } else if (pressed & SCE_CTRL_UP) {
                player_focus_section = PLAYER_FOCUS_SEEKBAR;
                audio_play_nav();
                player_overlay_timer = 240;
            } else if (pressed & SCE_CTRL_DOWN) {
                if (player_related_count > 0) {
                    player_focus_section = PLAYER_FOCUS_CAROUSEL;
                    audio_play_nav();
                    player_overlay_timer = 240;
                }
            } else if (pressed & SCE_CTRL_CROSS) {
                audio_play_select();
                player_overlay_timer = 240;
                switch (player_focus_btn) {
                    case PLAYER_BTN_AVATAR:
                    case PLAYER_BTN_DESC:
                        player_show_desc = !player_show_desc;
                        player_show_comments = 0;
                        player_show_settings = 0;
                        break;
                    case PLAYER_BTN_SUBS:
                        player_is_subscribed = youtube_toggle_channel_subscription(player_video_author);
                        subs_video_count = 0;
                        subs_worker_state = 0;
                        break;
                    case PLAYER_BTN_PREV: {
                        uint64_t cur_sec = video_player_get_current_time() / 1000;
                        if (cur_sec > 3) {
                            video_player_seek(-(int64_t)cur_sec);
                        } else if (history_count() > 1) {
                            const YoutubeVideo *prev = history_get(1);
                            if (prev) {
                                YoutubeVideo v = *prev;
                                open_player_for_video(&v);
                            }
                        }
                        break;
                    }
                    case PLAYER_BTN_PLAY:
                        video_player_toggle_pause();
                        break;
                    case PLAYER_BTN_NEXT: {
                        if (player_related_count > 0) {
                            YoutubeVideo next_v = player_related_videos[0];
                            open_player_for_video(&next_v);
                        }
                        break;
                    }
                    case PLAYER_BTN_LIKE:
                        player_like_state = (player_like_state == 1) ? 0 : 1;
                        youtube_set_video_rating(player_video_id, player_like_state);
                        break;
                    case PLAYER_BTN_DISLIKE:
                        player_like_state = (player_like_state == -1) ? 0 : -1;
                        youtube_set_video_rating(player_video_id, player_like_state);
                        break;
                    case PLAYER_BTN_COMMENTS:
                        player_show_comments = !player_show_comments;
                        player_show_desc = 0;
                        player_show_settings = 0;
                        shorts_comment_scroll = 0;
                        if (player_show_comments && shorts_comment_count == 0 && shorts_meta_state != 1) {
                            start_shorts_meta_request(player_video_id);
                        }
                        break;
                    case PLAYER_BTN_SAVE:
                        if (player_related_count > 0) {
                            history_add(&player_related_videos[0]);
                        }
                        break;
                    case PLAYER_BTN_THANKS:
                        player_like_state = 1;
                        youtube_set_video_rating(player_video_id, 1);
                        break;
                    case PLAYER_BTN_SETTINGS:
                        player_show_settings = !player_show_settings;
                        player_show_desc = 0;
                        player_show_comments = 0;
                        player_settings_selected = 0;
                        break;
                    default:
                        break;
                }
            }
        } else if (player_focus_section == PLAYER_FOCUS_CAROUSEL) {
            if (pressed & SCE_CTRL_LEFT) {
                if (player_carousel_idx > 0) {
                    player_carousel_idx--;
                    audio_play_nav();
                }
                player_overlay_timer = 240;
            } else if (pressed & SCE_CTRL_RIGHT) {
                if (player_carousel_idx < player_related_count - 1) {
                    player_carousel_idx++;
                    audio_play_nav();
                }
                player_overlay_timer = 240;
            } else if (pressed & SCE_CTRL_UP) {
                player_focus_section = PLAYER_FOCUS_CONTROLS;
                audio_play_nav();
                player_overlay_timer = 240;
            } else if (pressed & SCE_CTRL_CROSS) {
                if (player_carousel_idx < player_related_count) {
                    audio_play_select();
                    YoutubeVideo selected = player_related_videos[player_carousel_idx];
                    open_player_for_video(&selected);
                }
            }
        }

        /* Soporte Touch en el reproductor */
        if (touch && touch->reportNum > 0) {
            if (previous_touch_x < 0) {
                player_overlay_timer = 240;
                int touch_x = touch->report[0].x / 2;
                int touch_y = touch->report[0].y / 2;

                /* Tocar centro de video: Play/Pause */
                if (touch_y < 330) {
                    video_player_toggle_pause();
                } else if (touch_y >= 335 && touch_y <= 360 && touch_x >= 34 && touch_x <= 926) {
                    /* Barra de progreso */
                    float p = (float)(touch_x - 34) / 892.0f;
                    if (p < 0.0f) p = 0.0f;
                    if (p > 1.0f) p = 1.0f;
                    uint64_t dur_ms = video_player_get_duration();
                    unsigned int total_sec = (player_video_duration > 0) ? player_video_duration : ((dur_ms > 0) ? (unsigned int)(dur_ms / 1000) : 0);
                    if (total_sec > 0 && !player_video_is_live) {
                        video_player_seek_to((uint64_t)(p * total_sec * 1000.0f));
                    }
                } else if (touch_y >= 370 && touch_y <= 415) {
                    /* Boton de Play/Pause */
                    if (touch_x >= 455 && touch_x <= 515) {
                        video_player_toggle_pause();
                    } else if (touch_x >= 415 && touch_x <= 455) {
                        video_player_seek(-10);
                    } else if (touch_x >= 515 && touch_x <= 555) {
                        if (player_related_count > 0) {
                            YoutubeVideo next_v = player_related_videos[0];
                            open_player_for_video(&next_v);
                        }
                    } else if (touch_x >= 34 && touch_x <= 170) {
                        player_show_desc = !player_show_desc;
                        player_show_comments = 0;
                        player_show_settings = 0;
                    } else if (touch_x >= 178 && touch_x <= 276) {
                        player_is_subscribed = youtube_toggle_channel_subscription(player_video_author);
                        subs_video_count = 0;
                        subs_worker_state = 0;
                    } else if (touch_x >= 660 && touch_x <= 702) {
                        player_like_state = (player_like_state == 1) ? 0 : 1;
                        youtube_set_video_rating(player_video_id, player_like_state);
                    } else if (touch_x >= 704 && touch_x <= 746) {
                        player_like_state = (player_like_state == -1) ? 0 : -1;
                        youtube_set_video_rating(player_video_id, player_like_state);
                    } else if (touch_x >= 748 && touch_x <= 790) {
                        player_show_comments = !player_show_comments;
                        player_show_desc = 0;
                        player_show_settings = 0;
                        shorts_comment_scroll = 0;
                        if (player_show_comments && shorts_comment_count == 0 && shorts_meta_state != 1) {
                            start_shorts_meta_request(player_video_id);
                        }
                    }
                } else if (touch_y >= 435 && touch_y <= 526) {
                    /* Tocar tarjeta de video relacionado */
                    for (unsigned int c = 0; c < player_related_count; ++c) {
                        float cx = 34.0f + (float)c * 170.0f - player_carousel_scroll;
                        if (touch_x >= cx && touch_x <= cx + 156.0f) {
                            YoutubeVideo selected = player_related_videos[c];
                            open_player_for_video(&selected);
                            break;
                        }
                    }
                }
            }
            previous_touch_x = touch->report[0].x;
            previous_touch_y = touch->report[0].y;
        } else {
            previous_touch_x = -1;
            previous_touch_y = -1;
        }
        return;
    }

    /* =========================================================================
     * MODO SHORTS VERTICAL (active_view == 1 && !search_results_active && !sidebar_active)
     * ========================================================================= */
    if (active_view == 1 && !search_results_active && !sidebar_active) {
        /* Combinacion L + R o boton START: Alternar Modo Telefono Vertical (90 grados) */
        SceCtrlData pad_now;
        memset(&pad_now, 0, sizeof(pad_now));
        sceCtrlPeekBufferPositive(0, &pad_now, 1);
        int lr_combo = ((pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) != 0) &&
                       ((pad_now.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) ==
                        (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER));
        if ((pressed & SCE_CTRL_START) || lr_combo) {
            shorts_phone_mode = !shorts_phone_mode;
            s_phone_ui_dirty = 1;
            audio_play_select();
            status_message = shorts_phone_mode
                ? "Modo Telefono Vertical (START o L+R: Salir | SELECT: Girar)"
                : "Vista estandar de Shorts";
            return;
        }

        /* Repetir o pasar al siguiente cuando el Short termina */
        if (video_player_get_status() == VPLAYER_COMPLETED) {
            if (youtube_autoplay() && online_video_count > 0) {
                unsigned int next_idx = (shorts_index + 1 < online_video_count) ? (shorts_index + 1) : 0;
                start_short_playback(next_idx);
            } else {
                video_player_seek_to(0);
            }
        }

        if (shorts_show_comments) {
            if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE | SCE_CTRL_CROSS | SCE_CTRL_SQUARE)) {
                shorts_show_comments = 0;
                s_phone_ui_dirty = 1;
                audio_play_cancel();
            } else if (pressed & (SCE_CTRL_UP | SCE_CTRL_LEFT)) {
                if (shorts_comment_scroll > 0) {
                    shorts_comment_scroll--;
                    s_phone_ui_dirty = 1;
                    audio_play_nav();
                }
            } else if (pressed & (SCE_CTRL_DOWN | SCE_CTRL_RIGHT)) {
                if (shorts_comment_scroll + 3 < shorts_comment_count) {
                    shorts_comment_scroll++;
                    s_phone_ui_dirty = 1;
                    audio_play_nav();
                }
            }

            if (touch && touch->reportNum > 0 && previous_touch_x < 0) {
                int tx = touch->report[0].x / 2;
                int ty = touch->report[0].y / 2;
                if (shorts_phone_mode || tx < 625 || (tx >= 890 && ty <= 70)) {
                    shorts_show_comments = 0;
                    s_phone_ui_dirty = 1;
                    audio_play_cancel();
                }
                previous_touch_x = tx;
                previous_touch_y = ty;
            } else if (!touch || touch->reportNum == 0) {
                previous_touch_x = -1;
                previous_touch_y = -1;
            }
            return;
        }

        /* Si esta en Modo Telefono Vertical, mapear la cruceta segun como se sujeta la PS Vita */
        unsigned int btn_next_short = SCE_CTRL_DOWN;
        unsigned int btn_prev_short = SCE_CTRL_UP;
        unsigned int btn_nav_left   = SCE_CTRL_LEFT;
        unsigned int btn_nav_right  = SCE_CTRL_RIGHT;
        if (shorts_phone_mode) {
            if (shorts_phone_clockwise) {
                /* Rotado 90° horario (cruceta arriba al sujetar como telefono):
                 * Fisico IZQUIERDA = Abajo en telefono (Siguiente Short)
                 * Fisico DERECHA   = Arriba en telefono (Short anterior) */
                btn_next_short = SCE_CTRL_DOWN | SCE_CTRL_LEFT;
                btn_prev_short = SCE_CTRL_UP;
                btn_nav_left   = 0;
                btn_nav_right  = SCE_CTRL_RIGHT;
            } else {
                btn_next_short = SCE_CTRL_DOWN | SCE_CTRL_RIGHT;
                btn_prev_short = SCE_CTRL_UP;
                btn_nav_left   = SCE_CTRL_LEFT;
                btn_nav_right  = 0;
            }
        }

        /* Siguiente Short */
        if (pressed & btn_next_short) {
            if (online_video_count > 0) {
                audio_play_nav();
                unsigned int next_idx = (shorts_index + 1 < online_video_count) ? (shorts_index + 1) : 0;
                start_short_playback(next_idx);
                status_message = "Siguiente Short";
            }
            return;
        }

        /* Short anterior */
        if (pressed & btn_prev_short) {
            if (online_video_count > 0 && shorts_index > 0) {
                audio_play_nav();
                start_short_playback(shorts_index - 1);
                status_message = "Short anterior";
            }
            return;
        }

        /* Navegar entre Barra Lateral, Suscribirse, Play, Like, Dislike, Comentarios */
        if (btn_nav_left && (pressed & btn_nav_left)) {
            audio_play_nav();
            if (shorts_focus_btn == SHORTS_BTN_SUBSCRIBE) {
                if (!shorts_phone_mode) {
                    sidebar_active = 1;
                    selected_sidebar = 1;
                } else {
                    shorts_focus_btn = SHORTS_BTN_SHARE;
                }
            } else if (shorts_focus_btn == SHORTS_BTN_PLAY) {
                shorts_focus_btn = SHORTS_BTN_SUBSCRIBE;
            } else if (shorts_focus_btn > SHORTS_BTN_PLAY) {
                shorts_focus_btn--;
            }
            s_phone_ui_dirty = 1;
            return;
        }

        if (btn_nav_right && (pressed & btn_nav_right)) {
            audio_play_nav();
            if (shorts_focus_btn == SHORTS_BTN_SUBSCRIBE) {
                shorts_focus_btn = SHORTS_BTN_PLAY;
            } else if (shorts_focus_btn < SHORTS_BTN_COUNT - 1) {
                shorts_focus_btn++;
            } else {
                shorts_focus_btn = SHORTS_BTN_SUBSCRIBE;
            }
            s_phone_ui_dirty = 1;
            return;
        }

        /* Atajos rapidos en Shorts: CUADRADO = Comentarios, L = Like, R = Suscribirse */
        if (pressed & SCE_CTRL_SQUARE) {
            audio_play_select();
            shorts_show_comments = !shorts_show_comments;
            shorts_comment_scroll = 0;
            s_phone_ui_dirty = 1;
            if (shorts_show_comments && shorts_comment_count == 0 && shorts_meta_state != 1) {
                start_shorts_meta_request(player_video_id);
            }
            return;
        }
        if (pressed & SCE_CTRL_LTRIGGER) {
            audio_play_select();
            player_like_state = (player_like_state == 1) ? 0 : 1;
            youtube_set_video_rating(player_video_id, player_like_state);
            s_phone_ui_dirty = 1;
            status_message = (player_like_state == 1) ? "Te gusta este Short" : "Like eliminado";
            return;
        }
        if (pressed & SCE_CTRL_RTRIGGER) {
            audio_play_select();
            player_is_subscribed = youtube_toggle_channel_subscription(player_video_author);
            subs_video_count = 0;
            subs_worker_state = 0;
            s_phone_ui_dirty = 1;
            status_message = player_is_subscribed ? "Suscrito al canal" : "Suscripcion cancelada";
            return;
        }

        /* Boton X: Ejecutar accion del boton enfocado en el Short */
        if (pressed & SCE_CTRL_CROSS) {
            audio_play_select();
            if (player_error_msg[0] != '\0') {
                start_short_playback(shorts_index);
                return;
            }
            if (shorts_phone_mode && video_player_get_status() == VPLAYER_PAUSED) {
                video_player_play();
                s_phone_ui_dirty = 1;
                return;
            }
            switch (shorts_focus_btn) {
                case SHORTS_BTN_PLAY:
                    video_player_toggle_pause();
                    break;
                case SHORTS_BTN_SUBSCRIBE:
                    player_is_subscribed = youtube_toggle_channel_subscription(player_video_author);
                    subs_video_count = 0;
                    subs_worker_state = 0;
                    status_message = player_is_subscribed ? "Suscrito al canal" : "Suscripcion cancelada";
                    break;
                case SHORTS_BTN_LIKE:
                    player_like_state = (player_like_state == 1) ? 0 : 1;
                    youtube_set_video_rating(player_video_id, player_like_state);
                    status_message = (player_like_state == 1) ? "Te gusta este Short" : "Like eliminado";
                    break;
                case SHORTS_BTN_DISLIKE:
                    player_like_state = (player_like_state == -1) ? 0 : -1;
                    youtube_set_video_rating(player_video_id, player_like_state);
                    status_message = (player_like_state == -1) ? "No te gusta este Short" : "Dislike eliminado";
                    break;
                case SHORTS_BTN_COMMENTS:
                    shorts_show_comments = !shorts_show_comments;
                    shorts_comment_scroll = 0;
                    if (shorts_show_comments && shorts_comment_count == 0 && shorts_meta_state != 1) {
                        start_shorts_meta_request(player_video_id);
                    }
                    break;
                case SHORTS_BTN_SHARE:
                    shorts_phone_mode = !shorts_phone_mode;
                    status_message = shorts_phone_mode
                        ? "Modo Telefono Vertical activado"
                        : "Vista estandar de Shorts";
                    break;
                default:
                    break;
            }
            s_phone_ui_dirty = 1;
            return;
        }

        if (pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE)) {
            audio_play_cancel();
            if (shorts_phone_mode) {
                shorts_phone_mode = 0;
                s_phone_ui_dirty = 1;
                status_message = "Vista estandar de Shorts";
            } else {
                sidebar_active = 1;
                selected_sidebar = 1;
            }
            return;
        }

        /* Soporte Touch y Deslizamiento en Modo Telefono Vertical vs Modo Estandar */
        if (touch && touch->reportNum > 0) {
            int tx = touch->report[0].x / 2;
            int ty = touch->report[0].y / 2;

            if (shorts_phone_mode) {
                /* Coordenadas en el espacio del telefono (px: 0..544 ancho, py: 0..960 alto) */
                int px = shorts_phone_clockwise ? ty : (544 - ty);
                int py = shorts_phone_clockwise ? (960 - tx) : tx;

                if (shorts_touch_start_y < 0) {
                    shorts_touch_start_x = px;
                    shorts_touch_start_y = py;
                    shorts_swipe_handled = 0;

                    if (py >= 16 && py <= 64 && px >= 320 && px <= 385) {
                        /* Icono Bocina/Mute en Modo Telefono */
                        shorts_muted = !shorts_muted;
                        video_player_set_volume(shorts_muted ? 0 : player_volume);
                        s_phone_ui_dirty = 1;
                        shorts_swipe_handled = 1;
                    } else if (py >= 16 && py <= 64 && px >= 395 && px <= 460) {
                        /* Icono Lupa (Buscar) en Modo Telefono */
                        shorts_phone_mode = 0;
                        open_search_dialog();
                        shorts_swipe_handled = 1;
                    } else if (py >= 16 && py <= 64 && px >= 470 && px <= 540) {
                        /* Icono Menu (...) -> Salir de Modo Telefono */
                        shorts_phone_mode = 0;
                        s_phone_ui_dirty = 1;
                        shorts_swipe_handled = 1;
                    } else if (py >= 72 && py <= 122 && px >= 16 && px <= 245) {
                        /* Pill "Suscripciones" arriba */
                        shorts_phone_mode = 0;
                        switch_to_sidebar_view(3);
                        shorts_swipe_handled = 1;
                    } else if (px >= 445 && px <= 540 && py >= 400 && py <= 488) {
                        /* Boton Corazon (Like) */
                        audio_play_select();
                        shorts_focus_btn = SHORTS_BTN_LIKE;
                        player_like_state = (player_like_state == 1) ? 0 : 1;
                        youtube_set_video_rating(player_video_id, player_like_state);
                        s_phone_ui_dirty = 1;
                        shorts_swipe_handled = 1;
                    } else if (px >= 445 && px <= 540 && py >= 492 && py <= 580) {
                        /* Boton Comentarios */
                        audio_play_select();
                        shorts_focus_btn = SHORTS_BTN_COMMENTS;
                        shorts_show_comments = !shorts_show_comments;
                        shorts_comment_scroll = 0;
                        if (shorts_show_comments && shorts_comment_count == 0 && shorts_meta_state != 1) {
                            start_shorts_meta_request(player_video_id);
                        }
                        s_phone_ui_dirty = 1;
                        shorts_swipe_handled = 1;
                    } else if (px >= 445 && px <= 540 && py >= 584 && py <= 672) {
                        /* Boton Guardar / Dislike */
                        audio_play_select();
                        shorts_focus_btn = SHORTS_BTN_DISLIKE;
                        player_like_state = (player_like_state == -1) ? 0 : -1;
                        youtube_set_video_rating(player_video_id, player_like_state);
                        s_phone_ui_dirty = 1;
                        shorts_swipe_handled = 1;
                    } else if (py >= 760 && py <= 838 && px >= 20 && px <= 380) {
                        /* Boton Suscribirse blanco abajo */
                        audio_play_select();
                        shorts_focus_btn = SHORTS_BTN_SUBSCRIBE;
                        player_is_subscribed = youtube_toggle_channel_subscription(player_video_author);
                        subs_video_count = 0;
                        subs_worker_state = 0;
                        s_phone_ui_dirty = 1;
                        shorts_swipe_handled = 1;
                    } else if (py >= 890 && px <= 140) {
                        /* Barra inferior: Home */
                        shorts_phone_mode = 0;
                        switch_to_sidebar_view(0);
                        shorts_swipe_handled = 1;
                    } else if (py >= 890 && px >= 400) {
                        /* Barra inferior: Perfil */
                        shorts_phone_mode = 0;
                        account_view = 1;
                        shorts_swipe_handled = 1;
                    }
                } else if (!shorts_swipe_handled) {
                    int dpy = py - shorts_touch_start_y;
                    if (dpy < -50) {
                        shorts_swipe_handled = 1;
                        if (online_video_count > 0) {
                            audio_play_nav();
                            unsigned int next_idx = (shorts_index + 1 < online_video_count) ? (shorts_index + 1) : 0;
                            start_short_playback(next_idx);
                        }
                    } else if (dpy > 50) {
                        shorts_swipe_handled = 1;
                        if (online_video_count > 0 && shorts_index > 0) {
                            audio_play_nav();
                            start_short_playback(shorts_index - 1);
                        }
                    }
                }
                previous_touch_x = tx;
                previous_touch_y = ty;
                return;
            }

            if (shorts_touch_start_y < 0) {
                shorts_touch_start_x = tx;
                shorts_touch_start_y = ty;
                shorts_swipe_handled = 0;

                /* Verificar toques directos en botones */
                if (tx < 52 && ty < 50) {
                    account_view = 1;
                    sidebar_active = 0;
                } else if (tx < 56 && ty >= 60 && ty < 515) {
                    unsigned int target_sb = (ty >= 450) ? 5 : (unsigned int)((ty - 60) / 62);
                    if (target_sb >= 5 && ty < 450) target_sb = 4;
                    audio_play_select();
                    switch_to_sidebar_view(target_sb);
                } else if (tx >= 584 && tx <= 628 && ty >= 14 && ty <= 56) {
                    /* Boton Expandir [↗] arriba a la derecha del Short -> Modo Telefono Vertical */
                    audio_play_select();
                    shorts_phone_mode = 1;
                    s_phone_ui_dirty = 1;
                    status_message = "Modo Telefono Vertical (START o L+R: Salir)";
                    shorts_swipe_handled = 1;
                } else if (tx >= 64 && tx <= 325 && ty >= 395 && ty <= 442) {
                    /* Boton Suscribirse (abajo a la izquierda) */
                    audio_play_select();
                    shorts_focus_btn = SHORTS_BTN_SUBSCRIBE;
                    player_is_subscribed = youtube_toggle_channel_subscription(player_video_author);
                    subs_video_count = 0;
                    subs_worker_state = 0;
                    status_message = player_is_subscribed ? "Suscrito al canal" : "Suscripcion cancelada";
                    shorts_swipe_handled = 1;
                } else if (tx >= 624 && tx <= 690 && ty >= 195 && ty <= 260) {
                    /* Boton Like */
                    audio_play_select();
                    shorts_focus_btn = SHORTS_BTN_LIKE;
                    player_like_state = (player_like_state == 1) ? 0 : 1;
                    youtube_set_video_rating(player_video_id, player_like_state);
                    shorts_swipe_handled = 1;
                } else if (tx >= 624 && tx <= 690 && ty >= 264 && ty <= 329) {
                    /* Boton Dislike */
                    audio_play_select();
                    shorts_focus_btn = SHORTS_BTN_DISLIKE;
                    player_like_state = (player_like_state == -1) ? 0 : -1;
                    youtube_set_video_rating(player_video_id, player_like_state);
                    shorts_swipe_handled = 1;
                } else if (tx >= 624 && tx <= 690 && ty >= 333 && ty <= 398) {
                    /* Boton Comentarios */
                    audio_play_select();
                    shorts_focus_btn = SHORTS_BTN_COMMENTS;
                    shorts_show_comments = !shorts_show_comments;
                    shorts_comment_scroll = 0;
                    if (shorts_show_comments && shorts_comment_count == 0 && shorts_meta_state != 1) {
                        start_shorts_meta_request(player_video_id);
                    }
                    shorts_swipe_handled = 1;
                } else if (tx >= 624 && tx <= 690 && ty >= 402 && ty <= 466) {
                    /* Boton Compartir / Pantalla completa */
                    audio_play_select();
                    shorts_focus_btn = SHORTS_BTN_SHARE;
                    if (shorts_index < online_video_count) {
                        YoutubeVideo cur_v = online_videos[shorts_index];
                        open_player_for_video(&cur_v);
                    }
                    shorts_swipe_handled = 1;
                } else if (tx >= 880 && tx <= 948 && ty >= 245 && ty <= 310) {
                    /* Boton flecha abajo (Siguiente Short) a la derecha */
                    if (online_video_count > 0) {
                        audio_play_nav();
                        unsigned int next_idx = (shorts_index + 1 < online_video_count) ? (shorts_index + 1) : 0;
                        start_short_playback(next_idx);
                    }
                    shorts_swipe_handled = 1;
                } else if (tx >= 384 && tx <= 426 && ty >= 16 && ty <= 56) {
                    /* Boton Volumen/Mute arriba a la izquierda del Short */
                    shorts_muted = !shorts_muted;
                    video_player_set_volume(shorts_muted ? 0 : player_volume);
                    shorts_swipe_handled = 1;
                }
            } else if (!shorts_swipe_handled) {
                int dy = ty - shorts_touch_start_y;
                if (dy < -36) {
                    /* Deslizamiento de abajo hacia arriba -> Siguiente Short */
                    shorts_swipe_handled = 1;
                    if (online_video_count > 0) {
                        audio_play_nav();
                        unsigned int next_idx = (shorts_index + 1 < online_video_count) ? (shorts_index + 1) : 0;
                        start_short_playback(next_idx);
                        status_message = "Siguiente Short";
                    }
                } else if (dy > 36) {
                    /* Deslizamiento de arriba hacia abajo -> Short anterior */
                    shorts_swipe_handled = 1;
                    if (online_video_count > 0 && shorts_index > 0) {
                        audio_play_nav();
                        start_short_playback(shorts_index - 1);
                        status_message = "Short anterior";
                    }
                }
            }
            previous_touch_x = tx;
            previous_touch_y = ty;
        } else {
            /* Si solto el toque en el centro del video sin deslizar -> Play/Pause */
            if (shorts_touch_start_y >= 0 && !shorts_swipe_handled) {
                if (shorts_phone_mode) {
                    if (shorts_touch_start_x >= 30 && shorts_touch_start_x <= 420 &&
                        shorts_touch_start_y >= 140 && shorts_touch_start_y <= 740) {
                        video_player_toggle_pause();
                        s_phone_ui_dirty = 1;
                    }
                } else if (shorts_touch_start_x >= 340 && shorts_touch_start_x <= 630 &&
                           shorts_touch_start_y >= 10 && shorts_touch_start_y <= 526) {
                    video_player_toggle_pause();
                }
            }
            shorts_touch_start_x = -1;
            shorts_touch_start_y = -1;
            shorts_swipe_handled = 0;
            previous_touch_x = -1;
            previous_touch_y = -1;
        }

        /* Panel tactil trasero para deslizar Shorts (deshabilitado en Modo Telefono / Pantalla Completa) */
        if (!shorts_phone_mode && touch_back && touch_back->reportNum > 0) {
            int back_y = touch_back->report[0].y;
            if (previous_back_touch_y >= 0) {
                int delta_back_y = back_y - previous_back_touch_y;
                if (delta_back_y < -80 && online_video_count > 0) {
                    unsigned int next_idx = (shorts_index + 1 < online_video_count) ? (shorts_index + 1) : 0;
                    start_short_playback(next_idx);
                    audio_play_nav();
                    previous_back_touch_y = back_y;
                } else if (delta_back_y > 80 && online_video_count > 0 && shorts_index > 0) {
                    start_short_playback(shorts_index - 1);
                    audio_play_nav();
                    previous_back_touch_y = back_y;
                }
            } else {
                previous_back_touch_y = back_y;
            }
        } else {
            previous_back_touch_y = -1;
        }
        return;
    }

    if (sidebar_active) {
        if (pressed & SCE_CTRL_UP) {
            audio_play_nav();
            if (selected_sidebar == 0) {
                account_view = 1;
                sidebar_active = 0;
                account_status = youtube_is_logged_in()
                    ? "Sesion guardada en esta consola"
                    : "Inicia sesion o registrate";
            } else {
                --selected_sidebar;
            }
        } else if (pressed & SCE_CTRL_DOWN) {
            audio_play_nav();
            selected_sidebar = (selected_sidebar + 1) % SIDEBAR_ITEMS;
        } else if (pressed & SCE_CTRL_RIGHT) {
            audio_play_nav();
            sidebar_active = 0;
        } else if (pressed & SCE_CTRL_CROSS) {
            audio_play_select();
            switch_to_sidebar_view(selected_sidebar);
        }
    } else {
        if (active_view == 5) {
            if (pressed & SCE_CTRL_UP) {
                selected_setting = selected_setting == 0 ? 3 : selected_setting - 1;
                audio_play_nav();
            } else if (pressed & SCE_CTRL_DOWN) {
                selected_setting = (selected_setting + 1) % 4;
                audio_play_nav();
            } else if (pressed & SCE_CTRL_LEFT) {
                sidebar_active = 1;
                audio_play_nav();
            }
        } else {
            unsigned int row_cols = (has_home_shorts_shelf() && selected_row == 1)
                ? (home_shorts_count < HOME_SHORTS_SHELF_COLS ? home_shorts_count : HOME_SHORTS_SHELF_COLS)
                : CARD_COLUMNS;
            if (row_cols == 0) row_cols = 1;

            if (pressed & SCE_CTRL_UP) {
                audio_play_nav();
                if (selected_row == 0 && content_offset >= CARD_COLUMNS) {
                    content_offset -= CARD_COLUMNS;
                    if (has_home_shorts_shelf()) {
                        selected_row = 1;
                    }
                } else {
                    selected_row = selected_row == 0 ? CARD_ROWS - 1 : selected_row - 1;
                }
                unsigned int new_cols = (has_home_shorts_shelf() && selected_row == 1)
                    ? (home_shorts_count < HOME_SHORTS_SHELF_COLS ? home_shorts_count : HOME_SHORTS_SHELF_COLS)
                    : CARD_COLUMNS;
                if (selected_column >= new_cols) selected_column = new_cols - 1;
            } else if (pressed & SCE_CTRL_DOWN) {
                audio_play_nav();
                unsigned int total_active = search_results_active ? search_video_count :
                    (active_view == 3 ? subs_video_count :
                    (active_view == 4 ? history_count() : online_video_count));
                if (has_home_shorts_shelf() && selected_row == 1 && total_active > CARD_COLUMNS) {
                    content_offset = CARD_COLUMNS;
                    selected_row = 0;
                    if (selected_column >= CARD_COLUMNS) selected_column = CARD_COLUMNS - 1;
                } else if (selected_row == CARD_ROWS - 1 &&
                    content_offset + CARD_ROWS * CARD_COLUMNS < total_active &&
                    content_offset + CARD_ROWS * CARD_COLUMNS + CARD_COLUMNS <= total_active) {
                    content_offset += CARD_COLUMNS;
                } else {
                    selected_row = (selected_row + 1) % CARD_ROWS;
                }
                unsigned int new_cols = (has_home_shorts_shelf() && selected_row == 1)
                    ? (home_shorts_count < HOME_SHORTS_SHELF_COLS ? home_shorts_count : HOME_SHORTS_SHELF_COLS)
                    : CARD_COLUMNS;
                if (selected_column >= new_cols) selected_column = new_cols - 1;
            }

            if (pressed & SCE_CTRL_LEFT) {
                audio_play_nav();
                if (selected_column == 0) {
                    sidebar_active = 1;
                } else {
                    --selected_column;
                }
            } else if (pressed & SCE_CTRL_RIGHT) {
                audio_play_nav();
                selected_column = (selected_column + 1) % row_cols;
            }
        }
    }

    if ((pressed & SCE_CTRL_SQUARE) && !sidebar_active && active_view == 4 && !search_results_active) {
        history_clear();
        clear_thumbnails(history_thumbnails);
        memset(history_thumb_ids, 0, sizeof(history_thumb_ids));
        audio_play_cancel();
        content_offset = 0;
        selected_row = 0;
        selected_column = 0;
        status_message = "Historial borrado";
    }

    if ((pressed & SCE_CTRL_CROSS) && !sidebar_active) {
        if (active_view == 5) {
            if (selected_setting == 0) {
                youtube_cycle_quality();
                youtube_save_config();
                audio_play_select();
                status_message = "Calidad actualizada";
            } else if (selected_setting == 1) {
                youtube_toggle_autoplay();
                youtube_save_config();
                audio_play_select();
                status_message = youtube_autoplay() ? "Autoplay activado" : "Autoplay desactivado";
            } else if (selected_setting == 2) {
                audio_play_nav();
                status_message = "Solo tema oscuro disponible";
            } else if (selected_setting == 3) {
                account_view = 1;
                audio_play_select();
                status_message = "Cuenta de Google OAuth 2.0";
            }
        } else if (has_home_shorts_shelf() && selected_row == 1) {
            open_home_short_at(selected_column);
        } else if (active_view == 4 && !search_results_active) {
            unsigned int count = history_count();
            unsigned int video_index = content_offset +
                selected_row * CARD_COLUMNS + selected_column;
            if (video_index < count) {
                const YoutubeVideo *hvid = history_get(video_index);
                if (hvid) {
                    audio_play_select();
                    open_player_for_video(hvid);
                    status_message = "Consultando streams...";
                }
            } else {
                audio_play_cancel();
                status_message = "No hay video en esta posicion";
            }
        } else if (active_view == 3 && !search_results_active) {
            if (!youtube_has_subscriptions()) {
                account_view = 1;
                audio_play_select();
                status_message = "Inicia sesion con Google o suscribete a un canal";
            } else {
                unsigned int video_index = content_offset +
                    selected_row * CARD_COLUMNS + selected_column;
                if (video_index < subs_video_count) {
                    audio_play_select();
                    open_player_for_video(&subs_videos[video_index]);
                    status_message = "Consultando streams...";
                } else {
                    audio_play_cancel();
                    status_message = "No hay video en esta posicion";
                }
            }
        } else {
            YoutubeVideo *videos = search_results_active
                ? search_videos : online_videos;
            unsigned int count = search_results_active
                ? search_video_count : online_video_count;
            unsigned int video_index = content_offset +
                selected_row * CARD_COLUMNS + selected_column;
            if (video_index < count) {
                audio_play_select();
                open_player_for_video(&videos[video_index]);
                status_message = "Consultando streams...";
            } else {
                audio_play_cancel();
                status_message = "No hay video disponible";
            }
        }
    }

    if (pressed & SCE_CTRL_TRIANGLE) {
        audio_play_cancel();
        if (search_results_active) {
            search_results_active = 0;
            search_label[0] = 0;
            status_message = "Resultados cerrados";
        } else {
            status_message = "Selecciona un video con X";
        }
    }

    if (touch && touch->reportNum > 0) {
        int x = touch->report[0].x / 2;
        int y = touch->report[0].y / 2;

        if (previous_touch_x >= 0) {
            int delta_x = x - previous_touch_x;
            int delta_y = y - previous_touch_y;
            unsigned int total_active = search_results_active ? search_video_count :
                (active_view == 3 ? subs_video_count :
                (active_view == 4 ? history_count() : online_video_count));
            if (delta_x > 45) {
                sidebar_active = 0;
            } else if (delta_x < -45) {
                sidebar_active = 1;
            } else if (delta_y > 35) {
                if (selected_row == CARD_ROWS - 1 &&
                    content_offset + CARD_ROWS * CARD_COLUMNS < total_active &&
                    content_offset + CARD_ROWS * CARD_COLUMNS + CARD_COLUMNS <= total_active) {
                    content_offset += CARD_COLUMNS;
                } else {
                    selected_row = (selected_row + 1) % CARD_ROWS;
                }
            } else if (delta_y < -35) {
                if (selected_row == 0 && content_offset >= CARD_COLUMNS) {
                    content_offset -= CARD_COLUMNS;
                } else {
                    selected_row = selected_row == 0
                        ? CARD_ROWS - 1 : selected_row - 1;
                }
            }
        }

        if (x < 52 && y < 50) {
            account_view = 1;
            account_status = youtube_is_logged_in()
                ? "Sesion guardada en esta consola"
                : "Inicia sesion o registrate";
            sidebar_active = 0;
        } else if (x < 56 && y >= 60 && y < 515 && previous_touch_x < 0) {
            unsigned int target_sb;
            if (y >= 450) {
                target_sb = 5;
            } else {
                target_sb = (unsigned int)((y - 60) / 62);
                if (target_sb >= 5) target_sb = 4;
            }
            audio_play_select();
            switch_to_sidebar_view(target_sb);
        } else if (x >= 76 && y >= 88 && y < 510 && previous_touch_x < 0) {
            sidebar_active = 0;
            if (active_view == 5) {
                if (y >= 120 && y < 172) {
                    selected_setting = 0;
                    youtube_cycle_quality();
                    youtube_save_config();
                    audio_play_select();
                    status_message = "Calidad actualizada";
                } else if (y >= 180 && y < 232) {
                    selected_setting = 1;
                    youtube_toggle_autoplay();
                    youtube_save_config();
                    audio_play_select();
                    status_message = youtube_autoplay() ? "Autoplay activado" : "Autoplay desactivado";
                } else if (y >= 240 && y < 292) {
                    selected_setting = 2;
                    audio_play_nav();
                    status_message = "Solo tema oscuro disponible";
                } else if (y >= 300 && y < 352) {
                    selected_setting = 3;
                    account_view = 1;
                    audio_play_select();
                }
            } else if (has_home_shorts_shelf() && y >= 280) {
                unsigned int max_cols = home_shorts_count < HOME_SHORTS_SHELF_COLS
                    ? home_shorts_count : HOME_SHORTS_SHELF_COLS;
                unsigned int tapped_col = (unsigned int)((x - 76) / 174);
                if (tapped_col >= max_cols) tapped_col = max_cols - 1;
                if (selected_row == 1 && selected_column == tapped_col) {
                    open_home_short_at(tapped_col);
                } else {
                    selected_row = 1;
                    selected_column = tapped_col;
                    audio_play_nav();
                }
            } else {
                unsigned int tapped_col = (unsigned int)((x - 76) / 292);
                unsigned int tapped_row = (unsigned int)((y - 88) / 216);
                if (tapped_col >= CARD_COLUMNS) tapped_col = CARD_COLUMNS - 1;
                if (tapped_row >= CARD_ROWS) tapped_row = CARD_ROWS - 1;
                if (selected_column == tapped_col && selected_row == tapped_row) {
                    /* Segundo toque sobre la misma tarjeta abre el video */
                    unsigned int video_index = content_offset + tapped_row * CARD_COLUMNS + tapped_col;
                    if (active_view == 4 && !search_results_active) {
                        const YoutubeVideo *hv = history_get(video_index);
                        if (hv) open_player_for_video(hv);
                    } else if (active_view == 3 && !search_results_active) {
                        if (video_index < subs_video_count) open_player_for_video(&subs_videos[video_index]);
                    } else {
                        YoutubeVideo *videos = search_results_active ? search_videos : online_videos;
                        unsigned int count = search_results_active ? search_video_count : online_video_count;
                        if (video_index < count) open_player_for_video(&videos[video_index]);
                    }
                } else {
                    selected_column = tapped_col;
                    selected_row = tapped_row;
                    audio_play_nav();
                }
            }
        } else if (x >= 52 && x < 270 && y < 46) {
            log_event("search_open_source=touch x=%d y=%d", x, y);
            open_search_dialog();
        }

        previous_touch_x = x;
        previous_touch_y = y;
    } else {
        previous_touch_x = -1;
        previous_touch_y = -1;
    }

    if (touch_back && touch_back->reportNum > 0) {
        int back_y = touch_back->report[0].y;
        if (previous_back_touch_y >= 0) {
            int delta_back_y = back_y - previous_back_touch_y;
            unsigned int total_active = search_results_active ? search_video_count :
                (active_view == 3 ? subs_video_count :
                (active_view == 4 ? history_count() : online_video_count));
            if (delta_back_y > 80) {
                if (content_offset + CARD_ROWS * CARD_COLUMNS < total_active &&
                    content_offset + CARD_ROWS * CARD_COLUMNS + CARD_COLUMNS <= total_active) {
                    content_offset += CARD_COLUMNS;
                    audio_play_nav();
                    previous_back_touch_y = back_y;
                }
            } else if (delta_back_y < -80) {
                if (content_offset >= CARD_COLUMNS) {
                    content_offset -= CARD_COLUMNS;
                    audio_play_nav();
                    previous_back_touch_y = back_y;
                }
            }
        } else {
            previous_back_touch_y = back_y;
        }
    } else {
        previous_back_touch_y = -1;
    }
}

static void draw_player_screen(void)
{
    /* 1. Dibujar el video en pantalla completa (960 x 544) */
    video_player_draw(0, 0, 960, 544);

    VideoPlayerStatus st = video_player_get_status();
    uint64_t cur_sec = video_player_get_current_time() / 1000;
    unsigned int cur_m = (unsigned int)(cur_sec / 60);
    unsigned int cur_s = (unsigned int)(cur_sec % 60);

    uint64_t dur_ms = video_player_get_duration();
    unsigned int total_sec = (player_video_duration > 0) ? player_video_duration : ((dur_ms > 0) ? (unsigned int)(dur_ms / 1000) : 0);
    unsigned int total_m = total_sec / 60;
    unsigned int total_s = total_sec % 60;

    float progress = (total_sec > 0) ? ((float)cur_sec / (float)total_sec) : 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    /* Descontar temporizador de OSD durante reproduccion */
    if (st == VPLAYER_PLAYING && player_overlay_timer > 0) {
        player_overlay_timer--;
    }

    int has_error = (player_error_msg[0] != '\0' || st == VPLAYER_ERROR);
    const char *notice_txt = video_player_get_notice();

    /* Mensajes de extraccion de stream o buffering (sin tapar la tarjeta de error) */
    if (!has_error) {
        if (player_state == 1) {
            char q_msg[96];
            snprintf(q_msg, sizeof(q_msg), "Cargando stream %s...", youtube_quality_label());
            draw_rounded_rectangle(270.0f, 180.0f, 420.0f, 54.0f, 10.0f, RGBA8(15, 15, 15, 225));
            draw_text(300.0f, 214.0f, RGBA8(250, 250, 255, 255), 0.78f, q_msg);
        } else if (st == VPLAYER_BUFFERING) {
            char buf_info[96];
            snprintf(buf_info, sizeof(buf_info), "Cargando buffer... (%u KB)",
                     video_player_get_buffered_bytes() / 1024);
            draw_rounded_rectangle(270.0f, 180.0f, 420.0f, 54.0f, 10.0f, RGBA8(15, 15, 15, 225));
            draw_text(300.0f, 214.0f, RGBA8(250, 250, 255, 255), 0.75f, buf_info);
        } else if (st == VPLAYER_PLAYING && !video_player_has_frame()) {
            draw_rounded_rectangle(270.0f, 180.0f, 420.0f, 54.0f, 10.0f, RGBA8(15, 15, 15, 225));
            draw_text(320.0f, 214.0f, RGBA8(250, 250, 255, 255), 0.75f, "Iniciando reproduccion...");
        }

        if (notice_txt && notice_txt[0]) {
            float nw = font ? (float)vita2d_pgf_text_width(font, 0.56f, notice_txt) + 36.0f : 320.0f;
            float nx = (960.0f - nw) * 0.5f;
            draw_rounded_rectangle(nx, 76.0f, nw, 34.0f, 17.0f, RGBA8(20, 20, 22, 230));
            draw_rounded_outline(nx, 76.0f, nw, 34.0f, 17.0f, 1.5f, RGBA8(90, 90, 100, 255));
            draw_text(nx + 18.0f, 99.0f, RGBA8(245, 245, 250, 255), 0.56f, notice_txt);
        }
    }

    int show_osd = (player_overlay_timer > 0 || st == VPLAYER_PAUSED ||
                    player_state == 1 || st == VPLAYER_BUFFERING ||
                    has_error || player_show_desc || player_show_comments || player_show_settings);
    if (!show_osd) return;

    /* 2. Degradados / Scrims estilo YouTube TV */
    /* Scrim superior para contraste del titulo */
    for (int y = 0; y < 90; y += 3) {
        float f = 1.0f - ((float)y / 90.0f);
        unsigned char alpha = (unsigned char)(190.0f * f * f);
        vita2d_draw_rectangle(0, y, 960, 3, RGBA8(5, 5, 5, alpha));
    }

    if (!has_error) {
        /* Scrim inferior degradado suave */
        for (int y = 230; y < 330; y += 4) {
            float f = ((float)(y - 230) / 100.0f);
            unsigned char alpha = (unsigned char)(210.0f * f * f);
            vita2d_draw_rectangle(0, y, 960, 4, RGBA8(10, 10, 10, alpha));
        }
        /* Fondo oscuro semi-solido detras de controles y carrusel */
        vita2d_draw_rectangle(0, 330, 960, 214, RGBA8(10, 10, 10, 240));
    }

    /* 3. Metadatos del Video (Esquina Superior Izquierda) */
    draw_title_with_emojis(34.0f, 38.0f, RGBA8(255, 255, 255, 255), 0.85f,
                           player_video_title[0] ? player_video_title : "Reproduciendo video");

    char views_str[32];
    format_views(player_video_views, views_str, sizeof(views_str));
    char meta_str[160];
    if (player_video_is_live) {
        snprintf(meta_str, sizeof(meta_str), "%s  *  %s espectadores  *  En vivo",
                 player_video_author[0] ? player_video_author : "VitaTube", views_str);
    } else if (player_video_published[0] != '\0') {
        snprintf(meta_str, sizeof(meta_str), "%s  *  %s vistas  *  %s",
                 player_video_author[0] ? player_video_author : "VitaTube",
                 views_str, player_video_published);
    } else {
        snprintf(meta_str, sizeof(meta_str), "%s  *  %s vistas",
                 player_video_author[0] ? player_video_author : "VitaTube", views_str);
    }
    draw_text(34.0f, 66.0f, RGBA8(170, 170, 170, 255), 0.54f, meta_str);

    if (has_error) {
        const char *err_desc = player_error_msg[0]
            ? player_error_msg
            : (video_player_get_error()[0] ? video_player_get_error() : "No se pudo cargar o reproducir el video");
        float ex = 150.0f, ey = 120.0f, ew = 660.0f, eh = 175.0f;
        draw_rounded_rectangle(ex, ey, ew, eh, 14.0f, RGBA8(20, 20, 24, 248));
        draw_rounded_outline(ex, ey, ew, eh, 14.0f, 2.0f, RGBA8(220, 45, 55, 255));

        vita2d_draw_fill_circle(ex + 44.0f, ey + 44.0f, 18.0f, RGBA8(220, 35, 45, 255));
        vita2d_draw_rectangle(ex + 42.0f, ey + 32.0f, 4.0f, 14.0f, RGBA8(255, 255, 255, 255));
        vita2d_draw_rectangle(ex + 42.0f, ey + 49.0f, 4.0f, 4.0f, RGBA8(255, 255, 255, 255));

        draw_text(ex + 76.0f, ey + 42.0f, RGBA8(255, 255, 255, 255), 0.80f,
                  player_video_is_live ? "Error al cargar el directo" : "Error al cargar el video");
        draw_text(ex + 76.0f, ey + 74.0f, RGBA8(205, 208, 218, 255), 0.54f, err_desc);

        draw_rounded_rectangle(260.0f, 236.0f, 200.0f, 42.0f, 10.0f, RGBA8(245, 245, 245, 255));
        draw_text(294.0f, 263.0f, RGBA8(15, 15, 15, 255), 0.60f, "[X] Reintentar");

        draw_rounded_rectangle(500.0f, 236.0f, 200.0f, 42.0f, 10.0f, RGBA8(42, 42, 46, 255));
        draw_text(538.0f, 263.0f, RGBA8(240, 240, 245, 255), 0.60f, "[O] Volver");
        return;
    }

    /* 4. Barra de Tiempo y Progreso */
    char cur_time_str[32], total_time_str[32];
    snprintf(cur_time_str, sizeof(cur_time_str), "%u:%02u", cur_m, cur_s);
    if (player_video_is_live) {
        snprintf(total_time_str, sizeof(total_time_str), "EN VIVO");
    } else {
        snprintf(total_time_str, sizeof(total_time_str), "%u:%02u", total_m, total_s);
    }

    draw_text(34.0f, 338.0f, RGBA8(235, 235, 235, 255), 0.50f, cur_time_str);
    float total_w = vita2d_pgf_text_width(font, 0.50f, total_time_str);
    draw_text(926.0f - total_w, 338.0f, RGBA8(235, 235, 235, 255), 0.50f, total_time_str);

    float bar_x = 34.0f;
    float bar_y = 346.0f;
    float bar_w = 892.0f;
    float bar_h = (player_focus_section == PLAYER_FOCUS_SEEKBAR) ? 4.0f : 2.5f;

    /* Pista base */
    draw_rounded_rectangle(bar_x, bar_y, bar_w, bar_h, bar_h * 0.5f, RGBA8(75, 75, 75, 200));

    /* Progreso actual y cabezal */
    if (progress > 0.0f) {
        float fill_w = bar_w * progress;
        draw_rounded_rectangle(bar_x, bar_y, fill_w, bar_h, bar_h * 0.5f, RGBA8(255, 255, 255, 255));
        float head_r = (player_focus_section == PLAYER_FOCUS_SEEKBAR) ? 6.5f : 4.5f;
        vita2d_draw_fill_circle(bar_x + fill_w, bar_y + bar_h * 0.5f, head_r, RGBA8(255, 255, 255, 255));
        if (player_focus_section == PLAYER_FOCUS_SEEKBAR) {
            vita2d_draw_fill_circle(bar_x + fill_w, bar_y + bar_h * 0.5f, 2.5f, RGBA8(220, 20, 30, 255));
        }
    }

    /* 5. Fila Horizontal de Iconos de Control */
    /* Avatar (btn 0) */
    float av_cx = 52.0f, av_cy = 392.0f, av_r = 16.0f;
    int av_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_AVATAR);
    if (player_author_avatar_tex) {
        float tw = (float)vita2d_texture_get_width(player_author_avatar_tex);
        float th = (float)vita2d_texture_get_height(player_author_avatar_tex);
        if (tw > 0.0f && th > 0.0f) {
            vita2d_draw_texture_scale(player_author_avatar_tex, av_cx - av_r, av_cy - av_r,
                                      (av_r * 2.0f) / tw, (av_r * 2.0f) / th);
            draw_rounded_corner_mask(av_cx - av_r, av_cy - av_r, av_r * 2.0f, av_r * 2.0f, av_r,
                                     RGBA8(10, 10, 10, 240));
        }
    } else {
        vita2d_draw_fill_circle(av_cx, av_cy, av_r, RGBA8(210, 85, 20, 255));
        char av_init[2] = { player_video_author[0] ? (char)toupper((unsigned char)player_video_author[0]) : 'Y', '\0' };
        draw_text(av_cx - 5.0f, av_cy + 6.0f, RGBA8(255, 255, 255, 255), 0.65f, av_init);
    }
    if (av_foc) {
        draw_rounded_outline(av_cx - 19.0f, av_cy - 19.0f, 38.0f, 38.0f, 19.0f, 2.5f, RGBA8(255, 255, 255, 255));
    }

    /* Descripcion (btn 1) */
    float d_x = 76.0f, d_y = 375.0f, d_w = 94.0f, d_h = 34.0f;
    int d_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_DESC);
    draw_rounded_rectangle(d_x, d_y, d_w, d_h, 17.0f, d_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    draw_text(d_x + 12.0f, d_y + 22.0f, d_foc ? RGBA8(15, 15, 15, 255) : RGBA8(235, 235, 235, 255), 0.54f, "Descripcion");

    /* Suscribirse (btn 2) */
    float s_x = 178.0f, s_y = 375.0f, s_w = 98.0f, s_h = 34.0f;
    int s_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_SUBS);
    draw_rounded_rectangle(s_x, s_y, s_w, s_h, 17.0f, s_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    const char *sub_label = player_is_subscribed ? "Suscrito" : "Suscribirse";
    float sub_tx = player_is_subscribed ? (s_x + 22.0f) : (s_x + 14.0f);
    draw_text(sub_tx, s_y + 22.0f, s_foc ? RGBA8(15, 15, 15, 255) : (player_is_subscribed ? RGBA8(160, 160, 160, 255) : RGBA8(235, 235, 235, 255)), 0.54f, sub_label);

    /* Anterior |< (btn 3) */
    float p_cx = 435.0f, p_cy = 392.0f, p_r = 16.0f;
    int p_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_PREV);
    vita2d_draw_fill_circle(p_cx, p_cy, p_r, p_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    unsigned int p_col = p_foc ? RGBA8(15, 15, 15, 255) : RGBA8(235, 235, 235, 255);
    vita2d_draw_rectangle(p_cx - 6.5f, p_cy - 5.5f, 2.0f, 11.0f, p_col);
    for (int p = 0; p <= 7; p++) {
        vita2d_draw_line(p_cx + 4.5f - (float)p, p_cy - 5.5f + (float)p * 0.78f,
                         p_cx + 4.5f - (float)p, p_cy + 5.5f - (float)p * 0.78f, p_col);
    }

    /* Play/Pause (btn 4) - Circulo grande destacado */
    float pl_cx = 485.0f, pl_cy = 392.0f, pl_r = 24.0f;
    int pl_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_PLAY);
    vita2d_draw_fill_circle(pl_cx, pl_cy, pl_r, pl_foc ? RGBA8(255, 255, 255, 255) : RGBA8(45, 45, 45, 240));
    unsigned int pl_col = pl_foc ? RGBA8(15, 15, 15, 255) : RGBA8(255, 255, 255, 255);
    if (st == VPLAYER_PAUSED || !video_player_has_frame()) {
        /* Triangulo Play */
        for (int p = 0; p <= 12; p++) {
            vita2d_draw_line(pl_cx - 5.0f + (float)p, pl_cy - 8.5f + (float)p * 0.71f,
                             pl_cx - 5.0f + (float)p, pl_cy + 8.5f - (float)p * 0.71f, pl_col);
        }
    } else {
        /* Barras de Pausa */
        vita2d_draw_rectangle(pl_cx - 6.0f, pl_cy - 8.0f, 4.0f, 16.0f, pl_col);
        vita2d_draw_rectangle(pl_cx + 2.0f, pl_cy - 8.0f, 4.0f, 16.0f, pl_col);
    }

    /* Siguiente >| (btn 5) */
    float nx_cx = 535.0f, nx_cy = 392.0f, nx_r = 16.0f;
    int nx_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_NEXT);
    vita2d_draw_fill_circle(nx_cx, nx_cy, nx_r, nx_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    unsigned int nx_col = nx_foc ? RGBA8(15, 15, 15, 255) : RGBA8(235, 235, 235, 255);
    vita2d_draw_rectangle(nx_cx + 4.5f, nx_cy - 5.5f, 2.0f, 11.0f, nx_col);
    for (int p = 0; p <= 7; p++) {
        vita2d_draw_line(nx_cx - 4.5f + (float)p, nx_cy - 5.5f + (float)p * 0.78f,
                         nx_cx - 4.5f + (float)p, nx_cy + 5.5f - (float)p * 0.78f, nx_col);
    }

    /* Like (btn 6) */
    float lk_cx = 680.0f, lk_cy = 392.0f, lk_r = 16.0f;
    int lk_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_LIKE);
    vita2d_draw_fill_circle(lk_cx, lk_cy, lk_r, lk_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    unsigned int lk_col = (player_like_state == 1) ? RGBA8(255, 60, 80, 255) : (lk_foc ? RGBA8(15, 15, 15, 255) : RGBA8(235, 235, 235, 255));
    vita2d_draw_rectangle(lk_cx - 5.0f, lk_cy - 2.0f, 10.0f, 7.0f, lk_col);
    vita2d_draw_rectangle(lk_cx - 5.0f, lk_cy - 6.5f, 3.0f, 5.0f, lk_col);

    /* Dislike (btn 7) */
    float dk_cx = 724.0f, dk_cy = 392.0f, dk_r = 16.0f;
    int dk_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_DISLIKE);
    vita2d_draw_fill_circle(dk_cx, dk_cy, dk_r, dk_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    unsigned int dk_col = (player_like_state == -1) ? RGBA8(255, 60, 80, 255) : (dk_foc ? RGBA8(15, 15, 15, 255) : RGBA8(235, 235, 235, 255));
    vita2d_draw_rectangle(dk_cx - 5.0f, dk_cy - 5.0f, 10.0f, 7.0f, dk_col);
    vita2d_draw_rectangle(dk_cx - 5.0f, dk_cy + 1.5f, 3.0f, 5.0f, dk_col);

    /* Comentarios (btn 8) */
    float cm_cx = 768.0f, cm_cy = 392.0f, cm_r = 16.0f;
    int cm_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_COMMENTS);
    vita2d_draw_fill_circle(cm_cx, cm_cy, cm_r, cm_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    unsigned int cm_col = cm_foc ? RGBA8(15, 15, 15, 255) : RGBA8(235, 235, 235, 255);
    draw_rounded_rectangle(cm_cx - 6.5f, cm_cy - 5.0f, 13.0f, 9.0f, 2.0f, cm_col);
    vita2d_draw_line(cm_cx - 3.5f, cm_cy + 4.0f, cm_cx - 6.5f, cm_cy + 6.5f, cm_col);

    /* Guardar / Bookmark (btn 9) */
    float sv_cx = 812.0f, sv_cy = 392.0f, sv_r = 16.0f;
    int sv_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_SAVE);
    vita2d_draw_fill_circle(sv_cx, sv_cy, sv_r, sv_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    unsigned int sv_col = sv_foc ? RGBA8(15, 15, 15, 255) : RGBA8(235, 235, 235, 255);
    draw_rounded_rectangle(sv_cx - 4.5f, sv_cy - 6.0f, 9.0f, 12.0f, 1.5f, sv_col);
    vita2d_draw_line(sv_cx - 4.5f, sv_cy + 6.0f, sv_cx, sv_cy + 2.0f, sv_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    vita2d_draw_line(sv_cx + 4.5f, sv_cy + 6.0f, sv_cx, sv_cy + 2.0f, sv_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));

    /* Corazon / Agradecer (btn 10) */
    float th_cx = 856.0f, th_cy = 392.0f, th_r = 16.0f;
    int th_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_THANKS);
    vita2d_draw_fill_circle(th_cx, th_cy, th_r, th_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    unsigned int th_col = th_foc ? RGBA8(15, 15, 15, 255) : RGBA8(235, 235, 235, 255);
    vita2d_draw_fill_circle(th_cx - 2.8f, th_cy - 2.0f, 2.8f, th_col);
    vita2d_draw_fill_circle(th_cx + 2.8f, th_cy - 2.0f, 2.8f, th_col);
    for (int p = 0; p <= 5; p++) {
        vita2d_draw_line(th_cx - 5.0f + (float)p, th_cy - 1.0f + (float)p * 1.1f,
                         th_cx + 5.0f - (float)p, th_cy - 1.0f + (float)p * 1.1f, th_col);
    }

    /* Ajustes / Engranaje (btn 11) */
    float st_cx = 900.0f, st_cy = 392.0f, st_r = 16.0f;
    int st_foc = (player_focus_section == PLAYER_FOCUS_CONTROLS && player_focus_btn == PLAYER_BTN_SETTINGS);
    vita2d_draw_fill_circle(st_cx, st_cy, st_r, st_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    unsigned int st_col = st_foc ? RGBA8(15, 15, 15, 255) : RGBA8(235, 235, 235, 255);
    vita2d_draw_fill_circle(st_cx, st_cy, 6.0f, st_col);
    vita2d_draw_fill_circle(st_cx, st_cy, 3.0f, st_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 240));
    vita2d_draw_rectangle(st_cx - 7.5f, st_cy - 1.5f, 15.0f, 3.0f, st_col);
    vita2d_draw_rectangle(st_cx - 1.5f, st_cy - 7.5f, 3.0f, 15.0f, st_col);

    /* 6. Fila Horizontal de Videos Relacionados */
    float rel_y = 438.0f;
    float rel_w = 156.0f;
    float rel_h = 88.0f;
    float rel_pitch = 170.0f;

    float target_scroll = 0.0f;
    if (player_carousel_idx >= 3) {
        target_scroll = (float)(player_carousel_idx - 2) * rel_pitch;
    }
    player_carousel_scroll += (target_scroll - player_carousel_scroll) * 0.35f;

    for (unsigned int c = 0; c < player_related_count; ++c) {
        float cx = 34.0f + (float)c * rel_pitch - player_carousel_scroll;
        if (cx + rel_w < 0.0f || cx > 960.0f) continue;

        int is_card_focused = (player_focus_section == PLAYER_FOCUS_CAROUSEL &&
                               player_carousel_idx == c);

        if (is_card_focused) {
            draw_rounded_outline(cx - 3.0f, rel_y - 3.0f, rel_w + 6.0f, rel_h + 6.0f, 10.0f, 2.5f,
                                 RGBA8(255, 255, 255, 255));
        }

        const YoutubeVideo *v = &player_related_videos[c];
        vita2d_texture *t = find_history_card_texture(ONLINE_VIDEO_CAPACITY, v);
        if (t) {
            float tw = (float)vita2d_texture_get_width(t);
            float th = (float)vita2d_texture_get_height(t);
            vita2d_draw_texture_scale(t, cx, rel_y,
                                      tw > 0.0f ? rel_w / tw : 1.0f,
                                      th > 0.0f ? rel_h / th : 1.0f);
        } else {
            draw_rounded_rectangle(cx, rel_y, rel_w, rel_h, 8.0f, RGBA8(32, 32, 32, 255));
            vita2d_draw_fill_circle(cx + rel_w * 0.5f, rel_y + rel_h * 0.5f, 14.0f, RGBA8(48, 48, 48, 255));
        }

        draw_rounded_corner_mask(cx, rel_y, rel_w, rel_h, 8.0f, RGBA8(10, 10, 10, 240));
        if (v->is_live) {
            float bw = 70.0f, b_h = 18.0f;
            float bx = cx + rel_w - bw - 4.0f, b_y = rel_y + rel_h - b_h - 4.0f;
            draw_rounded_rectangle(bx, b_y, bw, b_h, 3.5f, RGBA8(220, 10, 35, 255));
            draw_text(bx + 6.0f, b_y + 13.0f, RGBA8(255, 255, 255, 255), 0.45f, "((o)) EN VIVO");
        } else if (v->duration > 0 && v->duration < 86400) {
            char dur[16] = {0};
            format_duration(v->duration, dur, sizeof(dur));
            float tw = (float)vita2d_pgf_text_width(font, 0.44f, dur);
            float bw = tw + 10.0f;
            float b_h = 16.0f;
            float bx = cx + rel_w - bw - 4.0f, b_y = rel_y + rel_h - b_h - 4.0f;
            draw_rounded_rectangle(bx, b_y, bw, b_h, 3.5f, RGBA8(0, 0, 0, 220));
            draw_text(bx + 5.0f, b_y + 12.0f, RGBA8(255, 255, 255, 255), 0.44f, dur);
        }
    }

    /* Modal emergente de descripcion */
    if (player_show_desc) {
        draw_rounded_rectangle(140.0f, 100.0f, 680.0f, 220.0f, 14.0f, RGBA8(18, 18, 18, 248));
        draw_rounded_outline(140.0f, 100.0f, 680.0f, 220.0f, 14.0f, 2.0f, RGBA8(60, 60, 60, 255));
        draw_text(165.0f, 135.0f, RGBA8(255, 255, 255, 255), 0.80f, "Informacion del video");
        char sh_title[60];
        shorten_title(player_video_title, sh_title, sizeof(sh_title));
        draw_title_with_emojis(165.0f, 172.0f, RGBA8(240, 240, 240, 255), 0.68f, sh_title);
        if (player_author_avatar_tex) {
            float tw = (float)vita2d_texture_get_width(player_author_avatar_tex);
            float th = (float)vita2d_texture_get_height(player_author_avatar_tex);
            if (tw > 0.0f && th > 0.0f) {
                vita2d_draw_texture_scale(player_author_avatar_tex, 165.0f, 186.0f, 28.0f / tw, 28.0f / th);
                draw_rounded_corner_mask(165.0f, 186.0f, 28.0f, 28.0f, 14.0f, RGBA8(18, 18, 18, 248));
            }
            draw_text(201.0f, 206.0f, RGBA8(220, 220, 225, 255), 0.60f,
                      player_video_author[0] ? player_video_author : "Canal de YouTube");
        } else {
            draw_text(165.0f, 202.0f, RGBA8(180, 180, 180, 255), 0.58f, player_video_author);
        }
        draw_text(165.0f, 236.0f, RGBA8(140, 140, 140, 255), 0.54f, meta_str);
        draw_text(165.0f, 286.0f, RGBA8(200, 200, 200, 255), 0.50f, "[X] o [O] para cerrar");
    }

    /* Modal emergente de Comentarios del video */
    if (player_show_comments) {
        float px = 140.0f, py = 46.0f, pw = 680.0f, ph = 310.0f;
        draw_rounded_rectangle(px, py, pw, ph, 14.0f, RGBA8(18, 18, 22, 250));
        draw_rounded_outline(px, py, pw, ph, 14.0f, 2.0f, RGBA8(65, 65, 75, 255));

        if (player_author_avatar_tex) {
            float tw = (float)vita2d_texture_get_width(player_author_avatar_tex);
            float th = (float)vita2d_texture_get_height(player_author_avatar_tex);
            if (tw > 0.0f && th > 0.0f) {
                vita2d_draw_texture_scale(player_author_avatar_tex, px + 18.0f, py + 12.0f, 26.0f / tw, 26.0f / th);
                draw_rounded_corner_mask(px + 18.0f, py + 12.0f, 26.0f, 26.0f, 13.0f, RGBA8(18, 18, 22, 250));
            }
        } else {
            vita2d_draw_fill_circle(px + 31.0f, py + 25.0f, 13.0f, RGBA8(210, 85, 20, 255));
        }
        char hdr_txt[128];
        snprintf(hdr_txt, sizeof(hdr_txt), "Comentarios  *  %s",
                 player_video_author[0] ? player_video_author : "YouTube");
        draw_text(px + 52.0f, py + 31.0f, RGBA8(255, 255, 255, 255), 0.68f, hdr_txt);
        draw_text(px + pw - 190.0f, py + 31.0f, RGBA8(175, 175, 185, 255), 0.48f,
                  "Arriba/Abajo: Scroll  [O]: Cerrar");
        vita2d_draw_rectangle(px + 16.0f, py + 44.0f, pw - 32.0f, 1.0f, RGBA8(55, 55, 65, 255));

        if (shorts_meta_state == 1 && shorts_comment_count == 0) {
            draw_text(px + 230.0f, py + 165.0f, RGBA8(210, 210, 220, 255), 0.62f, "Cargando comentarios...");
        } else if (shorts_comment_count == 0) {
            draw_text(px + 210.0f, py + 165.0f, RGBA8(190, 190, 200, 255), 0.58f, "No hay comentarios disponibles");
        } else {
            unsigned int max_vis = 3;
            for (unsigned int i = 0; i < max_vis && (shorts_comment_scroll + i) < shorts_comment_count; i++) {
                const YoutubeComment *c = &shorts_comments[shorts_comment_scroll + i];
                float cy = py + 54.0f + (float)i * 82.0f;
                draw_rounded_rectangle(px + 16.0f, cy, pw - 32.0f, 74.0f, 10.0f, RGBA8(30, 30, 36, 255));
                vita2d_draw_fill_circle(px + 34.0f, cy + 20.0f, 10.0f, RGBA8(95, 70, 160, 255));
                char auth_line[64];
                snprintf(auth_line, sizeof(auth_line), "%s   (Likes: %s)",
                         c->author[0] ? c->author : "@usuario",
                         c->likes[0] ? c->likes : "0");
                draw_text(px + 50.0f, cy + 24.0f, RGBA8(195, 205, 230, 255), 0.50f, auth_line);

                char c_l1[76] = {0}, c_l2[76] = {0};
                size_t clen = strlen(c->text);
                if (clen <= 70) {
                    snprintf(c_l1, sizeof(c_l1), "%s", c->text);
                } else {
                    size_t sp = 70;
                    while (sp > 35 && c->text[sp] != ' ') sp--;
                    if (c->text[sp] != ' ') sp = 70;
                    memcpy(c_l1, c->text, sp);
                    c_l1[sp] = '\0';
                    const char *r = c->text + sp;
                    while (*r == ' ') r++;
                    shorten_title(r, c_l2, 70);
                }
                draw_title_with_emojis(px + 24.0f, cy + 46.0f, RGBA8(245, 245, 250, 255), 0.52f, c_l1);
                if (c_l2[0]) {
                    draw_title_with_emojis(px + 24.0f, cy + 64.0f, RGBA8(225, 225, 235, 255), 0.50f, c_l2);
                }
            }
        }
    }

    /* Panel lateral flotante de Configuracion (Estilo YouTube TV - Solo Calidad y Tiempo) */
    if (player_show_settings) {
        float pan_w = 360.0f;
        float pan_h = 240.0f;
        float pan_x = 960.0f - pan_w - 24.0f;
        float pan_y = 54.0f;

        /* Sombra y fondo oscuro semi-opaco */
        draw_rounded_rectangle(pan_x, pan_y, pan_w, pan_h, 16.0f, RGBA8(18, 18, 18, 250));
        draw_rounded_outline(pan_x, pan_y, pan_w, pan_h, 16.0f, 2.0f, RGBA8(55, 55, 55, 255));

        /* Titulo del panel */
        draw_text(pan_x + 22.0f, pan_y + 34.0f, RGBA8(255, 255, 255, 255), 0.76f, "Configuracion");

        float rw = pan_w - 36.0f;
        float rh = 62.0f;
        float rx = pan_x + 18.0f;

        /* Fila 0: Calidad */
        float r0_y = pan_y + 52.0f;
        int sel0 = (player_settings_selected == 0);
        if (sel0) {
            draw_rounded_rectangle(rx, r0_y, rw, rh, 14.0f, RGBA8(245, 245, 245, 255));
        } else {
            draw_rounded_rectangle(rx, r0_y, rw, rh, 14.0f, RGBA8(32, 32, 32, 220));
        }
        unsigned int col0 = sel0 ? RGBA8(15, 15, 15, 255) : RGBA8(250, 250, 250, 255);
        unsigned int sub0 = sel0 ? RGBA8(80, 80, 80, 255) : RGBA8(160, 160, 160, 255);

        /* Icono HD badge */
        draw_rounded_rectangle(rx + 14.0f, r0_y + 20.0f, 26.0f, 18.0f, 3.5f, col0);
        draw_text(rx + 18.0f, r0_y + 33.5f, sel0 ? RGBA8(245, 245, 245, 255) : RGBA8(24, 24, 24, 255), 0.40f, "HD");

        draw_text(rx + 50.0f, r0_y + 28.0f, col0, 0.62f, "Calidad");
        char q_str[64];
        snprintf(q_str, sizeof(q_str), "%s   (< X >)", youtube_quality_label());
        draw_text(rx + 50.0f, r0_y + 49.0f, sub0, 0.48f, q_str);

        /* Chevron flecha */
        vita2d_draw_line(rx + rw - 22.0f, r0_y + 25.0f, rx + rw - 14.0f, r0_y + 31.0f, col0);
        vita2d_draw_line(rx + rw - 14.0f, r0_y + 31.0f, rx + rw - 22.0f, r0_y + 37.0f, col0);

        /* Fila 1: Tiempo de reproduccion */
        float r1_y = r0_y + 70.0f;
        int sel1 = (player_settings_selected == 1);
        if (sel1) {
            draw_rounded_rectangle(rx, r1_y, rw, rh, 14.0f, RGBA8(245, 245, 245, 255));
        } else {
            draw_rounded_rectangle(rx, r1_y, rw, rh, 14.0f, RGBA8(32, 32, 32, 220));
        }
        unsigned int col1 = sel1 ? RGBA8(15, 15, 15, 255) : RGBA8(250, 250, 250, 255);
        unsigned int sub1 = sel1 ? RGBA8(80, 80, 80, 255) : RGBA8(160, 160, 160, 255);

        /* Icono reloj */
        float clk_cx = rx + 27.0f;
        float clk_cy = r1_y + 29.0f;
        vita2d_draw_fill_circle(clk_cx, clk_cy, 10.0f, col1);
        vita2d_draw_fill_circle(clk_cx, clk_cy, 8.0f, sel1 ? RGBA8(245, 245, 245, 255) : RGBA8(32, 32, 32, 220));
        vita2d_draw_line(clk_cx, clk_cy - 5.5f, clk_cx, clk_cy, col1);
        vita2d_draw_line(clk_cx, clk_cy, clk_cx + 4.5f, clk_cy, col1);

        draw_text(rx + 50.0f, r1_y + 28.0f, col1, 0.62f, "Tiempo de reproduccion");
        char time_info[80];
        snprintf(time_info, sizeof(time_info), "%s / %s   (<- -30s   +30s ->)", cur_time_str, total_time_str);
        draw_text(rx + 50.0f, r1_y + 49.0f, sub1, 0.44f, time_info);

        /* Chevron flecha */
        vita2d_draw_line(rx + rw - 22.0f, r1_y + 25.0f, rx + rw - 14.0f, r1_y + 31.0f, col1);
        vita2d_draw_line(rx + rw - 14.0f, r1_y + 31.0f, rx + rw - 22.0f, r1_y + 37.0f, col1);

        /* Pie informativo */
        draw_text(pan_x + 22.0f, pan_y + pan_h - 14.0f, RGBA8(160, 160, 160, 255), 0.42f,
                  "[O] o [TRIANGULO] Volver     [X] Reiniciar (0:00)");
    }
}

/* =========================================================================
 * Renderizador de Interfaz de Telefono Vertical Rotada 90 grados (272x480 -> 544x960)
 * ========================================================================= */
#define PHONE_TEX_W 272
#define PHONE_TEX_H 480

static const unsigned char s_font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* '\'' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x14,0x08,0x3E,0x08,0x14}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' */
    {0x00,0x56,0x36,0x00,0x00}, /* ';' */
    {0x08,0x14,0x22,0x41,0x00}, /* '<' */
    {0x14,0x14,0x14,0x14,0x14}, /* '=' */
    {0x00,0x41,0x22,0x14,0x08}, /* '>' */
    {0x02,0x01,0x51,0x09,0x06}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@' */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 'F' */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J' */
    {0x7F,0x08,0x14,0x22,0x41}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L' */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V' */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X' */
    {0x07,0x08,0x70,0x08,0x07}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z' */
    {0x00,0x7F,0x41,0x41,0x00}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* '\\' */
    {0x00,0x41,0x41,0x7F,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* '^' */
    {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00}, /* '`' */
    {0x20,0x54,0x54,0x54,0x78}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f' */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j' */
    {0x7F,0x10,0x28,0x44,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l' */
    {0x7C,0x04,0x18,0x04,0x78}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' */
    {0x08,0x14,0x14,0x18,0x7C}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r' */
    {0x48,0x54,0x54,0x54,0x20}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't' */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v' */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x' */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z' */
    {0x00,0x08,0x36,0x41,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|' */
    {0x00,0x41,0x36,0x08,0x00}, /* '}' */
    {0x10,0x08,0x08,0x10,0x08}  /* '~' */
};

static inline void ptex_put(uint32_t *buf, int stride, int x, int y, uint32_t col)
{
    if ((unsigned int)x < PHONE_TEX_W && (unsigned int)y < PHONE_TEX_H) {
        buf[y * stride + x] = col;
    }
}

static void ptex_rect(uint32_t *buf, int stride, int x, int y, int w, int h, uint32_t col)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > PHONE_TEX_W) ? PHONE_TEX_W : (x + w);
    int y1 = (y + h > PHONE_TEX_H) ? PHONE_TEX_H : (y + h);
    for (int py = y0; py < y1; py++) {
        uint32_t *row = buf + py * stride;
        for (int px = x0; px < x1; px++) {
            row[px] = col;
        }
    }
}

static void ptex_rounded_rect(uint32_t *buf, int stride, int x, int y, int w, int h, int r, uint32_t col)
{
    if (r <= 0) {
        ptex_rect(buf, stride, x, y, w, h, col);
        return;
    }
    int r2 = r * r;
    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if ((unsigned int)py >= PHONE_TEX_H) continue;
        int cy_dist = 0;
        if (dy < r) cy_dist = r - 1 - dy;
        else if (dy >= h - r) cy_dist = dy - (h - r);
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            if ((unsigned int)px >= PHONE_TEX_W) continue;
            if (cy_dist > 0) {
                int cx_dist = 0;
                if (dx < r) cx_dist = r - 1 - dx;
                else if (dx >= w - r) cx_dist = dx - (w - r);
                if (cx_dist > 0 && (cx_dist * cx_dist + cy_dist * cy_dist > r2)) {
                    continue;
                }
            }
            buf[py * stride + px] = col;
        }
    }
}

static void ptex_circle(uint32_t *buf, int stride, int cx, int cy, int r, uint32_t col)
{
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r2) {
                ptex_put(buf, stride, cx + dx, cy + dy, col);
            }
        }
    }
}

static void ptex_ring(uint32_t *buf, int stride, int cx, int cy, int r_out, int r_in, uint32_t col)
{
    int ro2 = r_out * r_out;
    int ri2 = r_in * r_in;
    for (int dy = -r_out; dy <= r_out; dy++) {
        for (int dx = -r_out; dx <= r_out; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= ro2 && d2 >= ri2) {
                ptex_put(buf, stride, cx + dx, cy + dy, col);
            }
        }
    }
}

static char ptex_ascii_char(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    if (*p < 0x80) {
        (*pp)++;
        return (char)*p;
    }
    if (*p == 0xC3 && p[1]) {
        unsigned char c2 = p[1];
        *pp += 2;
        if (c2 == 0xA1 || c2 == 0x81) return 'a';
        if (c2 == 0xA9 || c2 == 0x89) return 'e';
        if (c2 == 0xAD || c2 == 0x8D) return 'i';
        if (c2 == 0xB3 || c2 == 0x93) return 'o';
        if (c2 == 0xBA || c2 == 0x9A || c2 == 0xBC) return 'u';
        if (c2 == 0xB1) return 'n';
        if (c2 == 0x91) return 'N';
        return ' ';
    }
    while (**pp && (**pp & 0xC0) == 0x80) (*pp)++;
    if (**pp) (*pp)++;
    return ' ';
}

static void ptex_text(uint32_t *buf, int stride, int x, int y, int scale, uint32_t col, const char *str)
{
    if (!str) return;
    int cur_x = x;
    const unsigned char *p = (const unsigned char *)str;
    uint32_t shadow = RGBA8(0, 0, 0, 170);
    while (*p) {
        char ch = ptex_ascii_char(&p);
        if (ch < 32 || ch > 126) ch = ' ';
        const unsigned char *glyph = s_font5x7[(int)ch - 32];
        for (int gx = 0; gx < 5; gx++) {
            unsigned char col_bits = glyph[gx];
            for (int gy = 0; gy < 7; gy++) {
                if (col_bits & (1 << gy)) {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            ptex_put(buf, stride, cur_x + gx * scale + sx + 1, y + gy * scale + sy + 1, shadow);
                            ptex_put(buf, stride, cur_x + gx * scale + sx, y + gy * scale + sy, col);
                        }
                    }
                }
            }
        }
        cur_x += 6 * scale;
    }
}

static void rebuild_phone_ui_texture(const YoutubeVideo *v)
{
    if (!s_phone_ui_tex) {
        vita2d_wait_rendering_done();
        s_phone_ui_tex = vita2d_create_empty_texture(PHONE_TEX_W, PHONE_TEX_H);
        vita2d_wait_rendering_done();
        if (!s_phone_ui_tex) return;
    }

    uint32_t *buf = (uint32_t *)vita2d_texture_get_datap(s_phone_ui_tex);
    int stride = (int)(vita2d_texture_get_stride(s_phone_ui_tex) / 4);
    memset(buf, 0, (size_t)stride * PHONE_TEX_H * sizeof(uint32_t));

    /* 1. Degradado superior oscuro + Cabecera "Shorts" (sin barra de estado de telefono) */
    for (int y = 0; y < 56; y++) {
        unsigned int a = (unsigned int)((56 - y) * 3);
        if (a > 175) a = 175;
        ptex_rect(buf, stride, 0, y, PHONE_TEX_W, 1, RGBA8(8, 8, 10, a));
    }

    /* Titulo "Shorts" arriba a la izquierda + Iconos de altavoz, lupa y menu a la derecha */
    ptex_text(buf, stride, 12, 16, 2, RGBA8(255, 255, 255, 255), "Shorts");

    /* Altavoz (x=182, y=22) */
    ptex_rect(buf, stride, 178, 20, 3, 5, RGBA8(255, 255, 255, 255));
    ptex_rect(buf, stride, 181, 18, 2, 9, RGBA8(255, 255, 255, 255));
    if (shorts_muted) {
        ptex_text(buf, stride, 185, 19, 1, RGBA8(255, 75, 75, 255), "X");
    } else {
        ptex_rect(buf, stride, 185, 20, 1, 5, RGBA8(255, 255, 255, 255));
        ptex_rect(buf, stride, 187, 18, 1, 9, RGBA8(255, 255, 255, 255));
    }

    /* Lupa Buscar (x=216, y=22) */
    ptex_ring(buf, stride, 215, 21, 5, 3, RGBA8(255, 255, 255, 255));
    ptex_rect(buf, stride, 219, 25, 3, 3, RGBA8(255, 255, 255, 255));

    /* 3 puntos (x=248, y=22) */
    ptex_circle(buf, stride, 244, 22, 1, RGBA8(255, 255, 255, 255));
    ptex_circle(buf, stride, 249, 22, 1, RGBA8(255, 255, 255, 255));
    ptex_circle(buf, stride, 254, 22, 1, RGBA8(255, 255, 255, 255));

    /* 2. Pildoras superiores: "Suscripciones" y "Transmisiones en vivo" */
    ptex_rounded_rect(buf, stride, 12, 48, 114, 20, 10, RGBA8(22, 22, 24, 165));
    ptex_rect(buf, stride, 22, 54, 8, 8, RGBA8(255, 255, 255, 240));
    ptex_text(buf, stride, 34, 55, 1, RGBA8(255, 255, 255, 255), "Suscripciones");

    ptex_rounded_rect(buf, stride, 134, 48, 128, 20, 10, RGBA8(22, 22, 24, 165));
    ptex_circle(buf, stride, 145, 58, 2, RGBA8(255, 255, 255, 255));
    ptex_ring(buf, stride, 145, 58, 5, 4, RGBA8(255, 255, 255, 220));
    ptex_text(buf, stride, 154, 55, 1, RGBA8(255, 255, 255, 255), "Transmisiones");

    /* 3. Degradado inferior para legibilidad del canal y titulo */
    for (int y = 375; y < 452; y++) {
        unsigned int a = (unsigned int)((y - 375) * 2 + 30);
        if (a > 195) a = 195;
        ptex_rect(buf, stride, 0, y, PHONE_TEX_W, 1, RGBA8(8, 8, 10, a));
    }

    /* 4. Columna derecha de iconos estilo Smartphone (Corazon, Comentarios, Guardar, Compartir, Remix, Avatar) */
    int rx = 246;

    /* 4.1 Corazon / Like (y=222) */
    uint32_t lk_col = (player_like_state == 1) ? RGBA8(255, 40, 65, 255) : RGBA8(255, 255, 255, 255);
    if (shorts_focus_btn == SHORTS_BTN_LIKE) {
        ptex_ring(buf, stride, rx, 222, 13, 11, RGBA8(255, 255, 255, 220));
    }
    ptex_circle(buf, stride, rx - 3, 219, 4, lk_col);
    ptex_circle(buf, stride, rx + 3, 219, 4, lk_col);
    for (int p = 0; p < 7; p++) {
        ptex_rect(buf, stride, rx - 6 + p, 220 + p, 13 - p * 2, 1, lk_col);
    }
    unsigned long like_cnt = (v && v->views > 0 ? (v->views / 12 + 120) : 27000) + (player_like_state == 1 ? 1 : 0);
    char lk_str[20];
    format_views(like_cnt, lk_str, sizeof(lk_str));
    ptex_text(buf, stride, rx - (int)strlen(lk_str) * 3, 234, 1, RGBA8(255, 255, 255, 255), lk_str);

    /* 4.2 Comentarios (y=266) */
    if (shorts_focus_btn == SHORTS_BTN_COMMENTS) {
        ptex_ring(buf, stride, rx, 266, 13, 11, RGBA8(255, 255, 255, 220));
    }
    ptex_rounded_rect(buf, stride, rx - 7, 260, 14, 10, 2, RGBA8(255, 255, 255, 255));
    ptex_rect(buf, stride, rx - 4, 263, 8, 1, RGBA8(25, 25, 25, 255));
    ptex_rect(buf, stride, rx - 4, 266, 6, 1, RGBA8(25, 25, 25, 255));
    char cm_str[16];
    snprintf(cm_str, sizeof(cm_str), "%u", shorts_comment_count > 0 ? (shorts_comment_count * 23) : 138);
    ptex_text(buf, stride, rx - (int)strlen(cm_str) * 3, 278, 1, RGBA8(255, 255, 255, 255), cm_str);

    /* 4.3 Guardar / Bookmark (y=310) */
    if (shorts_focus_btn == SHORTS_BTN_DISLIKE) {
        ptex_ring(buf, stride, rx, 310, 13, 11, RGBA8(255, 255, 255, 220));
    }
    ptex_rect(buf, stride, rx - 5, 303, 10, 12, RGBA8(255, 255, 255, 255));
    ptex_rect(buf, stride, rx - 3, 305, 6, 7, RGBA8(25, 25, 25, 200));
    ptex_text(buf, stride, rx - 21, 322, 1, RGBA8(255, 255, 255, 255), "Guardar");

    /* 4.4 Compartir (y=354) */
    if (shorts_focus_btn == SHORTS_BTN_SHARE) {
        ptex_ring(buf, stride, rx, 354, 13, 11, RGBA8(255, 255, 255, 220));
    }
    ptex_rect(buf, stride, rx - 5, 352, 8, 3, RGBA8(255, 255, 255, 255));
    ptex_rect(buf, stride, rx + 1, 349, 4, 9, RGBA8(255, 255, 255, 255));
    ptex_text(buf, stride, rx - 26, 366, 1, RGBA8(255, 255, 255, 255), "Compartir");

    /* 4.5 Remix (y=396) */
    ptex_ring(buf, stride, rx, 396, 7, 5, RGBA8(255, 255, 255, 255));
    ptex_text(buf, stride, rx - 15, 408, 1, RGBA8(255, 255, 255, 255), "Remix");

    /* 4.6 Insignia cuadrada del canal abajo a la derecha (x=235, y=424, 22x22) */
    const char *raw_ch = (player_video_author[0]) ? player_video_author : (v && v->channel[0] ? v->channel : "YouTube");
    char ch_init[2] = { (char)toupper((unsigned char)(raw_ch[0] == '@' && raw_ch[1] ? raw_ch[1] : raw_ch[0])), '\0' };
    ptex_rounded_rect(buf, stride, rx - 11, 422, 22, 22, 4, RGBA8(245, 245, 245, 255));
    if (player_author_avatar_tex) {
        int aw = (int)vita2d_texture_get_width(player_author_avatar_tex);
        int ah = (int)vita2d_texture_get_height(player_author_avatar_tex);
        int astride = (int)(vita2d_texture_get_stride(player_author_avatar_tex) / 4);
        const uint32_t *abuf = (const uint32_t *)vita2d_texture_get_datap(player_author_avatar_tex);
        if (abuf && aw > 0 && ah > 0) {
            for (int dy = 0; dy < 18; dy++) {
                int sy = (dy * ah) / 18;
                for (int dx = 0; dx < 18; dx++) {
                    int sx = (dx * aw) / 18;
                    ptex_put(buf, stride, rx - 9 + dx, 424 + dy, abuf[sy * astride + sx] | 0xFF000000u);
                }
            }
        }
    } else {
        ptex_rounded_rect(buf, stride, rx - 9, 424, 18, 18, 3, RGBA8(45, 95, 175, 255));
        ptex_text(buf, stride, rx - 4, 427, 1, RGBA8(255, 255, 255, 255), ch_init);
    }

    /* 5. Bloque Inferior Izquierdo: Avatar + @canal + Pildora Blanca "Suscribirse" + Titulo */
    char handle_str[24];
    if (raw_ch[0] == '@') {
        shorten_title(raw_ch, handle_str, 15);
    } else {
        char tmp_h[20];
        shorten_title(raw_ch, tmp_h, 13);
        for (int i = 0; tmp_h[i]; i++) {
            if (tmp_h[i] == ' ') tmp_h[i] = '_';
            else tmp_h[i] = (char)tolower((unsigned char)tmp_h[i]);
        }
        snprintf(handle_str, sizeof(handle_str), "@%s", tmp_h);
    }

    if (player_author_avatar_tex) {
        int aw = (int)vita2d_texture_get_width(player_author_avatar_tex);
        int ah = (int)vita2d_texture_get_height(player_author_avatar_tex);
        int astride = (int)(vita2d_texture_get_stride(player_author_avatar_tex) / 4);
        const uint32_t *abuf = (const uint32_t *)vita2d_texture_get_datap(player_author_avatar_tex);
        if (abuf && aw > 0 && ah > 0) {
            for (int dy = -9; dy <= 9; dy++) {
                for (int dx = -9; dx <= 9; dx++) {
                    if (dx * dx + dy * dy <= 81) {
                        int sx = ((dx + 9) * aw) / 19;
                        int sy = ((dy + 9) * ah) / 19;
                        ptex_put(buf, stride, 22 + dx, 408 + dy, abuf[sy * astride + sx] | 0xFF000000u);
                    }
                }
            }
        }
    } else {
        ptex_circle(buf, stride, 22, 408, 9, RGBA8(65, 115, 195, 255));
        ptex_text(buf, stride, 19, 405, 1, RGBA8(255, 255, 255, 255), ch_init);
    }
    ptex_text(buf, stride, 36, 405, 1, RGBA8(255, 255, 255, 255), handle_str);

    int sub_x = 36 + (int)strlen(handle_str) * 6 + 6;
    if (sub_x > 132) sub_x = 132;
    uint32_t sub_bg = player_is_subscribed ? RGBA8(48, 48, 54, 245) : RGBA8(250, 250, 250, 255);
    uint32_t sub_fg = player_is_subscribed ? RGBA8(240, 240, 245, 255) : RGBA8(15, 15, 15, 255);
    if (shorts_focus_btn == SHORTS_BTN_SUBSCRIBE) {
        ptex_rounded_rect(buf, stride, sub_x - 2, 397, 78, 22, 11, RGBA8(255, 35, 55, 255));
    }
    ptex_rounded_rect(buf, stride, sub_x, 399, 74, 18, 9, sub_bg);
    ptex_text(buf, stride, sub_x + (player_is_subscribed ? 13 : 4), 405, 1, sub_fg,
              player_is_subscribed ? "Suscrito" : "Suscribirse");

    const char *full_title = (v && v->title[0]) ? v->title : "YouTube Short";
    char t_l1[36] = {0};
    char t_l2[36] = {0};
    size_t tlen = strlen(full_title);
    if (tlen <= 34) {
        snprintf(t_l1, sizeof(t_l1), "%s", full_title);
    } else {
        size_t sp = 34;
        while (sp > 16 && full_title[sp] != ' ') sp--;
        if (full_title[sp] != ' ') sp = 34;
        memcpy(t_l1, full_title, sp);
        t_l1[sp] = '\0';
        const char *rem = full_title + sp;
        while (*rem == ' ') rem++;
        shorten_title(rem, t_l2, 34);
    }
    ptex_text(buf, stride, 12, 426, 1, RGBA8(255, 255, 255, 255), t_l1);
    if (t_l2[0]) {
        ptex_text(buf, stride, 12, 437, 1, RGBA8(255, 255, 255, 255), t_l2);
    }

    /* 6. Barra de navegacion inferior del telefono (Home, Shorts, +, Perfil) */
    ptex_rect(buf, stride, 0, 452, PHONE_TEX_W, 28, RGBA8(10, 10, 12, 252));
    /* Icono Home (x=38, y=464) */
    ptex_rect(buf, stride, 33, 462, 10, 7, RGBA8(200, 200, 205, 255));
    ptex_rect(buf, stride, 35, 459, 6, 3, RGBA8(200, 200, 205, 255));
    /* Icono Shorts activo (x=102, y=464) */
    ptex_rounded_rect(buf, stride, 97, 457, 10, 13, 3, RGBA8(255, 255, 255, 255));
    ptex_rect(buf, stride, 100, 461, 4, 5, RGBA8(15, 15, 15, 255));
    /* Icono (+) Crear (x=168, y=464) */
    ptex_ring(buf, stride, 168, 463, 7, 5, RGBA8(235, 235, 240, 255));
    ptex_rect(buf, stride, 165, 463, 7, 1, RGBA8(235, 235, 240, 255));
    ptex_rect(buf, stride, 168, 460, 1, 7, RGBA8(235, 235, 240, 255));
    /* Icono Perfil (x=234, y=464) */
    ptex_circle(buf, stride, 234, 463, 7, RGBA8(220, 85, 20, 255));
    ptex_text(buf, stride, 231, 460, 1, RGBA8(255, 255, 255, 255),
              youtube_is_logged_in() ? "U" : "A");
    /* Barra gestual inferior del telefono */
    ptex_rounded_rect(buf, stride, 92, 476, 88, 2, 1, RGBA8(170, 170, 175, 220));

    /* 7. Hoja de comentarios superpuesta si esta abierta */
    if (shorts_show_comments) {
        ptex_rounded_rect(buf, stride, 6, 150, PHONE_TEX_W - 12, 296, 8, RGBA8(20, 20, 24, 248));
        ptex_text(buf, stride, 16, 160, 1, RGBA8(255, 255, 255, 255), "Comentarios ([O] Cerrar)");
        ptex_rect(buf, stride, 14, 172, PHONE_TEX_W - 28, 1, RGBA8(60, 60, 70, 255));
        for (unsigned int i = 0; i < 4 && (shorts_comment_scroll + i) < shorts_comment_count; i++) {
            const YoutubeComment *c = &shorts_comments[shorts_comment_scroll + i];
            int cy = 180 + (int)i * 64;
            ptex_rounded_rect(buf, stride, 12, cy, PHONE_TEX_W - 24, 56, 5, RGBA8(32, 32, 38, 255));
            char a_line[36];
            shorten_title(c->author[0] ? c->author : "@usuario", a_line, sizeof(a_line));
            ptex_text(buf, stride, 18, cy + 6, 1, RGBA8(180, 195, 235, 255), a_line);
            char c_l1[38] = {0}, c_l2[38] = {0};
            shorten_title(c->text, c_l1, 36);
            if (strlen(c->text) > 34) {
                shorten_title(c->text + 34, c_l2, 36);
            }
            ptex_text(buf, stride, 18, cy + 20, 1, RGBA8(245, 245, 250, 255), c_l1);
            if (c_l2[0]) {
                ptex_text(buf, stride, 18, cy + 32, 1, RGBA8(225, 225, 235, 255), c_l2);
            }
        }
    }

    s_phone_ui_dirty = 0;
}

static void draw_shorts_phone_view(const YoutubeVideo *v, float prog)
{
    /* 1. Dibujar el video rotado 90 grados ocupando toda la pantalla 960x544 (544x960 en vertical) */
    video_player_draw_rotated(shorts_phone_clockwise);

    /* 2. Actualizar textura de la interfaz de telefono solo cuando cambia el estado */
    if (s_phone_ui_dirty || !s_phone_ui_tex) {
        rebuild_phone_ui_texture(v);
    }

    /* 3. Dibujar la capa de interfaz de telefono rotada 90 grados en 1 solo quad de GPU */
    if (s_phone_ui_tex) {
        float angle = shorts_phone_clockwise ? 1.5707963f : -1.5707963f;
        vita2d_draw_texture_scale_rotate_hotspot(
            s_phone_ui_tex,
            480.0f, 272.0f,
            2.0f, 2.0f,
            angle,
            136.0f, 240.0f
        );
    }

    /* 4. Dibujar la barra de progreso roja en tiempo real sobre el borde de la barra inferior (py=902) */
    float fill_len = 544.0f * prog;
    if (!shorts_phone_clockwise) {
        /* angle = -90: py=902 -> vx=902, px in [0..544] -> vy in [544..0] */
        vita2d_draw_rectangle(901.0f, 0.0f, 3.0f, 544.0f, RGBA8(80, 80, 85, 190));
        if (fill_len > 0.0f) {
            vita2d_draw_rectangle(901.0f, 544.0f - fill_len, 3.0f, fill_len, RGBA8(255, 15, 35, 255));
            vita2d_draw_fill_circle(902.5f, 544.0f - fill_len, 5.5f, RGBA8(255, 20, 40, 255));
        }
    } else {
        /* angle = +90: py=902 -> vx=960-902=58, px in [0..544] -> vy in [0..544] */
        vita2d_draw_rectangle(56.0f, 0.0f, 3.0f, 544.0f, RGBA8(80, 80, 85, 190));
        if (fill_len > 0.0f) {
            vita2d_draw_rectangle(56.0f, 0.0f, 3.0f, fill_len, RGBA8(255, 15, 35, 255));
            vita2d_draw_fill_circle(57.5f, fill_len, 5.5f, RGBA8(255, 20, 40, 255));
        }
    }
}

static void draw_shorts_view(void)
{
    if (shorts_phone_mode && online_video_count > 0) {
        unsigned int idx_p = shorts_index < online_video_count ? shorts_index : 0;
        const YoutubeVideo *vp = &online_videos[idx_p];
        uint64_t cur_s = video_player_get_current_time() / 1000;
        uint64_t dur_m = video_player_get_duration();
        unsigned int tot_s = (dur_m > 0) ? (unsigned int)(dur_m / 1000) : (vp->duration > 0 ? vp->duration : 45);
        float pr = (tot_s > 0) ? ((float)cur_s / (float)tot_s) : 0.0f;
        if (pr > 1.0f) pr = 1.0f;
        draw_shorts_phone_view(vp, pr);
        return;
    }

    /* 1. Barra lateral a la izquierda para poder cambiar de seccion en cualquier momento */
    vita2d_draw_fill_circle(28.0f, 26.0f, 13.0f, RGBA8(220, 85, 20, 255));
    draw_text(23.0f, 32.0f, RGBA8(255, 255, 255, 255), 0.65f,
              youtube_is_logged_in() && youtube_username()[0] ? "U" : "A");

    for (unsigned int i = 0; i < SIDEBAR_ITEMS; ++i) {
        float y = (i == 5) ? 485.0f : (85.0f + (float)i * 62.0f);
        unsigned int is_item_active = ((sidebar_active && i == selected_sidebar) ||
                                       (!sidebar_active && i == active_view));
        if (sidebar_active && i == selected_sidebar) {
            draw_rounded_rectangle(6.0f, y - 16.0f, 44.0f, 32.0f, 16.0f, RGBA8(255, 255, 255, 45));
        }
        unsigned int icon_color = is_item_active ? RGBA8(255, 255, 255, 255) : RGBA8(140, 140, 140, 180);
        draw_sidebar_icon(i, y, icon_color);
    }

    if (online_video_count == 0) {
        draw_text(380.0f, 265.0f, RGBA8(235, 238, 245, 255), 0.85f,
                  startup_worker_state == 1 ? "Cargando Shorts..." : "No se pudieron cargar los Shorts");
        return;
    }

    unsigned int idx = shorts_index < online_video_count ? shorts_index : 0;
    const YoutubeVideo *v = &online_videos[idx];
    vita2d_texture *thumb = online_thumbnails[idx];

    /* 2. Marco Vertical Central 9:16 del Short (x=340, y=10, w=290, h=518) */
    const float vx = 340.0f;
    const float vy = 10.0f;
    const float vw = 290.0f;
    const float vh = 518.0f;

    draw_rounded_rectangle(vx, vy, vw, vh, 14.0f, RGBA8(24, 24, 24, 255));

    VideoPlayerStatus st = video_player_get_status();
    if (video_player_has_frame() && st != VPLAYER_ERROR) {
        video_player_draw((int)vx, (int)vy, (int)vw, (int)vh);
    } else if (thumb) {
        float tw = (float)vita2d_texture_get_width(thumb);
        float th = (float)vita2d_texture_get_height(thumb);
        if (tw > 0.0f && th > 0.0f) {
            vita2d_draw_texture_scale(thumb, vx, vy, vw / tw, vh / th);
        }
    }

    /* Redondear las 4 esquinas del marco vertical 9:16 con el color de fondo (#0f0f0f) */
    draw_rounded_corner_mask(vx, vy, vw, vh, 14.0f, RGBA8(15, 15, 15, 255));

    if (!sidebar_active && !shorts_show_comments && shorts_focus_btn == SHORTS_BTN_PLAY) {
        draw_rounded_outline(vx - 2.0f, vy - 2.0f, vw + 4.0f, vh + 4.0f, 15.0f, 2.0f,
                             RGBA8(255, 255, 255, 220));
    }

    /* 3. Controles superiores dentro del marco vertical (Play/Pausa, Volumen, Menu, Expandir) */
    float top_cy = vy + 26.0f;
    /* Play / Pausa (arriba-izquierda) */
    float play_cx = vx + 26.0f;
    vita2d_draw_fill_circle(play_cx, top_cy, 15.0f, RGBA8(20, 20, 20, 165));
    if (st == VPLAYER_PAUSED) {
        for (int p = 0; p <= 8; p++) {
            vita2d_draw_line(play_cx - 3.0f + (float)p, top_cy - 5.5f + (float)p * 0.68f,
                             play_cx - 3.0f + (float)p, top_cy + 5.5f - (float)p * 0.68f,
                             RGBA8(255, 255, 255, 255));
        }
    } else {
        vita2d_draw_rectangle(play_cx - 4.5f, top_cy - 5.5f, 3.0f, 11.0f, RGBA8(255, 255, 255, 255));
        vita2d_draw_rectangle(play_cx + 1.5f, top_cy - 5.5f, 3.0f, 11.0f, RGBA8(255, 255, 255, 255));
    }

    /* Volumen / Mute (arriba-izquierda 2) */
    float vol_cx = vx + 62.0f;
    vita2d_draw_fill_circle(vol_cx, top_cy, 15.0f, RGBA8(20, 20, 20, 165));
    vita2d_draw_rectangle(vol_cx - 6.0f, top_cy - 3.0f, 4.0f, 6.0f, RGBA8(255, 255, 255, 255));
    for (int p = 0; p <= 4; p++) {
        vita2d_draw_line(vol_cx - 2.0f + (float)p, top_cy - 3.0f - (float)p,
                         vol_cx - 2.0f + (float)p, top_cy + 3.0f + (float)p,
                         RGBA8(255, 255, 255, 255));
    }
    if (shorts_muted) {
        vita2d_draw_line(vol_cx + 3.0f, top_cy - 4.0f, vol_cx + 8.0f, top_cy + 4.0f, RGBA8(255, 70, 70, 255));
        vita2d_draw_line(vol_cx + 3.0f, top_cy + 4.0f, vol_cx + 8.0f, top_cy - 4.0f, RGBA8(255, 70, 70, 255));
    } else {
        vita2d_draw_line(vol_cx + 4.5f, top_cy - 3.5f, vol_cx + 6.0f, top_cy, RGBA8(255, 255, 255, 255));
        vita2d_draw_line(vol_cx + 6.0f, top_cy, vol_cx + 4.5f, top_cy + 3.5f, RGBA8(255, 255, 255, 255));
    }

    /* Menu 3 puntos (arriba-derecha 1) */
    float more_cx = vx + vw - 62.0f;
    vita2d_draw_fill_circle(more_cx, top_cy, 15.0f, RGBA8(20, 20, 20, 165));
    vita2d_draw_fill_circle(more_cx, top_cy - 5.0f, 1.6f, RGBA8(255, 255, 255, 255));
    vita2d_draw_fill_circle(more_cx, top_cy, 1.6f, RGBA8(255, 255, 255, 255));
    vita2d_draw_fill_circle(more_cx, top_cy + 5.0f, 1.6f, RGBA8(255, 255, 255, 255));

    /* Expandir (arriba-derecha 2) */
    float exp_cx = vx + vw - 26.0f;
    vita2d_draw_fill_circle(exp_cx, top_cy, 15.0f, RGBA8(20, 20, 20, 165));
    vita2d_draw_line(exp_cx - 4.5f, top_cy + 4.5f, exp_cx + 4.5f, top_cy - 4.5f, RGBA8(255, 255, 255, 255));
    vita2d_draw_line(exp_cx + 1.0f, top_cy - 4.5f, exp_cx + 4.5f, top_cy - 4.5f, RGBA8(255, 255, 255, 255));
    vita2d_draw_line(exp_cx + 4.5f, top_cy - 4.5f, exp_cx + 4.5f, top_cy - 1.0f, RGBA8(255, 255, 255, 255));
    vita2d_draw_line(exp_cx - 4.5f, top_cy + 1.0f, exp_cx - 4.5f, top_cy + 4.5f, RGBA8(255, 255, 255, 255));
    vita2d_draw_line(exp_cx - 4.5f, top_cy + 4.5f, exp_cx - 1.0f, top_cy + 4.5f, RGBA8(255, 255, 255, 255));

    /* Indicador de estado de carga o error sobre el Short */
    if (player_error_msg[0] != '\0' || st == VPLAYER_ERROR) {
        draw_rounded_rectangle(vx + 16.0f, vy + 210.0f, vw - 32.0f, 92.0f, 10.0f, RGBA8(18, 18, 22, 240));
        draw_rounded_outline(vx + 16.0f, vy + 210.0f, vw - 32.0f, 92.0f, 10.0f, 1.5f, RGBA8(220, 45, 55, 255));
        draw_text(vx + 32.0f, vy + 242.0f, RGBA8(255, 255, 255, 255), 0.60f, "Error al cargar el Short");
        draw_text(vx + 32.0f, vy + 278.0f, RGBA8(200, 205, 215, 255), 0.50f, "[X] Reintentar  [Abajo] Siguiente");
    } else if (player_state == 1 || st == VPLAYER_BUFFERING) {
        draw_rounded_rectangle(vx + 55.0f, vy + 240.0f, 180.0f, 36.0f, 18.0f, RGBA8(15, 15, 15, 210));
        draw_text(vx + 76.0f, vy + 264.0f, RGBA8(245, 245, 250, 255), 0.56f, "Cargando Short...");
    }

    /* Barra de progreso roja en el borde inferior del marco vertical */
    uint64_t cur_sec = video_player_get_current_time() / 1000;
    uint64_t dur_ms = video_player_get_duration();
    unsigned int total_sec = (dur_ms > 0) ? (unsigned int)(dur_ms / 1000) : (v->duration > 0 ? v->duration : 45);
    float prog = (total_sec > 0) ? ((float)cur_sec / (float)total_sec) : 0.0f;
    if (prog > 1.0f) prog = 1.0f;
    vita2d_draw_rectangle(vx + 8.0f, vy + vh - 4.0f, vw - 16.0f, 3.0f, RGBA8(90, 90, 90, 170));
    if (prog > 0.0f) {
        vita2d_draw_rectangle(vx + 8.0f, vy + vh - 4.0f, (vw - 16.0f) * prog, 3.0f, RGBA8(255, 15, 35, 255));
    }

    /* 4. Bloque Inferior Izquierdo: Avatar + @canal + Pildora Suscribirse + Titulo + Pildora Audio */
    const char *raw_ch = (player_video_author[0]) ? player_video_author : (v->channel[0] ? v->channel : "YouTube");
    char handle_str[32];
    if (raw_ch[0] == '@') {
        shorten_title(raw_ch, handle_str, 16);
    } else {
        char tmp_h[28];
        shorten_title(raw_ch, tmp_h, 14);
        /* Quitar espacios para estilo @handle */
        for (int i = 0; tmp_h[i]; i++) {
            if (tmp_h[i] == ' ') tmp_h[i] = '_';
            else tmp_h[i] = (char)tolower((unsigned char)tmp_h[i]);
        }
        snprintf(handle_str, sizeof(handle_str), "@%s", tmp_h);
    }

    float info_x = 62.0f;
    float av_cx = info_x + 14.0f;
    float av_cy = 418.0f;
    char ch_init[2] = { (char)toupper((unsigned char)(raw_ch[0] == '@' && raw_ch[1] ? raw_ch[1] : raw_ch[0])), '\0' };
    if (player_author_avatar_tex) {
        float tw = (float)vita2d_texture_get_width(player_author_avatar_tex);
        float th = (float)vita2d_texture_get_height(player_author_avatar_tex);
        if (tw > 0.0f && th > 0.0f) {
            vita2d_draw_texture_scale(player_author_avatar_tex, av_cx - 13.0f, av_cy - 13.0f,
                                      26.0f / tw, 26.0f / th);
            draw_rounded_corner_mask(av_cx - 13.0f, av_cy - 13.0f, 26.0f, 26.0f, 13.0f,
                                     RGBA8(15, 15, 15, 255));
        }
    } else {
        vita2d_draw_fill_circle(av_cx, av_cy, 13.0f, RGBA8(65, 115, 195, 255));
        draw_text(av_cx - 4.5f, av_cy + 5.0f, RGBA8(255, 255, 255, 255), 0.56f, ch_init);
    }

    draw_text(info_x + 32.0f, av_cy + 5.0f, RGBA8(255, 255, 255, 255), 0.54f, handle_str);

    /* Boton Pildora "Suscribirse" / "Suscrito" */
    float sub_x = 204.0f;
    float sub_y = 403.0f;
    float sub_w = 104.0f;
    float sub_h = 28.0f;
    int sub_focused = (!sidebar_active && !shorts_show_comments && shorts_focus_btn == SHORTS_BTN_SUBSCRIBE);
    unsigned int sub_bg = player_is_subscribed ? RGBA8(45, 45, 52, 255) : RGBA8(245, 245, 245, 255);
    unsigned int sub_fg = player_is_subscribed ? RGBA8(240, 240, 245, 255) : RGBA8(15, 15, 15, 255);
    draw_rounded_rectangle(sub_x, sub_y, sub_w, sub_h, 14.0f, sub_bg);
    if (sub_focused) {
        draw_rounded_outline(sub_x - 2.0f, sub_y - 2.0f, sub_w + 4.0f, sub_h + 4.0f, 16.0f, 2.5f,
                             RGBA8(255, 35, 55, 255));
    }
    draw_text(player_is_subscribed ? (sub_x + 24.0f) : (sub_x + 15.0f),
              sub_y + 19.0f, sub_fg, 0.52f,
              player_is_subscribed ? "Suscrito" : "Suscribirse");

    /* Titulo del Short en 2 lineas blancas en negrita */
    const char *full_title = v->title[0] ? v->title : "YouTube Short";
    char line1[38] = {0};
    char line2[38] = {0};
    size_t tlen = strlen(full_title);
    if (tlen <= 32) {
        snprintf(line1, sizeof(line1), "%s", full_title);
    } else {
        size_t split = 32;
        while (split > 16 && full_title[split] != ' ') split--;
        if (full_title[split] != ' ') split = 32;
        memcpy(line1, full_title, split);
        line1[split] = '\0';
        const char *rem = full_title + split;
        while (*rem == ' ') rem++;
        shorten_title(rem, line2, 34);
    }
    draw_title_with_emojis(info_x, 452.0f, RGBA8(255, 255, 255, 255), 0.58f, line1);
    if (line2[0]) {
        draw_title_with_emojis(info_x, 472.0f, RGBA8(255, 255, 255, 255), 0.58f, line2);
    }

    /* Pildora oscura inferior con icono Play y descripcion/hashtags */
    float pill_y = 486.0f;
    draw_rounded_rectangle(info_x, pill_y, 264.0f, 28.0f, 8.0f, RGBA8(32, 32, 32, 235));
    for (int p = 0; p <= 6; p++) {
        vita2d_draw_line(info_x + 10.0f + (float)p, pill_y + 10.0f + (float)p * 0.65f,
                         info_x + 10.0f + (float)p, pill_y + 18.0f - (float)p * 0.65f,
                         RGBA8(255, 255, 255, 255));
    }
    char tag_line[42];
    snprintf(tag_line, sizeof(tag_line), "%s #shorts #viral", raw_ch);
    char short_tag[34];
    shorten_title(tag_line, short_tag, sizeof(short_tag));
    draw_text(info_x + 22.0f, pill_y + 19.0f, RGBA8(225, 225, 225, 255), 0.48f, short_tag);

    /* 5. Columna Vertical Derecha de Acciones (Like, Dislike, Comentarios, Compartir, Avatar) */
    const float act_cx = 656.0f;
    const float act_r  = 21.0f;

    /* 5.1 Boton Like */
    float lk_cy = 220.0f;
    int lk_foc = (!sidebar_active && !shorts_show_comments && shorts_focus_btn == SHORTS_BTN_LIKE);
    vita2d_draw_fill_circle(act_cx, lk_cy, act_r,
                            lk_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 235));
    unsigned int lk_col = (player_like_state == 1)
        ? RGBA8(255, 35, 65, 255)
        : (lk_foc ? RGBA8(15, 15, 15, 255) : RGBA8(245, 245, 245, 255));
    /* Icono Corazon / Like */
    vita2d_draw_fill_circle(act_cx - 3.8f, lk_cy - 2.5f, 4.2f, lk_col);
    vita2d_draw_fill_circle(act_cx + 3.8f, lk_cy - 2.5f, 4.2f, lk_col);
    for (int p = 0; p <= 8; p++) {
        vita2d_draw_line(act_cx - 7.5f + (float)p, lk_cy - 1.0f + (float)p * 1.05f,
                         act_cx + 7.5f - (float)p, lk_cy - 1.0f + (float)p * 1.05f, lk_col);
    }
    unsigned long like_cnt = (v->views > 0 ? (v->views / 12 + 120) : 37000) + (player_like_state == 1 ? 1 : 0);
    char like_str[24];
    format_views(like_cnt, like_str, sizeof(like_str));
    float lw = font ? (float)vita2d_pgf_text_width(font, 0.48f, like_str) : 28.0f;
    draw_text(act_cx - lw * 0.5f, lk_cy + 35.0f, RGBA8(235, 235, 235, 255), 0.48f, like_str);

    /* 5.2 Boton Dislike */
    float dk_cy = 290.0f;
    int dk_foc = (!sidebar_active && !shorts_show_comments && shorts_focus_btn == SHORTS_BTN_DISLIKE);
    vita2d_draw_fill_circle(act_cx, dk_cy, act_r,
                            dk_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 235));
    unsigned int dk_col = (player_like_state == -1)
        ? RGBA8(255, 65, 85, 255)
        : (dk_foc ? RGBA8(15, 15, 15, 255) : RGBA8(245, 245, 245, 255));
    vita2d_draw_rectangle(act_cx - 6.0f, dk_cy - 6.0f, 11.0f, 8.0f, dk_col);
    vita2d_draw_rectangle(act_cx - 6.0f, dk_cy + 1.5f, 3.5f, 6.0f, dk_col);
    draw_text(act_cx - 26.0f, dk_cy + 35.0f, RGBA8(225, 225, 225, 255), 0.44f, "No me gusta");

    /* 5.3 Boton Comentarios */
    float cm_cy = 360.0f;
    int cm_foc = (!sidebar_active && !shorts_show_comments && shorts_focus_btn == SHORTS_BTN_COMMENTS);
    vita2d_draw_fill_circle(act_cx, cm_cy, act_r,
                            cm_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 235));
    unsigned int cm_col = cm_foc ? RGBA8(15, 15, 15, 255) : RGBA8(245, 245, 245, 255);
    draw_rounded_rectangle(act_cx - 7.5f, cm_cy - 6.0f, 15.0f, 11.0f, 2.5f, cm_col);
    vita2d_draw_line(act_cx - 4.0f, cm_cy + 5.0f, act_cx - 7.5f, cm_cy + 8.0f, cm_col);
    unsigned int bg_line_col = cm_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 235);
    vita2d_draw_rectangle(act_cx - 4.5f, cm_cy - 3.0f, 9.0f, 1.5f, bg_line_col);
    vita2d_draw_rectangle(act_cx - 4.5f, cm_cy + 0.5f, 6.5f, 1.5f, bg_line_col);
    char cm_cnt_str[24];
    snprintf(cm_cnt_str, sizeof(cm_cnt_str), "%u",
             shorts_comment_count > 0 ? (shorts_comment_count * 142) : 1704);
    float cw = font ? (float)vita2d_pgf_text_width(font, 0.48f, cm_cnt_str) : 28.0f;
    draw_text(act_cx - cw * 0.5f, cm_cy + 35.0f, RGBA8(235, 235, 235, 255), 0.48f, cm_cnt_str);

    /* 5.4 Boton Compartir / Pantalla Completa */
    float sh_cy = 430.0f;
    int sh_foc = (!sidebar_active && !shorts_show_comments && shorts_focus_btn == SHORTS_BTN_SHARE);
    vita2d_draw_fill_circle(act_cx, sh_cy, act_r,
                            sh_foc ? RGBA8(255, 255, 255, 255) : RGBA8(38, 38, 38, 235));
    unsigned int sh_col = sh_foc ? RGBA8(15, 15, 15, 255) : RGBA8(245, 245, 245, 255);
    vita2d_draw_line(act_cx - 6.0f, sh_cy + 4.0f, act_cx + 5.0f, sh_cy - 4.0f, sh_col);
    vita2d_draw_line(act_cx + 1.0f, sh_cy - 5.0f, act_cx + 6.0f, sh_cy - 4.0f, sh_col);
    vita2d_draw_line(act_cx + 6.0f, sh_cy - 4.0f, act_cx + 3.0f, sh_cy + 2.0f, sh_col);
    draw_text(act_cx - 24.0f, sh_cy + 35.0f, RGBA8(225, 225, 225, 255), 0.46f, "Compartir");

    /* 5.5 Insignia cuadrada del canal abajo de la columna */
    if (player_author_avatar_tex) {
        float tw = (float)vita2d_texture_get_width(player_author_avatar_tex);
        float th = (float)vita2d_texture_get_height(player_author_avatar_tex);
        if (tw > 0.0f && th > 0.0f) {
            vita2d_draw_texture_scale(player_author_avatar_tex, act_cx - 17.0f, 478.0f, 34.0f / tw, 34.0f / th);
            draw_rounded_corner_mask(act_cx - 17.0f, 478.0f, 34.0f, 34.0f, 8.0f, RGBA8(15, 15, 15, 255));
        }
    } else {
        draw_rounded_rectangle(act_cx - 17.0f, 478.0f, 34.0f, 34.0f, 8.0f, RGBA8(45, 95, 175, 255));
        draw_text(act_cx - 5.0f, 501.0f, RGBA8(255, 255, 255, 255), 0.64f, ch_init);
    }
    draw_rounded_outline(act_cx - 17.0f, 478.0f, 34.0f, 34.0f, 8.0f, 1.5f, RGBA8(220, 220, 220, 200));

    /* 6. Boton Circular Derecho con Flecha Abajo (↓ Siguiente Short) o Panel de Comentarios */
    if (shorts_show_comments) {
        float px = 638.0f, py = 18.0f, pw = 308.0f, ph = 508.0f;
        draw_rounded_rectangle(px, py, pw, ph, 14.0f, RGBA8(22, 22, 26, 250));
        draw_rounded_outline(px, py, pw, ph, 14.0f, 1.5f, RGBA8(65, 65, 75, 255));

        draw_text(px + 18.0f, py + 32.0f, RGBA8(255, 255, 255, 255), 0.68f, "Comentarios");
        draw_text(px + pw - 86.0f, py + 32.0f, RGBA8(180, 180, 190, 255), 0.48f, "[O] Cerrar");
        vita2d_draw_rectangle(px + 14.0f, py + 44.0f, pw - 28.0f, 1.0f, RGBA8(55, 55, 65, 255));

        if (shorts_meta_state == 1 && shorts_comment_count == 0) {
            draw_text(px + 55.0f, py + 240.0f, RGBA8(200, 200, 210, 255), 0.58f, "Cargando comentarios...");
        } else {
            unsigned int max_vis = 4;
            for (unsigned int i = 0; i < max_vis && (shorts_comment_scroll + i) < shorts_comment_count; i++) {
                const YoutubeComment *c = &shorts_comments[shorts_comment_scroll + i];
                float cy = py + 60.0f + (float)i * 104.0f;

                draw_rounded_rectangle(px + 12.0f, cy, pw - 24.0f, 94.0f, 10.0f, RGBA8(32, 32, 38, 255));
                vita2d_draw_fill_circle(px + 28.0f, cy + 20.0f, 10.0f, RGBA8(120, 75, 165, 255));

                char auth_short[28];
                shorten_title(c->author[0] ? c->author : "@usuario", auth_short, sizeof(auth_short));
                draw_text(px + 44.0f, cy + 24.0f, RGBA8(200, 205, 220, 255), 0.50f, auth_short);

                char c_l1[38] = {0}, c_l2[38] = {0};
                size_t clen = strlen(c->text);
                if (clen <= 34) {
                    snprintf(c_l1, sizeof(c_l1), "%s", c->text);
                } else {
                    size_t sp = 34;
                    while (sp > 16 && c->text[sp] != ' ') sp--;
                    if (c->text[sp] != ' ') sp = 34;
                    memcpy(c_l1, c->text, sp);
                    c_l1[sp] = '\0';
                    const char *r = c->text + sp;
                    while (*r == ' ') r++;
                    shorten_title(r, c_l2, 34);
                }
                draw_title_with_emojis(px + 20.0f, cy + 48.0f, RGBA8(245, 245, 250, 255), 0.52f, c_l1);
                if (c_l2[0]) {
                    draw_title_with_emojis(px + 20.0f, cy + 68.0f, RGBA8(245, 245, 250, 255), 0.52f, c_l2);
                }

                char lk_badge[32];
                snprintf(lk_badge, sizeof(lk_badge), "Likes: %s", c->likes[0] ? c->likes : "12");
                draw_text(px + 20.0f, cy + 86.0f, RGBA8(150, 155, 165, 255), 0.44f, lk_badge);
            }
        }
        draw_text(px + 22.0f, py + ph - 12.0f, RGBA8(150, 150, 160, 255), 0.44f,
                  "Arriba/Abajo: Mas comentarios   O / X: Cerrar");
    } else {
        float nav_cx = 912.0f;
        float nav_cy = 278.0f;
        vita2d_draw_fill_circle(nav_cx, nav_cy, 22.0f, RGBA8(38, 38, 38, 235));
        /* Flecha hacia abajo (↓) */
        vita2d_draw_line(nav_cx, nav_cy - 8.0f, nav_cx, nav_cy + 8.0f, RGBA8(255, 255, 255, 255));
        vita2d_draw_line(nav_cx + 1.0f, nav_cy - 8.0f, nav_cx + 1.0f, nav_cy + 8.0f, RGBA8(255, 255, 255, 255));
        vita2d_draw_line(nav_cx - 6.0f, nav_cy + 2.0f, nav_cx, nav_cy + 8.0f, RGBA8(255, 255, 255, 255));
        vita2d_draw_line(nav_cx + 6.0f, nav_cy + 2.0f, nav_cx, nav_cy + 8.0f, RGBA8(255, 255, 255, 255));
    }
}

void home_draw(void)
{
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(15, 15, 15, 255));

    if (startup_worker_state == 1 && online_video_count == 0 && loaded_feed_view < 0) {
        static const char *loading_text[] = {
            "Cargando.", "Cargando..", "Cargando..."
        };
        unsigned int dot = (startup_frame / 8) % 3;

        vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(15, 15, 15, 255));

        /* Logo oficial VitaTube (icono píldora roja con triángulo blanco + texto VitaTube) */
        float icon_w = 70.0f;
        float icon_h = 48.0f;
        float gap = 16.0f;
        float text_scale = 1.35f;
        float text_w = font ? (float)vita2d_pgf_text_width(font, text_scale, "VitaTube") : 160.0f;
        float total_w = icon_w + gap + text_w;

        float start_x = (960.0f - total_w) * 0.5f;
        float icon_y = 220.0f;

        /* Píldora roja redondeada */
        draw_rounded_rectangle(start_x, icon_y, icon_w, icon_h, 12.0f, RGBA8(255, 0, 0, 255));

        /* Triángulo blanco de reproducción dentro de la píldora */
        float tri_cx = start_x + icon_w * 0.5f + 1.5f;
        float tri_cy = icon_y + icon_h * 0.5f;
        for (int p = 0; p <= 14; p++) {
            float th = (float)(14 - p) * 0.72f;
            vita2d_draw_line(tri_cx - 6.0f + (float)p, tri_cy - th,
                             tri_cx - 6.0f + (float)p, tri_cy + th,
                             RGBA8(255, 255, 255, 255));
        }

        /* Texto VitaTube alineado horizontal y verticalmente con la píldora */
        draw_text(start_x + icon_w + gap, icon_y + icon_h * 0.5f + 13.0f,
                  RGBA8(255, 255, 255, 255), text_scale, "VitaTube");

        /* Texto animado "Cargando..." centrado abajo */
        float load_scale = 0.75f;
        float load_w = font ? (float)vita2d_pgf_text_width(font, load_scale, "Cargando...") : 90.0f;
        float load_x = (960.0f - load_w) * 0.5f;
        draw_text(load_x, icon_y + icon_h + 55.0f, RGBA8(185, 188, 198, 255),
                  load_scale, loading_text[dot]);
        return;
    }

    if (account_view) {
        draw_account_view();
        if (keyboard_active) {
            draw_keyboard();
        }
        return;
    }

    if (keyboard_active && keyboard_target == 0) {
        draw_search_view();
        return;
    }

    if (player_active) {
        draw_player_screen();
        return;
    }

    /* Vista exclusiva vertical para Shorts (active_view == 1) */
    if (active_view == 1 && !search_results_active) {
        draw_shorts_view();
        return;
    }

    /* 1. Header estilo YouTube TV: Avatar + Pildora Buscar + Logo YouTube */
    vita2d_draw_fill_circle(28.0f, 26.0f, 13.0f, RGBA8(220, 85, 20, 255));
    draw_text(23.0f, 32.0f, RGBA8(255, 255, 255, 255), 0.65f,
              youtube_is_logged_in() && youtube_username()[0] ? "U" : "A");

    unsigned int pill_bg = keyboard_active ? RGBA8(245, 245, 245, 255) : RGBA8(38, 38, 38, 255);
    unsigned int pill_fg = keyboard_active ? RGBA8(15, 15, 15, 255) : RGBA8(210, 210, 210, 255);
    draw_rounded_rectangle(52.0f, 10.0f, 210.0f, 32.0f, 16.0f, pill_bg);

    vita2d_draw_fill_circle(70.0f, 25.0f, 4.5f, pill_fg);
    vita2d_draw_fill_circle(70.0f, 25.0f, 3.2f, pill_bg);
    vita2d_draw_line(73.5f, 28.5f, 77.5f, 32.5f, pill_fg);
    draw_text(85.0f, 32.0f, pill_fg, 0.65f,
              search_label[0] != 0 ? search_label : "Buscar");

    float yt_x = 810.0f;
    float yt_y = 16.0f;
    draw_rounded_rectangle(yt_x, yt_y, 26.0f, 18.0f, 5.0f, RGBA8(255, 0, 0, 255));
    for (int p = 0; p <= 6; p++) {
        vita2d_draw_line(yt_x + 9.0f + (float)p, yt_y + 5.0f + (float)p * 0.7f,
                         yt_x + 9.0f + (float)p, yt_y + 13.0f - (float)p * 0.7f,
                         RGBA8(255, 255, 255, 255));
    }
    draw_text(yt_x + 32.0f, yt_y + 14.0f, RGBA8(255, 255, 255, 255), 0.72f, "VitaTube");

    /* 2. Barra lateral (Sidebar): Iconos minimalistas limpios y espaciados */
    for (unsigned int i = 0; i < SIDEBAR_ITEMS; ++i) {
        float y = (i == 5) ? 485.0f : (85.0f + (float)i * 62.0f);
        unsigned int is_item_active = ((sidebar_active && i == selected_sidebar) ||
                                       (!sidebar_active && i == active_view));

        if (sidebar_active && i == selected_sidebar) {
            draw_rounded_rectangle(6.0f, y - 16.0f, 44.0f, 32.0f, 16.0f, RGBA8(255, 255, 255, 35));
        }

        unsigned int icon_color = is_item_active ? RGBA8(255, 255, 255, 255) : RGBA8(160, 160, 160, 190);
        draw_sidebar_icon(i, y, icon_color);
    }

    if (search_results_active) {
        draw_text(76.0f, 74.0f, RGBA8(255, 255, 255, 255), 0.85f,
                  "Resultados de busqueda");
        draw_text(300.0f, 74.0f, RGBA8(170, 170, 170, 255), 0.65f,
                  search_label);
    } else if (active_view == 5) {
        char qual_text[96];
        char auto_text[96];
        snprintf(qual_text, sizeof(qual_text), "Calidad de video                       %s", youtube_quality_label());
        snprintf(auto_text, sizeof(auto_text), "Reproducir automaticamente                 %s", youtube_autoplay() ? "Si" : "No");

        draw_text(76.0f, 74.0f, RGBA8(255, 255, 255, 255), 0.85f,
                  "Configuracion");

        /* Fila 0: Calidad */
        draw_rounded_rectangle(76.0f, 120.0f, 600.0f, 48.0f, 8.0f,
                               selected_setting == 0 ? RGBA8(45, 45, 45, 255) : RGBA8(28, 28, 28, 255));
        if (selected_setting == 0) {
            draw_rounded_outline(74.0f, 118.0f, 604.0f, 52.0f, 10.0f, 2.0f, RGBA8(255, 255, 255, 255));
        }
        draw_text(96.0f, 151.0f, RGBA8(255, 255, 255, 255), 0.78f, qual_text);

        /* Fila 1: Autoplay */
        draw_rounded_rectangle(76.0f, 180.0f, 600.0f, 48.0f, 8.0f,
                               selected_setting == 1 ? RGBA8(45, 45, 45, 255) : RGBA8(28, 28, 28, 255));
        if (selected_setting == 1) {
            draw_rounded_outline(74.0f, 178.0f, 604.0f, 52.0f, 10.0f, 2.0f, RGBA8(255, 255, 255, 255));
        }
        draw_text(96.0f, 211.0f, RGBA8(255, 255, 255, 255), 0.78f, auto_text);

        /* Fila 2: Tema */
        draw_rounded_rectangle(76.0f, 240.0f, 600.0f, 48.0f, 8.0f,
                               selected_setting == 2 ? RGBA8(45, 45, 45, 255) : RGBA8(28, 28, 28, 255));
        if (selected_setting == 2) {
            draw_rounded_outline(74.0f, 238.0f, 604.0f, 52.0f, 10.0f, 2.0f, RGBA8(255, 255, 255, 255));
        }
        draw_text(96.0f, 271.0f, RGBA8(255, 255, 255, 255), 0.78f,
                  "Tema                                      Oscuro");

        /* Fila 3: Cuenta de Google OAuth 2.0 */
        draw_rounded_rectangle(76.0f, 300.0f, 600.0f, 48.0f, 8.0f,
                               selected_setting == 3 ? RGBA8(45, 45, 45, 255) : RGBA8(28, 28, 28, 255));
        if (selected_setting == 3) {
            draw_rounded_outline(74.0f, 298.0f, 604.0f, 52.0f, 10.0f, 2.0f, RGBA8(255, 255, 255, 255));
        }
        draw_text(96.0f, 331.0f, RGBA8(255, 255, 255, 255), 0.78f,
                  youtube_is_logged_in()
                      ? "Cuenta de Google OAuth 2.0                Conectada (X)"
                      : "Cuenta de Google OAuth 2.0                Iniciar sesion (X)");

        draw_text(76.0f, 380.0f, RGBA8(140, 140, 140, 255), 0.65f,
                  "Configuracion guardada automaticamente");
    } else {
        const char *view_title = active_view == 1 ? "Shorts"
            : active_view == 2 ? "Tendencias"
            : active_view == 3 ? "Suscripciones"
            : active_view == 4 ? "Biblioteca"
            : row_titles[0];
        draw_text(76.0f, 74.0f, RGBA8(255, 255, 255, 255), 0.85f, view_title);
        if (active_view == 0) {
            char region_text[32];
            snprintf(region_text, sizeof(region_text), "Region: %s",
                     youtube_region());
            draw_text(740.0f, 74.0f, RGBA8(160, 160, 160, 255), 0.60f,
                      region_text);
        }
    }

    if (active_view != 5) {
        unsigned int total_active = search_results_active ? search_video_count :
            (active_view == 3 ? subs_video_count :
            (active_view == 4 ? history_count() : online_video_count));

        if (active_view == 3 && !search_results_active && !youtube_has_subscriptions()) {
            draw_text(220, 250, RGBA8(235, 238, 245, 255), 0.92f,
                      "Aun no tienes suscripciones ni sesion iniciada");
            draw_text(215, 285, RGBA8(160, 163, 175, 255), 0.70f,
                      "Suscribete en Shorts o presiona X para vincular cuenta");
        } else if (active_view == 3 && !search_results_active && subs_worker_state == 1) {
            draw_text(350, 260, RGBA8(235, 238, 245, 255), 0.95f,
                      "Cargando suscripciones...");
        } else if (active_view == 3 && !search_results_active && subs_worker_state != 1 && subs_video_count == 0) {
            draw_text(240, 260, RGBA8(235, 238, 245, 255), 0.92f,
                      "No hay videos recientes en tus suscripciones");
            draw_text(245, 295, RGBA8(160, 163, 175, 255), 0.70f,
                      "Suscribete a canales en Shorts o vincula Google OAuth");
        } else if (active_view == 4 && !search_results_active && history_count() == 0) {
            draw_text(290, 260, RGBA8(235, 238, 245, 255), 0.95f,
                      "El historial de videos esta vacio");
            draw_text(240, 295, RGBA8(160, 163, 175, 255), 0.72f,
                      "Los videos que reproduzcas apareceran aqui");
        } else {
            if (has_home_shorts_shelf()) {
                for (unsigned int column = 0; column < CARD_COLUMNS; ++column) {
                    draw_card(0, column);
                }
                draw_home_shorts_shelf();
            } else {
                for (unsigned int row = 0; row < CARD_ROWS; ++row) {
                    for (unsigned int column = 0; column < CARD_COLUMNS; ++column) {
                        draw_card(row, column);
                    }
                }
            }

            /* Barra de desplazamiento vertical (Scrollbar) */
            if (total_active > CARD_ROWS * CARD_COLUMNS) {
                const float track_x = 948.0f;
                const float track_y = 88.0f;
                const float track_h = 410.0f;
                float visible_ratio = (float)(CARD_ROWS * CARD_COLUMNS) / (float)total_active;
                float thumb_h = visible_ratio * track_h;
                if (thumb_h < 30.0f) thumb_h = 30.0f;
                float max_offset = (float)(total_active - CARD_ROWS * CARD_COLUMNS);
                float scroll_progress = max_offset > 0.0f ? (float)content_offset / max_offset : 0.0f;
                if (scroll_progress > 1.0f) scroll_progress = 1.0f;
                float thumb_y = track_y + scroll_progress * (track_h - thumb_h);

                draw_rounded_rectangle(track_x, track_y, 4.0f, track_h, 2.0f, RGBA8(30, 30, 30, 200));
                draw_rounded_rectangle(track_x, thumb_y, 4.0f, thumb_h, 2.0f, RGBA8(150, 150, 150, 220));
            }
        }
    }

    if (active_view == 5) {
        draw_text(76, 530, RGBA8(130, 130, 130, 255), 0.52f,
                  "Cruceta: Navegar   X: Cambiar ajuste   TRIANGULO: Volver");
    } else if (active_view == 3 && !search_results_active && !youtube_has_subscriptions()) {
        draw_text(76, 530, RGBA8(130, 130, 130, 255), 0.52f,
                  "X: Iniciar sesion   Cruceta: Navegar   TRIANGULO: Volver");
    } else if (active_view == 4 && !search_results_active) {
        draw_text(76, 530, RGBA8(130, 130, 130, 255), 0.52f,
                  "Cruceta: Navegar   X: Reproducir   CUADRADO: Borrar historial   TRIANGULO: Volver");
    } else {
        draw_text(76, 530, RGBA8(130, 130, 130, 255), 0.52f,
                  "Cruceta: Navegar   X: Seleccionar   SELECT: Buscar   TRIANGULO: Volver");
    }
    draw_text(680, 530, RGBA8(140, 140, 140, 255), 0.52f, status_message);

    if (search_worker_state == 1 || search_worker_state == 3 ||
        search_worker_state == 4 || search_worker_state == 5) {
        static const char *loading_dots[] = {
            "Buscando.", "Buscando..", "Buscando..."
        };
        unsigned int dot = (loading_frame / 8) % 3;
        vita2d_draw_rectangle(335, 235, 290, 70,
                              RGBA8(35, 38, 48, 235));
        draw_text(385, 278, RGBA8(245, 245, 248, 255), 0.82f,
                  search_worker_state == 1
                      ? loading_dots[dot] : "Cargando miniaturas...");
    }

    if (keyboard_active) {
        draw_keyboard();
    }
}

void home_shutdown(void)
{
    log_event("home_shutdown");
    save_recent_searches();
    stop_oauth_worker();
    video_player_close();
    if (suggestions_thread >= 0) {
        suggestions_worker_state = -2;
        sceKernelWaitThreadEnd(suggestions_thread, NULL, NULL);
        sceKernelDeleteThread(suggestions_thread);
        suggestions_thread = -1;
    }
    if (startup_thread >= 0) {
        startup_worker_state = -2;
        sceKernelWaitThreadEnd(startup_thread, NULL, NULL);
        sceKernelDeleteThread(startup_thread);
        startup_thread = -1;
    }
    if (search_thread >= 0) {
        search_worker_state = -2;
        sceKernelWaitThreadEnd(search_thread, NULL, NULL);
        sceKernelDeleteThread(search_thread);
        search_thread = -1;
    }
    if (subs_thread >= 0) {
        subs_worker_state = -2;
        sceKernelWaitThreadEnd(subs_thread, NULL, NULL);
        sceKernelDeleteThread(subs_thread);
        subs_thread = -1;
    }
    if (history_thread >= 0) {
        history_worker_state = -2;
        sceKernelWaitThreadEnd(history_thread, NULL, NULL);
        sceKernelDeleteThread(history_thread);
        history_thread = -1;
    }
    if (shorts_meta_thread >= 0) {
        shorts_meta_state = -2;
        sceKernelWaitThreadEnd(shorts_meta_thread, NULL, NULL);
        sceKernelDeleteThread(shorts_meta_thread);
        shorts_meta_thread = -1;
    }
    if (player_thread >= 0) {
        sceKernelWaitThreadEnd(player_thread, NULL, NULL);
        sceKernelDeleteThread(player_thread);
        player_thread = -1;
    }
    clear_thumbnails(online_thumbnails);
    clear_thumbnails(home_shorts_thumbnails);
    clear_thumbnails(search_thumbnails);
    clear_thumbnails(subs_thumbnails);
    clear_thumbnails(history_thumbnails);
    free_author_avatar_texture();
    if (s_phone_ui_tex) {
        vita2d_free_texture(s_phone_ui_tex);
        s_phone_ui_tex = NULL;
    }
    if (font) {
        vita2d_free_pgf(font);
        font = NULL;
    }
}
