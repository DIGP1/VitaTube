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
#define ONLINE_VIDEO_CAPACITY 48
#define THUMBNAIL_BUFFER_SIZE 262144

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
static int player_show_settings = 0;
static int player_settings_selected = 0;
static uint64_t player_resume_ms = 0;
static int player_is_subscribed = 0;
static int player_like_state = 0;
static unsigned long player_video_views = 0;
static int player_video_is_live = 0;
static YoutubeVideo player_related_videos[12];
static unsigned int player_related_count = 0;
static vita2d_texture *player_related_thumbnails[12];
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
static unsigned int subs_video_count = 0;
static volatile int subs_worker_state = 0;
static int subs_thread = -1;

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

static void start_startup_request(void);
static void start_subs_request(void);

static void log_event(const char *format, ...)
{
    FILE *file;
    va_list args;

    mkdir("ux0:data/VitaTube", 0777);
    file = fopen("ux0:data/VitaTube/vitatube.log", "a");
    if (!file) {
        return;
    }

    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
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

    for (i = 0; i < ONLINE_VIDEO_CAPACITY; ++i) {
        if (textures[i]) {
            vita2d_free_texture(textures[i]);
            textures[i] = NULL;
        }
    }
}

static int search_worker(SceSize args, void *arg)
{
    int result;

    (void)args;
    (void)arg;
    result = youtube_search(search_worker_query, search_response,
                            sizeof(search_response));
    search_worker_state = result > 0 ? 2 : -1;
    while (search_worker_state != -2) {
        if (search_worker_state == 3) {
            int bytes = youtube_download_thumbnail(
                &search_videos[search_thumbnail_index],
                search_thumbnail_buffer, sizeof(search_thumbnail_buffer));
            search_thumbnail_bytes = bytes;
            search_worker_state = bytes > 0 ? 4 : 5;
        } else {
            sceKernelDelayThread(10000);
        }
    }
    return 0;
}

static int startup_worker(SceSize args, void *arg)
{
    int result;

    (void)args;
    (void)arg;
    result = youtube_trending(online_videos, ONLINE_VIDEO_CAPACITY);
    if (result <= 0) {
        startup_worker_state = -1;
        return 0;
    }
    online_video_count = (unsigned int)result;
    log_event("home_trending videos=%u region=%s", online_video_count,
              youtube_region());
    startup_thumbnail_index = 0;
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
        } else {
            sceKernelDelayThread(10000);
        }
    }
    return 0;
}

static void start_startup_request(void)
{
    if (startup_thread >= 0) {
        startup_worker_state = -2;
        sceKernelWaitThreadEnd(startup_thread, NULL, NULL);
        sceKernelDeleteThread(startup_thread);
        startup_thread = -1;
    }
    startup_worker_state = 1;
    startup_frame = 0;
    startup_thread = sceKernelCreateThread("VitaTubeStartup",
                                           startup_worker, 0x10000100,
                                           64 * 1024, 0, 0, NULL);
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
                                          0x10000100, 64 * 1024, 0, 0, NULL);
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
    subs_worker_state = 2;
    log_event("subs_loaded count=%u", subs_video_count);
    return 0;
}

static void start_subs_request(void)
{
    if (!youtube_is_logged_in()) {
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
                                        0x10000100, 64 * 1024, 0, 0, NULL);
    if (subs_thread < 0 ||
        sceKernelStartThread(subs_thread, 0, NULL) < 0) {
        subs_worker_state = -1;
        if (subs_thread >= 0) {
            sceKernelDeleteThread(subs_thread);
            subs_thread = -1;
        }
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
    player_state = 1;
    player_overlay_timer = 240;

    player_thread = sceKernelCreateThread("VitaTubeStreams", player_worker,
                                          0x10000100, 64 * 1024, 0, 0, NULL);
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
    player_video_duration = video->duration;
    player_video_views = video->views;
    player_video_is_live = (video->is_live || video->duration == 0);

    /* En los directos, solicitar 480p por defecto para máxima fluidez y evitar lag */
    if (player_video_is_live) {
        youtube_set_max_quality(480);
    }

    player_url[0] = 0;
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
    player_show_settings = 0;
    player_settings_selected = 0;
    player_resume_ms = 0;

    /* Cargar videos relacionados inmediatamente desde los disponibles en linea */
    player_related_count = 0;
    for (unsigned int o = 0; o < online_video_count && player_related_count < 10; ++o) {
        if (strcmp(online_videos[o].id, video->id) != 0) {
            player_related_videos[player_related_count] = online_videos[o];
            player_related_thumbnails[player_related_count] = online_thumbnails[o];
            player_related_count++;
        }
    }

    player_thread = sceKernelCreateThread("VitaTubeStreams", player_worker,
                                          0x10000100, 64 * 1024, 0, 0, NULL);
    if (player_thread < 0 ||
        sceKernelStartThread(player_thread, 0, NULL) < 0) {
        player_state = -1;
    }
}

static void update_player(void)
{
    if (player_state == 2) {
        player_state = 0;
        status_message = "Reproduciendo video";
        log_event("player_stream video=%s quality=%up",
                  player_video_id, player_quality);
        video_player_open(player_url);
        player_resume_ms = 0;
        video_player_set_volume(player_volume);
        player_overlay_timer = 180;
    } else if (player_state == -1) {
        status_message = "No se pudo obtener el stream del video";
        log_event("player_stream_failed video=%s", player_video_id);
        player_state = 0;
    }
}

static void update_search_loading(void)
{
    if (search_worker_state == 2) {
        clear_thumbnails(search_thumbnails);
        search_video_count = (unsigned int)youtube_parse_videos(
            search_response, search_videos, ONLINE_VIDEO_CAPACITY);
        search_thumbnail_index = 0;
        search_worker_state = search_video_count > 0 ? 3 : 0;
        status_message = "Cargando miniaturas...";
        log_event("youtube_search videos=%u", search_video_count);
    }

    if (search_worker_state == 4) {
        search_thumbnails[search_thumbnail_index] =
            vita2d_load_JPEG_buffer(search_thumbnail_buffer,
                                    search_thumbnail_bytes);
        ++search_thumbnail_index;
        if (search_thumbnail_index >= search_video_count) {
            search_worker_state = 0;
            status_message = "Resultados recibidos de Piped";
            log_event("youtube_search thumbnails_loaded=%u",
                      search_video_count);
        } else {
            search_worker_state = 3;
        }
    } else if (search_worker_state == 5) {
        ++search_thumbnail_index;
        if (search_thumbnail_index >= search_video_count) {
            search_worker_state = 0;
            status_message = "Resultados recibidos de Piped";
        } else {
            search_worker_state = 3;
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
    if (startup_worker_state == 4) {
        vt_log(VT_LOG_DEBUG, "[Inicio] Decodificando miniatura %u bytes=%d",
               startup_thumbnail_index, startup_thumbnail_bytes);
        online_thumbnails[startup_thumbnail_index] =
            vita2d_load_JPEG_buffer(startup_thumbnail_buffer,
                                    startup_thumbnail_bytes);
        vt_log(VT_LOG_DEBUG, "[Inicio] Miniatura %u textura=%s",
               startup_thumbnail_index,
               online_thumbnails[startup_thumbnail_index] ? "ok" : "null");
        ++startup_thumbnail_index;
        if (startup_thumbnail_index >= online_video_count) {
            startup_worker_state = 0;
            status_message = "Selecciona un video con X";
            log_event("home_thumbnails_loaded=%u", online_video_count);
        } else {
            startup_worker_state = 3;
        }
    } else if (startup_worker_state == 5) {
        ++startup_thumbnail_index;
        if (startup_thumbnail_index >= online_video_count) {
            startup_worker_state = 0;
            status_message = "Selecciona un video con X";
        } else {
            startup_worker_state = 3;
        }
    } else if (startup_worker_state == -1) {
        startup_worker_state = 0;
        status_message = "Sin conexion; mostrando inicio local";
        log_event("home_trending failed");
        vt_log(VT_LOG_ERROR, "[Inicio] No se pudo cargar tendencias");
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
    if (r <= 0.0f) {
        vita2d_draw_rectangle(x, y, w, h, color);
        return;
    }
    if (r * 2.0f > w) r = w * 0.5f;
    if (r * 2.0f > h) r = h * 0.5f;

    vita2d_draw_rectangle(x + r, y, w - 2.0f * r, h, color);
    vita2d_draw_rectangle(x, y + r, r, h - 2.0f * r, color);
    vita2d_draw_rectangle(x + w - r, y + r, r, h - 2.0f * r, color);

    vita2d_draw_fill_circle(x + r, y + r, r, color);
    vita2d_draw_fill_circle(x + w - r, y + r, r, color);
    vita2d_draw_fill_circle(x + r, y + h - r, r, color);
    vita2d_draw_fill_circle(x + w - r, y + h - r, r, color);
}

static void draw_rounded_outline(float x, float y, float w, float h, float r, float thickness, unsigned int color)
{
    vita2d_draw_rectangle(x + r, y, w - 2.0f * r, thickness, color);
    vita2d_draw_rectangle(x + r, y + h - thickness, w - 2.0f * r, thickness, color);
    vita2d_draw_rectangle(x, y + r, thickness, h - 2.0f * r, color);
    vita2d_draw_rectangle(x + w - thickness, y + r, thickness, h - 2.0f * r, color);

    const int segs = 6;
    float cx[4] = { x + r, x + w - r, x + w - r, x + r };
    float cy[4] = { y + r, y + r, y + h - r, y + h - r };
    float start_angles[4] = { 3.14159f, 4.71239f, 0.0f, 1.57079f };

    for (int c = 0; c < 4; c++) {
        float prev_x = cx[c] + cosf(start_angles[c]) * r;
        float prev_y = cy[c] + sinf(start_angles[c]) * r;
        for (int s = 1; s <= segs; s++) {
            float a = start_angles[c] + (float)s * (1.570796f / (float)segs);
            float cur_x = cx[c] + cosf(a) * r;
            float cur_y = cy[c] + sinf(a) * r;
            vita2d_draw_line(prev_x, prev_y, cur_x, cur_y, color);
            if (thickness > 1.0f) {
                float cur_x2 = cx[c] + cosf(a) * (r - 1.0f);
                float cur_y2 = cy[c] + sinf(a) * (r - 1.0f);
                vita2d_draw_line(prev_x, prev_y, cur_x2, cur_y2, color);
            }
            prev_x = cur_x;
            prev_y = cur_y;
        }
    }
}

static void draw_rounded_corner_mask(float x, float y, float w, float h, float r, unsigned int bg_color)
{
    float r2 = r * r;
    for (float dy = 0.0f; dy < r; dy += 1.0f) {
        float val = r2 - (r - dy) * (r - dy);
        float dx = val > 0.0f ? r - sqrtf(val) : r;
        if (dx > 0.0f) {
            vita2d_draw_rectangle(x, y + dy, dx, 1.0f, bg_color);
            vita2d_draw_rectangle(x + w - dx, y + dy, dx, 1.0f, bg_color);
            vita2d_draw_rectangle(x, y + h - 1.0f - dy, dx, 1.0f, bg_color);
            vita2d_draw_rectangle(x + w - dx, y + h - 1.0f - dy, dx, 1.0f, bg_color);
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

static void draw_card(unsigned int row, unsigned int column)
{
    const float card_w = 274.0f;
    const float card_h = 154.0f;
    const float x = 76.0f + column * 292.0f;
    const float y = 88.0f + row * 216.0f;
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
        : (active_view == 3 ? "Inicia sesion para ver tus suscripciones"
           : active_view == 4 ? "No hay video en esta posicion"
           : video_titles[row][column]);
    char visible_title[42];
    char duration_text[16] = {0};
    char views_text[24];
    char metadata[96];
    char visible_metadata[64];
    const char *channel = (card_video && card_video->channel[0])
        ? card_video->channel : "Canal desconocido";

    /* Dibujar miniatura con esquinas redondeadas */
    if (!is_history_view && !is_subs_view && video_index < video_count && textures[video_index]) {
        float texture_width = (float)vita2d_texture_get_width(textures[video_index]);
        float texture_height = (float)vita2d_texture_get_height(textures[video_index]);
        float scale_x = texture_width > 0.0f ? card_w / texture_width : 1.0f;
        float scale_y = texture_height > 0.0f ? card_h / texture_height : 1.0f;
        vita2d_draw_texture_scale(textures[video_index], x, y, scale_x, scale_y);
    } else {
        draw_rounded_rectangle(x, y, card_w, card_h, 10.0f, RGBA8(32, 32, 32, 255));
    }

    /* Redondear esquinas de la miniatura usando máscara del color de fondo (#0f0f0f) */
    draw_rounded_corner_mask(x, y, card_w, card_h, 10.0f, RGBA8(15, 15, 15, 255));

    if (is_history_view || is_subs_view || video_index >= video_count || !textures[video_index]) {
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

    /* Jerarquía tipográfica de YouTube TV: Título blanco puro y metadatos gris abajo */
    if (card_video) {
        format_views(card_video->views, views_text, sizeof(views_text));
        if (card_video->is_live) {
            snprintf(metadata, sizeof(metadata), "%s  *  %s espectadores", channel, views_text);
        } else {
            snprintf(metadata, sizeof(metadata), "%s  *  %s vistas", channel, views_text);
        }
    } else {
        snprintf(metadata, sizeof(metadata), "%s", channel);
    }

    shorten_title(title, visible_title, sizeof(visible_title));
    float title_y = y + card_h + 18.0f;
    draw_title_with_emojis(x + 2.0f, title_y, RGBA8(255, 255, 255, 255),
                           selected ? 0.64f : 0.60f, visible_title);

    shorten_title(metadata, visible_metadata, sizeof(visible_metadata));
    float meta_y = title_y + 17.0f;
    draw_text(x + 2.0f, meta_y, RGBA8(170, 170, 170, 255),
              0.50f, visible_metadata);
}

static void draw_account_view(void)
{
    char password_mask[64];
    unsigned int i;

    for (i = 0; i + 1 < sizeof(password_mask) &&
                i < strlen(account_password); ++i) {
        password_mask[i] = '*';
    }
    password_mask[i] = 0;
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(15, 15, 15, 255));
    draw_text(190, 96, RGBA8(245, 245, 248, 255), 1.0f,
              "Cuenta VitaTube");
    draw_text(190, 125, RGBA8(170, 173, 183, 255), 0.7f,
              youtube_is_logged_in() ? "Sesion iniciada" :
              "Cuenta Piped: inicia sesion o registrate");

    vita2d_draw_rectangle(190, 145, 630, 60,
                          account_field == 0
                              ? RGBA8(70, 73, 86, 255)
                              : RGBA8(42, 45, 55, 255));
    draw_text(215, 182, RGBA8(235, 238, 245, 255), 0.78f,
              account_username[0] ? account_username : "Usuario");

    vita2d_draw_rectangle(190, 215, 630, 60,
                          account_field == 1
                              ? RGBA8(70, 73, 86, 255)
                              : RGBA8(42, 45, 55, 255));
    draw_text(215, 252, RGBA8(235, 238, 245, 255), 0.78f,
              account_password[0] ? password_mask : "Contrasena");

    if (account_view == 2) {
        vita2d_draw_rectangle(190, 285, 630, 60,
                              account_field == 2
                                  ? RGBA8(70, 73, 86, 255)
                                  : RGBA8(42, 45, 55, 255));
        draw_text(215, 322, RGBA8(235, 238, 245, 255), 0.75f,
                  account_confirm_password[0] ? "Contrasena confirmada"
                                               : "Confirmar contrasena");
    }

    vita2d_draw_rectangle(190, account_view == 2 ? 365 : 285, 310, 60,
                          account_field == (account_view == 2 ? 3 : 2)
                              ? RGBA8(220, 30, 45, 255)
                              : RGBA8(55, 58, 68, 255));
    draw_text(245, account_view == 2 ? 402 : 322,
              RGBA8(245, 245, 248, 255), 0.75f,
              account_view == 1 ? "INICIAR SESION" : "REGISTRARSE");
    vita2d_draw_rectangle(500, account_view == 2 ? 365 : 285, 320, 60,
                          account_field == (account_view == 2 ? 4 : 3)
                              ? RGBA8(70, 73, 86, 255)
                              : RGBA8(42, 45, 55, 255));
    draw_text(625, account_view == 2 ? 402 : 322,
              RGBA8(235, 238, 245, 255), 0.75f,
              account_view == 1 ? "IR A REGISTRO" : "IR A INICIO");
    draw_text(190, account_view == 2 ? 465 : 385,
              RGBA8(180, 183, 193, 255), 0.7f, account_status);
    draw_text(190, account_view == 2 ? 510 : 430,
              RGBA8(135, 138, 148, 255), 0.62f,
              "X editar campo   UP/DOWN navegar   TRIANGULO volver");
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

void home_init(void)
{
    log_event("home_init");
    font = vita2d_load_default_pgf();
    selected_row = 0;
    selected_column = 0;
    content_offset = 0;
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
    account_view = youtube_is_logged_in() ? 0 : 1;
    account_confirm_password[0] = 0;
    account_field = 0;
    account_status = youtube_is_logged_in()
        ? "Sesion guardada en esta consola"
        : "Inicia sesion o registrate";
    online_video_count = 0;
    search_video_count = 0;
    subs_video_count = 0;
    subs_worker_state = 0;
    anim_tick = 0;
    previous_back_touch_y = -1;
    memset(online_thumbnails, 0, sizeof(online_thumbnails));
    memset(search_thumbnails, 0, sizeof(search_thumbnails));
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
    update_startup_loading();
    update_search_loading();
    update_player();
    video_player_update();
    if (player_active) {
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
        unsigned int account_fields = account_view == 2 ? 5 : 4;
        if (pressed & SCE_CTRL_UP) {
            account_field = account_field == 0
                ? account_fields - 1 : account_field - 1;
        } else if (pressed & SCE_CTRL_DOWN) {
            account_field = (account_field + 1) % account_fields;
        } else if (pressed & SCE_CTRL_CROSS) {
            if (account_field == 0 || account_field == 1) {
                open_account_keyboard(account_field);
            } else if (account_field == (account_view == 2 ? 3 : 2)) {
                submit_account();
            } else {
                account_view = account_view == 1 ? 2 : 1;
            }
        } else if (pressed & SCE_CTRL_TRIANGLE) {
            account_view = 0;
        }

        if (touch && touch->reportNum > 0) {
            int x = touch->report[0].x / 2;
            int y = touch->report[0].y / 2;
            if (x >= 190 && x < 820 && y >= 145 && y < 205) {
                account_field = 0;
                open_account_keyboard(0);
            } else if (x >= 190 && x < 820 && y >= 215 && y < 275) {
                account_field = 1;
                open_account_keyboard(1);
            } else if (account_view == 2 && x >= 190 && x < 820 &&
                       y >= 285 && y < 345) {
                account_field = 2;
                open_account_keyboard(2);
            } else if (x >= 190 && x < 500 &&
                       y >= (account_view == 2 ? 365 : 285) &&
                       y < (account_view == 2 ? 425 : 345)) {
                account_field = account_view == 2 ? 3 : 2;
                submit_account();
            } else if (x >= 500 && x < 820 &&
                       y >= (account_view == 2 ? 365 : 285) &&
                       y < (account_view == 2 ? 425 : 345)) {
                account_view = account_view == 1 ? 2 : 1;
            }
        }
        return;
    }

    if (pressed & SCE_CTRL_SELECT) {
        log_event("search_open_source=select");
        open_search_dialog();
        return;
    }

    if (player_active) {
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
        }

        /* Salir del reproductor con Triangulo o Circulo */
        if (pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE)) {
            video_player_close();
            player_active = 0;
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
                        break;
                    case PLAYER_BTN_SUBS:
                        player_is_subscribed = !player_is_subscribed;
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
                        break;
                    case PLAYER_BTN_DISLIKE:
                        player_like_state = (player_like_state == -1) ? 0 : -1;
                        break;
                    case PLAYER_BTN_COMMENTS:
                        player_show_desc = !player_show_desc;
                        break;
                    case PLAYER_BTN_SAVE:
                        if (player_related_count > 0) {
                            history_add(&player_related_videos[0]);
                        }
                        break;
                    case PLAYER_BTN_THANKS:
                        player_like_state = 1;
                        break;
                    case PLAYER_BTN_SETTINGS:
                        player_show_settings = !player_show_settings;
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
                    if (total_sec > 0) {
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
                    } else if (touch_x >= 76 && touch_x <= 170) {
                        player_show_desc = !player_show_desc;
                    } else if (touch_x >= 178 && touch_x <= 276) {
                        player_is_subscribed = !player_is_subscribed;
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
            active_view = selected_sidebar;
            search_results_active = 0;
            search_label[0] = 0;
            sidebar_active = 0;
            content_offset = 0;
            selected_row = 0;
            selected_column = 0;
            status_message = "Seccion abierta";
            if (active_view == 3) {
                if (youtube_is_logged_in()) {
                    if (subs_video_count == 0 && subs_worker_state == 0) {
                        start_subs_request();
                    }
                } else {
                    status_message = "Inicia sesion para ver suscripciones";
                }
            }
        }
    } else {
        if (active_view == 5) {
            if (pressed & SCE_CTRL_UP) {
                selected_setting = selected_setting == 0 ? 2 : selected_setting - 1;
                audio_play_nav();
            } else if (pressed & SCE_CTRL_DOWN) {
                selected_setting = (selected_setting + 1) % 3;
                audio_play_nav();
            } else if (pressed & SCE_CTRL_LEFT) {
                sidebar_active = 1;
                audio_play_nav();
            }
        } else {
            if (pressed & SCE_CTRL_UP) {
                audio_play_nav();
                if (selected_row == 0 && content_offset >= CARD_COLUMNS) {
                    content_offset -= CARD_COLUMNS;
                } else {
                    selected_row = selected_row == 0 ? CARD_ROWS - 1 : selected_row - 1;
                }
            } else if (pressed & SCE_CTRL_DOWN) {
                audio_play_nav();
                unsigned int total_active = search_results_active ? search_video_count :
                    (active_view == 3 ? subs_video_count :
                    (active_view == 4 ? history_count() : online_video_count));
                if (selected_row == CARD_ROWS - 1 &&
                    content_offset + CARD_ROWS * CARD_COLUMNS < total_active &&
                    content_offset + CARD_ROWS * CARD_COLUMNS + CARD_COLUMNS <= total_active) {
                    content_offset += CARD_COLUMNS;
                } else {
                    selected_row = (selected_row + 1) % CARD_ROWS;
                }
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
                selected_column = (selected_column + 1) % CARD_COLUMNS;
            }
        }
    }

    if ((pressed & SCE_CTRL_SQUARE) && !sidebar_active && active_view == 4 && !search_results_active) {
        history_clear();
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
            }
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
            if (!youtube_is_logged_in()) {
                account_view = 1;
                audio_play_select();
                status_message = "Inicia sesion en Piped";
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
            if (delta_x > 45) {
                sidebar_active = 0;
            } else if (delta_x < -45) {
                sidebar_active = 1;
            } else if (delta_y > 35) {
                if (selected_row == CARD_ROWS - 1 &&
                    (active_view == 0 || search_results_active) &&
                    content_offset + CARD_ROWS * CARD_COLUMNS <
                        online_video_count &&
                    content_offset + CARD_ROWS * CARD_COLUMNS +
                        CARD_COLUMNS <= online_video_count) {
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
        } else if (x < 56 && y >= 60 && y < 515) {
            sidebar_active = 1;
            if (y >= 450) {
                selected_sidebar = 5;
            } else {
                selected_sidebar = (unsigned int)((y - 60) / 62);
                if (selected_sidebar >= 5) selected_sidebar = 4;
            }
            active_view = selected_sidebar;
            search_results_active = 0;
            search_label[0] = 0;
            sidebar_active = 0;
        } else if (x >= 76 && y >= 88 && y < 510) {
            sidebar_active = 0;
            if (active_view == 5) {
                if (y >= 170 && y < 218) {
                    selected_setting = 0;
                    youtube_cycle_quality();
                    youtube_save_config();
                    audio_play_select();
                    status_message = "Calidad actualizada";
                } else if (y >= 230 && y < 278) {
                    selected_setting = 1;
                    youtube_toggle_autoplay();
                    youtube_save_config();
                    audio_play_select();
                    status_message = youtube_autoplay() ? "Autoplay activado" : "Autoplay desactivado";
                } else if (y >= 290 && y < 338) {
                    selected_setting = 2;
                    audio_play_nav();
                    status_message = "Solo tema oscuro disponible";
                }
            } else {
                selected_column = (unsigned int)((x - 76) / 292);
                selected_row = (unsigned int)((y - 88) / 216);
                if (selected_column >= CARD_COLUMNS) selected_column = CARD_COLUMNS - 1;
                if (selected_row >= CARD_ROWS) selected_row = CARD_ROWS - 1;
                audio_play_nav();
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

    /* Mensajes de extraccion de stream o buffering */
    if (player_state == 1) {
        char q_msg[96];
        snprintf(q_msg, sizeof(q_msg), "Cargando stream %s...", youtube_quality_label());
        draw_rounded_rectangle(270.0f, 240.0f, 420.0f, 54.0f, 10.0f, RGBA8(15, 15, 15, 225));
        draw_text(300.0f, 274.0f, RGBA8(250, 250, 255, 255), 0.78f, q_msg);
    } else if (st == VPLAYER_BUFFERING) {
        char buf_info[96];
        snprintf(buf_info, sizeof(buf_info), "Cargando buffer... (%u KB)",
                 video_player_get_buffered_bytes() / 1024);
        draw_rounded_rectangle(270.0f, 240.0f, 420.0f, 54.0f, 10.0f, RGBA8(15, 15, 15, 225));
        draw_text(300.0f, 274.0f, RGBA8(250, 250, 255, 255), 0.75f, buf_info);
    }

    int show_osd = (player_overlay_timer > 0 || st == VPLAYER_PAUSED ||
                    player_state == 1 || st == VPLAYER_BUFFERING ||
                    player_show_desc || player_show_settings);
    if (!show_osd) return;

    /* 2. Degradados / Scrims estilo YouTube TV */
    /* Scrim superior para contraste del titulo */
    for (int y = 0; y < 90; y += 3) {
        float f = 1.0f - ((float)y / 90.0f);
        unsigned char alpha = (unsigned char)(190.0f * f * f);
        vita2d_draw_rectangle(0, y, 960, 3, RGBA8(5, 5, 5, alpha));
    }

    /* Scrim inferior degradado suave */
    for (int y = 230; y < 330; y += 4) {
        float f = ((float)(y - 230) / 100.0f);
        unsigned char alpha = (unsigned char)(210.0f * f * f);
        vita2d_draw_rectangle(0, y, 960, 4, RGBA8(10, 10, 10, alpha));
    }
    /* Fondo oscuro semi-solido detras de controles y carrusel */
    vita2d_draw_rectangle(0, 330, 960, 214, RGBA8(10, 10, 10, 240));

    /* 3. Metadatos del Video (Esquina Superior Izquierda) */
    draw_title_with_emojis(34.0f, 38.0f, RGBA8(255, 255, 255, 255), 0.85f,
                           player_video_title[0] ? player_video_title : "Reproduciendo video");

    char views_str[32];
    format_views(player_video_views, views_str, sizeof(views_str));
    char meta_str[160];
    if (player_video_is_live) {
        snprintf(meta_str, sizeof(meta_str), "%s  *  %s espectadores  *  En vivo",
                 player_video_author[0] ? player_video_author : "VitaTube", views_str);
    } else {
        snprintf(meta_str, sizeof(meta_str), "%s  *  %s vistas",
                 player_video_author[0] ? player_video_author : "VitaTube", views_str);
    }
    draw_text(34.0f, 66.0f, RGBA8(170, 170, 170, 255), 0.54f, meta_str);

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
    if (av_foc) {
        draw_rounded_outline(av_cx - 19.0f, av_cy - 19.0f, 38.0f, 38.0f, 19.0f, 2.5f, RGBA8(255, 255, 255, 255));
    }
    vita2d_draw_fill_circle(av_cx, av_cy, av_r, RGBA8(210, 85, 20, 255));
    char av_init[2] = { player_video_author[0] ? (char)toupper((unsigned char)player_video_author[0]) : 'Y', '\0' };
    draw_text(av_cx - 5.0f, av_cy + 6.0f, RGBA8(255, 255, 255, 255), 0.65f, av_init);

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

        vita2d_texture *t = player_related_thumbnails[c];
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

        const YoutubeVideo *v = &player_related_videos[c];
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
        draw_text(165.0f, 202.0f, RGBA8(180, 180, 180, 255), 0.58f, player_video_author);
        draw_text(165.0f, 230.0f, RGBA8(140, 140, 140, 255), 0.54f, meta_str);
        draw_text(165.0f, 280.0f, RGBA8(200, 200, 200, 255), 0.50f, "[X] o [O] para cerrar");
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

void home_draw(void)
{
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(15, 15, 15, 255));

    if (startup_worker_state != 0) {
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

        draw_text(76.0f, 320.0f, RGBA8(140, 140, 140, 255), 0.65f,
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

        if (active_view == 3 && !search_results_active && !youtube_is_logged_in()) {
            draw_text(250, 250, RGBA8(235, 238, 245, 255), 0.95f,
                      "Inicia sesion para ver tus suscripciones");
            draw_text(270, 285, RGBA8(160, 163, 175, 255), 0.72f,
                      "Presiona X para ir a la pantalla de cuenta");
        } else if (active_view == 3 && !search_results_active && subs_worker_state == 1) {
            draw_text(350, 260, RGBA8(235, 238, 245, 255), 0.95f,
                      "Cargando suscripciones...");
        } else if (active_view == 3 && !search_results_active && subs_worker_state != 1 && subs_video_count == 0) {
            draw_text(240, 260, RGBA8(235, 238, 245, 255), 0.92f,
                      "No hay videos recientes en tus suscripciones");
            draw_text(260, 295, RGBA8(160, 163, 175, 255), 0.70f,
                      "Verifica tus canales suscritos en Piped");
        } else if (active_view == 4 && !search_results_active && history_count() == 0) {
            draw_text(290, 260, RGBA8(235, 238, 245, 255), 0.95f,
                      "El historial de videos esta vacio");
            draw_text(240, 295, RGBA8(160, 163, 175, 255), 0.72f,
                      "Los videos que reproduzcas apareceran aqui");
        } else {
            for (unsigned int row = 0; row < CARD_ROWS; ++row) {
                for (unsigned int column = 0; column < CARD_COLUMNS; ++column) {
                    draw_card(row, column);
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
    } else if (active_view == 3 && !search_results_active && !youtube_is_logged_in()) {
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
    if (player_thread >= 0) {
        sceKernelWaitThreadEnd(player_thread, NULL, NULL);
        sceKernelDeleteThread(player_thread);
        player_thread = -1;
    }
    clear_thumbnails(online_thumbnails);
    clear_thumbnails(search_thumbnails);
    if (font) {
        vita2d_free_pgf(font);
        font = NULL;
    }
}
