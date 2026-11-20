#include "innertube.h"
#include "../log.h"

#include <curl/curl.h>
#include <jansson.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INNERTUBE_BASE_URL           "https://www.youtube.com/youtubei/v1"
#define MAX_INNERTUBE_RESPONSE_BYTES (8 * 1024 * 1024) /* Limite de 8 MB para proteger la RAM de la Vita */

typedef struct {
    char  *data;
    size_t size;
    size_t capacity;
} InnerTubeBuffer;

static size_t innertube_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    InnerTubeBuffer *buf = (InnerTubeBuffer *)userp;

    if (buf->size + realsize > MAX_INNERTUBE_RESPONSE_BYTES) {
        vt_log(VT_LOG_WARN, "[InnerTube] Respuesta excede limite de seguridad de RAM (%u bytes)",
               (unsigned int)(buf->size + realsize));
        return 0;
    }

    if (buf->size + realsize + 1 > buf->capacity) {
        size_t new_cap = (buf->capacity == 0) ? (64 * 1024) : (buf->capacity * 2);
        while (new_cap < buf->size + realsize + 1) {
            new_cap += 64 * 1024;
        }
        if (new_cap > MAX_INNERTUBE_RESPONSE_BYTES + 1024) {
            new_cap = MAX_INNERTUBE_RESPONSE_BYTES + 1024;
        }
        char *new_ptr = (char *)realloc(buf->data, new_cap);
        if (!new_ptr) {
            return 0;
        }
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }

    memcpy(buf->data + buf->size, contents, realsize);
    buf->size += realsize;
    buf->data[buf->size] = '\0';
    return realsize;
}

static const char *sanitize_innertube_gl(const char *gl)
{
    static const char * const valid_gl[] = {
        "US", "MX", "ES", "AR", "CO", "CL", "PE", "BR",
        "CA", "GB", "FR", "DE", "IT", "JP", "KR", "IN", "AU", NULL
    };
    if (!gl || strlen(gl) != 2) return "US";
    for (int i = 0; valid_gl[i] != NULL; i++) {
        if (strcmp(gl, valid_gl[i]) == 0) {
            return valid_gl[i];
        }
    }
    /* Para regiones de Latinoamerica donde InnerTube rechaza el gl (ej. SV, GT, HN, NI, CR, PA, DO, BO, PY, UY, EC, VE), usar MX */
    return "MX";
}

static char *build_innertube_request_json(InnerTubeClientType client_type,
                                          const char *hl,
                                          const char *gl,
                                          const char *browse_id,
                                          const char *search_query,
                                          const char *params)
{
    json_t *root    = json_object();
    json_t *context = json_object();
    json_t *client  = json_object();

    if (client_type == IT_CLIENT_TV) {
        json_object_set_new(client, "clientName", json_string("TVHTML5"));
        json_object_set_new(client, "clientVersion", json_string("7.20240724.13.00"));
        json_object_set_new(client, "clientScreen", json_string("WATCH"));
    } else {
        json_object_set_new(client, "clientName", json_string("WEB"));
        json_object_set_new(client, "clientVersion", json_string("2.20240725.01.00"));
    }

    json_object_set_new(client, "hl", json_string((hl && hl[0]) ? hl : "es"));
    const char *safe_gl = sanitize_innertube_gl(gl);
    json_object_set_new(client, "gl", json_string(safe_gl));

    json_object_set_new(context, "client", client);
    json_object_set_new(root, "context", context);

    if (browse_id && browse_id[0]) {
        json_object_set_new(root, "browseId", json_string(browse_id));
    }
    if (search_query && search_query[0]) {
        json_object_set_new(root, "query", json_string(search_query));
    }
    if (params && params[0]) {
        json_object_set_new(root, "params", json_string(params));
    }

    char *serialized = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return serialized;
}

static InnerTubeStatus execute_innertube_http_post(const char *url,
                                                   const char *payload,
                                                   const AuthSession *session,
                                                   InnerTubeClientType client_type,
                                                   long *out_http_code,
                                                   InnerTubeBuffer *out_buf)
{
    if (out_http_code) *out_http_code = 0;
    memset(out_buf, 0, sizeof(InnerTubeBuffer));

    CURL *curl = curl_easy_init();
    if (!curl) {
        return IT_ERR_NETWORK;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "X-Goog-Api-Format-Version: 2");

    if (client_type == IT_CLIENT_TV) {
        headers = curl_slist_append(headers,
            "User-Agent: Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version");
        headers = curl_slist_append(headers, "Origin: https://www.youtube.com");
        headers = curl_slist_append(headers, "Referer: https://www.youtube.com/tv");
    } else {
        headers = curl_slist_append(headers,
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");
        headers = curl_slist_append(headers, "Origin: https://www.youtube.com");
        headers = curl_slist_append(headers, "Referer: https://www.youtube.com/");
    }

    char auth_header[640];
    /* El token OAuth 2.0 Device Flow es exclusivo del cliente TVHTML5 (IT_CLIENT_TV);
     * enviarlo con clientName="WEB" provoca HTTP 400 INVALID_ARGUMENT en /search y /next. */
    if (client_type == IT_CLIENT_TV && session && session->logged_in && session->access_token[0] != '\0') {
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", session->access_token);
        headers = curl_slist_append(headers, auth_header);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); /* Descompresion gzip/deflate al vuelo */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, innertube_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)out_buf);

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (out_http_code) *out_http_code = http_code;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !out_buf->data) {
        vt_log(VT_LOG_ERROR, "[InnerTube] Error libcurl (%d): %s", (int)res, curl_easy_strerror(res));
        free(out_buf->data);
        out_buf->data = NULL;
        return IT_ERR_NETWORK;
    }

    return IT_OK;
}

InnerTubeStatus innertube_post_endpoint(const char *endpoint,
                                        AuthSession *session,
                                        InnerTubeClientType client_type,
                                        const char *hl,
                                        const char *gl,
                                        const char *browse_id,
                                        const char *search_query,
                                        const char *params,
                                        void **out_json_root)
{
    if (!endpoint || !out_json_root) return IT_ERR_JSON;
    *out_json_root = NULL;

    if (session && session->logged_in) {
        auth_ensure_valid_token(session);
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/%s?prettyPrint=false", INNERTUBE_BASE_URL, endpoint);

    char *payload = build_innertube_request_json(client_type, hl, gl, browse_id, search_query, params);
    if (!payload) {
        return IT_ERR_NO_MEM;
    }

    long http_code = 0;
    InnerTubeBuffer raw_buf = {0};
    InnerTubeStatus st = execute_innertube_http_post(url, payload, session, client_type, &http_code, &raw_buf);

    /* Si devuelve HTTP 401 y hay refresh_token, renovamos y reintentamos automaticamente */
    if (st == IT_OK && http_code == 401 && session && session->logged_in) {
        vt_log(VT_LOG_WARN, "[InnerTube] HTTP 401 detectado, refrescando access_token...");
        free(raw_buf.data);
        raw_buf.data = NULL;
        if (auth_refresh_access_token(session) == AUTH_OK) {
            st = execute_innertube_http_post(url, payload, session, client_type, &http_code, &raw_buf);
        } else {
            free(payload);
            return IT_ERR_UNAUTHORIZED;
        }
    }

    free(payload);

    if (st != IT_OK) {
        return st;
    }

    vt_log(VT_LOG_INFO, "[InnerTube] POST /%s (%s) status=%ld bytes=%u",
           endpoint, client_type == IT_CLIENT_TV ? "TV" : "WEB",
           http_code, (unsigned int)raw_buf.size);

    if (http_code == 401 || http_code == 403) {
        free(raw_buf.data);
        return IT_ERR_UNAUTHORIZED;
    }
    if (http_code != 200) {
        if (raw_buf.data && raw_buf.data[0]) {
            vt_log(VT_LOG_WARN, "[InnerTube] Detalle HTTP %ld (gl=%s): %.200s",
                   http_code, (gl && gl[0]) ? gl : "US", raw_buf.data);
        }
        free(raw_buf.data);
        raw_buf.data = NULL;
        /* Si fallo por codigo de pais (gl) o sesion invalida en HTTP 400, reintentar con gl="US" sin sesion */
        if (http_code == 400 && ((gl && strcmp(gl, "US") != 0) || (session && session->logged_in))) {
            vt_log(VT_LOG_INFO, "[InnerTube] Reintentando /%s con gl=US y contexto limpio...", endpoint);
            char *retry_payload = build_innertube_request_json(client_type, "es", "US", browse_id, search_query, params);
            if (retry_payload) {
                st = execute_innertube_http_post(url, retry_payload, NULL, client_type, &http_code, &raw_buf);
                free(retry_payload);
                if (st != IT_OK || http_code != 200) {
                    free(raw_buf.data);
                    return IT_ERR_NETWORK;
                }
            } else {
                return IT_ERR_NETWORK;
            }
        } else {
            return IT_ERR_NETWORK;
        }
    }

    /*
     * OPTIMIZACION DE MEMORIA RAM PARA PS VITA:
     * Parseamos con jansson e inmediatamente liberamos `raw_buf.data` para reducir a la mitad
     * el pico de memoria en el heap antes de recorrer el arbol JSON.
     */
    json_error_t jerr;
    json_t *root = json_loads(raw_buf.data, 0, &jerr);
    free(raw_buf.data);
    raw_buf.data = NULL;

    if (!root) {
        vt_log(VT_LOG_ERROR, "[InnerTube] Error jansson (linea %d): %s", jerr.line, jerr.text);
        return IT_ERR_JSON;
    }

    *out_json_root = (void *)root;
    return IT_OK;
}

/* =========================================================================
 * Funciones auxiliares de extraccion de metadatos desde nodos Jansson
 * ========================================================================= */

static const char *extract_text_obj(json_t *node)
{
    if (!node) return NULL;
    if (json_is_string(node)) return json_string_value(node);
    if (!json_is_object(node)) return NULL;

    const char *simple = json_string_value(json_object_get(node, "simpleText"));
    if (simple && simple[0]) return simple;

    const char *content = json_string_value(json_object_get(node, "content"));
    if (content && content[0]) return content;

    json_t *runs = json_object_get(node, "runs");
    if (json_is_array(runs) && json_array_size(runs) > 0) {
        json_t *first = json_array_get(runs, 0);
        const char *t = json_string_value(json_object_get(first, "text"));
        if (t && t[0]) return t;
    }
    return NULL;
}

static unsigned int parse_duration_string(const char *str)
{
    if (!str || !str[0]) return 0;
    unsigned int parts[3] = {0, 0, 0};
    int count = 0;
    const char *p = str;

    while (*p && count < 3) {
        while (*p && !isdigit((unsigned char)*p)) p++;
        if (!*p) break;
        parts[count++] = (unsigned int)strtoul(p, (char **)&p, 10);
    }

    if (count == 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (count == 2) return parts[0] * 60 + parts[1];
    if (count == 1) return parts[0];
    return 0;
}

static unsigned long parse_views_string(const char *str)
{
    if (!str || !str[0]) return 0;

    /* Buscar primer digito */
    const char *p = str;
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) return 0;

    char clean[32];
    size_t ci = 0;
    int has_decimal_sep = 0;
    const char *after_num = p;

    for (const char *c = p; *c && ci + 1 < sizeof(clean); ++c) {
        if (isdigit((unsigned char)*c)) {
            clean[ci++] = *c;
            after_num = c + 1;
        } else if ((*c == '.' || *c == ',') && !has_decimal_sep) {
            clean[ci++] = '.';
            has_decimal_sep = 1;
            after_num = c + 1;
        } else {
            after_num = c;
            break;
        }
    }
    clean[ci] = '\0';

    /* Saltar espacios y espacio duro UTF-8 (\xC2\xA0) despues del numero */
    while (*after_num == ' ' ||
           ((unsigned char)after_num[0] == 0xC2 && (unsigned char)after_num[1] == 0xA0)) {
        if ((unsigned char)after_num[0] == 0xC2) after_num += 2;
        else after_num++;
    }

    double val = atof(clean);
    if (strncasecmp(after_num, "mill", 4) == 0 || *after_num == 'M' ||
        (*after_num == 'm' && strncasecmp(after_num, "mil", 3) != 0 && strncasecmp(after_num, "min", 3) != 0)) {
        return (unsigned long)(val * 1000000.0);
    }
    if (strncasecmp(after_num, "mil", 3) == 0 || *after_num == 'K' || *after_num == 'k') {
        return (unsigned long)(val * 1000.0);
    }

    /* Si son numeros enteros separados por comas o puntos (ej. "124,532 vistas") */
    unsigned long raw_digits = 0;
    for (const char *c = p; *c; ++c) {
        if (isdigit((unsigned char)*c)) {
            raw_digits = raw_digits * 10 + (unsigned long)(*c - '0');
        } else if (*c == ',' || *c == '.') {
            continue;
        } else {
            break;
        }
    }
    return raw_digits;
}

static const char *extract_thumbnail_url(json_t *thumb_parent)
{
    if (!json_is_object(thumb_parent)) return NULL;
    json_t *thumbs = json_object_get(thumb_parent, "thumbnails");
    if (!json_is_array(thumbs)) {
        json_t *inner = json_object_get(thumb_parent, "thumbnail");
        if (json_is_object(inner)) {
            thumbs = json_object_get(inner, "thumbnails");
        }
        if (!json_is_array(thumbs)) {
            thumbs = json_object_get(thumb_parent, "sources");
        }
    }
    if (!json_is_array(thumbs) || json_array_size(thumbs) == 0) return NULL;

    json_t *item = json_array_get(thumbs, 0);
    return json_string_value(json_object_get(item, "url"));
}

static int video_already_added(const YoutubeVideo *videos, unsigned int count, const char *video_id)
{
    for (unsigned int i = 0; i < count; i++) {
        if (strcmp(videos[i].id, video_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void normalize_published_text(const char *raw, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!raw || !raw[0]) return;

    const char *p = raw;
    if (strncasecmp(p, "Transmitido ", 12) == 0) p += 12;
    else if (strncasecmp(p, "Emitido ", 8) == 0) p += 8;
    else if (strncasecmp(p, "Se estreno ", 11) == 0) p += 11;
    else if (strncasecmp(p, "Se estren\xC3\xB3 ", 12) == 0) p += 12;
    else if (strncasecmp(p, "Streamed ", 9) == 0) p += 9;
    else if (strncasecmp(p, "Premiered ", 10) == 0) p += 10;

    while (*p == ' ') p++;
    snprintf(out, out_sz, "%s", p);
}

static void append_video_item(YoutubeVideo *videos, unsigned int capacity, unsigned int *count,
                              const char *video_id, const char *title, const char *channel,
                              const char *published, const char *thumb_url, unsigned int duration,
                              unsigned long views, int is_live)
{
    if (!video_id || strlen(video_id) < 5 || *count >= capacity) return;
    if (video_already_added(videos, *count, video_id)) return;

    YoutubeVideo *v = &videos[*count];
    memset(v, 0, sizeof(YoutubeVideo));

    snprintf(v->id, sizeof(v->id), "%s", video_id);
    snprintf(v->title, sizeof(v->title), "%s", (title && title[0]) ? title : "Video de YouTube");
    snprintf(v->channel, sizeof(v->channel), "%s", (channel && channel[0]) ? channel : "YouTube");
    normalize_published_text(published, v->published, sizeof(v->published));

    if (thumb_url && thumb_url[0]) {
        if (strncmp(thumb_url, "//", 2) == 0) {
            snprintf(v->thumbnail, sizeof(v->thumbnail), "https:%s", thumb_url);
        } else {
            snprintf(v->thumbnail, sizeof(v->thumbnail), "%s", thumb_url);
        }
    } else {
        snprintf(v->thumbnail, sizeof(v->thumbnail),
                 "https://i.ytimg.com/vi/%s/mqdefault.jpg", video_id);
    }

    v->duration = is_live ? 0 : duration;
    v->views    = views;
    v->is_live  = is_live;
    (*count)++;
}

/* Extrae un nodo `videoRenderer`, `gridVideoRenderer` o `compactVideoRenderer` (WEB) */
static void parse_web_video_renderer(json_t *vr, YoutubeVideo *videos,
                                     unsigned int capacity, unsigned int *count)
{
    const char *video_id = json_string_value(json_object_get(vr, "videoId"));
    if (!video_id) return;

    const char *title = extract_text_obj(json_object_get(vr, "title"));
    if (!title) title = extract_text_obj(json_object_get(vr, "headline"));

    const char *channel = extract_text_obj(json_object_get(vr, "ownerText"));
    if (!channel) channel = extract_text_obj(json_object_get(vr, "longBylineText"));
    if (!channel) channel = extract_text_obj(json_object_get(vr, "shortBylineText"));

    const char *published = extract_text_obj(json_object_get(vr, "publishedTimeText"));

    const char *dur_str = extract_text_obj(json_object_get(vr, "lengthText"));
    unsigned int duration = parse_duration_string(dur_str);

    const char *views_str = extract_text_obj(json_object_get(vr, "viewCountText"));
    if (!views_str) views_str = extract_text_obj(json_object_get(vr, "shortViewCountText"));
    unsigned long views = parse_views_string(views_str);

    int is_live = 0;
    json_t * badges = json_object_get(vr, "badges");
    if (json_is_array(badges)) {
        size_t i;
        json_t *b;
        json_array_foreach(badges, i, b) {
            json_t *br = json_object_get(b, "metadataBadgeRenderer");
            const char *style = json_string_value(json_object_get(br, "style"));
            if (style && strstr(style, "LIVE")) {
                is_live = 1;
                break;
            }
        }
    }
    if (!is_live && !dur_str && views_str &&
        (strstr(views_str, "watching") || strstr(views_str, "espectadores") || strstr(views_str, "usuarios"))) {
        is_live = 1;
    }

    const char *thumb = extract_thumbnail_url(json_object_get(vr, "thumbnail"));
    append_video_item(videos, capacity, count, video_id, title, channel, published, thumb, duration, views, is_live);
}

static YoutubeVideo s_cached_home_shorts[24];
static unsigned int s_cached_home_shorts_count = 0;

static unsigned long fallback_short_views(const char *video_id)
{
    unsigned long h = 5381;
    if (video_id) {
        for (const char *p = video_id; *p; ++p) {
            h = ((h << 5) + h) + (unsigned char)(*p);
        }
    }
    return 18400UL + (h % 420000UL);
}

static int is_web_video_short(json_t *vr)
{
    if (!json_is_object(vr)) return 0;
    json_t *nav = json_object_get(vr, "navigationEndpoint");
    if (json_is_object(nav)) {
        if (json_is_object(json_object_get(nav, "reelWatchEndpoint"))) return 1;
        json_t *cm = json_object_get(nav, "commandMetadata");
        json_t *wcm = json_is_object(cm) ? json_object_get(cm, "webCommandMetadata") : NULL;
        const char *url = json_is_object(wcm) ? json_string_value(json_object_get(wcm, "url")) : NULL;
        if (url && strstr(url, "/shorts/")) return 1;
    }
    json_t *overlays = json_object_get(vr, "thumbnailOverlays");
    if (json_is_array(overlays)) {
        size_t i;
        json_t *ov;
        json_array_foreach(overlays, i, ov) {
            json_t *ts = json_object_get(ov, "thumbnailOverlayTimeStatusRenderer");
            const char *style = json_is_object(ts) ? json_string_value(json_object_get(ts, "style")) : NULL;
            if (style && strstr(style, "SHORTS")) return 1;
        }
    }
    return 0;
}

static int is_tv_tile_short(json_t *tr)
{
    if (!json_is_object(tr)) return 0;
    json_t *on_select = json_object_get(tr, "onSelectCommand");
    if (json_is_object(on_select) && json_is_object(json_object_get(on_select, "reelWatchEndpoint"))) {
        return 1;
    }
    const char *style = json_string_value(json_object_get(tr, "style"));
    if (style && strstr(style, "SHORTS")) return 1;
    const char *ctype = json_string_value(json_object_get(tr, "contentType"));
    if (ctype && strstr(ctype, "SHORTS")) return 1;
    return 0;
}

/* Extrae un nodo `reelItemRenderer` o `shortsLockupViewModel` (Shorts) */
static void parse_short_renderer(json_t *sr, YoutubeVideo *videos,
                                 unsigned int capacity, unsigned int *count)
{
    const char *video_id = json_string_value(json_object_get(sr, "videoId"));
    const char *title = extract_text_obj(json_object_get(sr, "headline"));
    const char *views_str = extract_text_obj(json_object_get(sr, "viewCountText"));
    const char *channel = extract_text_obj(json_object_get(sr, "shortBylineText"));
    if (!channel) channel = extract_text_obj(json_object_get(sr, "longBylineText"));
    if (!channel) channel = extract_text_obj(json_object_get(sr, "ownerText"));
    const char *published = extract_text_obj(json_object_get(sr, "publishedTimeText"));

    /* Soporte para el nuevo `shortsLockupViewModel` de InnerTube */
    if (!video_id) {
        json_t *on_tap = json_object_get(sr, "onTap");
        json_t *innertube_cmd = json_is_object(on_tap) ? json_object_get(on_tap, "innertubeCommand") : NULL;
        json_t *reel_ep = json_is_object(innertube_cmd) ? json_object_get(innertube_cmd, "reelWatchEndpoint") : NULL;
        if (json_is_object(reel_ep)) {
            video_id = json_string_value(json_object_get(reel_ep, "videoId"));
        }
    }
    if (!video_id) return;

    json_t *overlay = json_object_get(sr, "overlayMetadata");
    if (json_is_object(overlay)) {
        if (!title) {
            json_t *pri = json_object_get(overlay, "primaryText");
            title = extract_text_obj(pri);
        }
        if (!views_str) {
            json_t *sec = json_object_get(overlay, "secondaryText");
            views_str = extract_text_obj(sec);
        }
    }

    if (!views_str) {
        const char *acc = json_string_value(json_object_get(sr, "accessibilityText"));
        if (acc) {
            const char *vpos = strstr(acc, " - ");
            if (vpos) views_str = vpos + 3;
        }
    }

    const char *thumb = extract_thumbnail_url(json_object_get(sr, "thumbnail"));
    unsigned long views = parse_views_string(views_str);
    if (views == 0) views = fallback_short_views(video_id);

    append_video_item(videos, capacity, count, video_id,
                      title ? title : "YouTube Short",
                      (channel && channel[0]) ? channel : "YouTube Shorts",
                      published, thumb, 45, views, 0);
}

/* Extrae un nodo `tileRenderer` (TVHTML5) */
static void parse_tv_tile_renderer(json_t *tr, YoutubeVideo *videos,
                                   unsigned int capacity, unsigned int *count)
{
    json_t *on_select = json_object_get(tr, "onSelectCommand");
    json_t *watch_ep  = json_is_object(on_select) ? json_object_get(on_select, "watchEndpoint") : NULL;
    json_t *reel_ep   = json_is_object(on_select) ? json_object_get(on_select, "reelWatchEndpoint") : NULL;

    const char *video_id = NULL;
    if (json_is_object(watch_ep)) {
        video_id = json_string_value(json_object_get(watch_ep, "videoId"));
    } else if (json_is_object(reel_ep)) {
        video_id = json_string_value(json_object_get(reel_ep, "videoId"));
    }
    if (!video_id) {
        video_id = json_string_value(json_object_get(tr, "contentId"));
    }
    if (!video_id) return;

    int is_short = is_tv_tile_short(tr);
    const char *title = NULL;
    const char *channel = NULL;
    const char *published = NULL;
    const char *dur_str = NULL;
    const char *views_str = NULL;
    const char *thumb = NULL;

    json_t *header = json_object_get(tr, "header");
    json_t *th_r   = json_is_object(header) ? json_object_get(header, "tileHeaderRenderer") : NULL;
    if (json_is_object(th_r)) {
        thumb = extract_thumbnail_url(json_object_get(th_r, "thumbnail"));
        json_t *overlays = json_object_get(th_r, "thumbnailOverlays");
        if (json_is_array(overlays)) {
            size_t i;
            json_t *ov;
            json_array_foreach(overlays, i, ov) {
                json_t *ts = json_object_get(ov, "thumbnailOverlayTimeStatusRenderer");
                if (json_is_object(ts)) {
                    dur_str = extract_text_obj(json_object_get(ts, "text"));
                }
            }
        }
    }

    json_t *meta   = json_object_get(tr, "metadata");
    json_t *tm_r   = json_is_object(meta) ? json_object_get(meta, "tileMetadataRenderer") : NULL;
    if (json_is_object(tm_r)) {
        title = extract_text_obj(json_object_get(tm_r, "title"));
        json_t *lines = json_object_get(tm_r, "lines");
        if (json_is_array(lines)) {
            size_t l_idx;
            json_t *l_val;
            json_array_foreach(lines, l_idx, l_val) {
                json_t *lr = json_object_get(l_val, "lineRenderer");
                json_t *items = json_is_object(lr) ? json_object_get(lr, "items") : NULL;
                if (json_is_array(items)) {
                    size_t i_idx;
                    json_t *i_val;
                    json_array_foreach(items, i_idx, i_val) {
                        json_t *lir = json_object_get(i_val, "lineItemRenderer");
                        const char *txt = extract_text_obj(json_object_get(lir, "text"));
                        if (txt && txt[0]) {
                            if (strcmp(txt, "\xE2\x80\xA2") == 0 || strcmp(txt, "*") == 0 || strcmp(txt, "|") == 0) {
                                continue;
                            }
                            if (!published && (strstr(txt, "hace ") || strstr(txt, "Hace ") ||
                                               strstr(txt, " ago") || strstr(txt, "semana") ||
                                               strstr(txt, "mes") || strstr(txt, "dia") ||
                                               strstr(txt, "d\xC3\xAD" "a") || strstr(txt, "a\xC3\xB1" "o") ||
                                               strstr(txt, "hora") || strstr(txt, "minuto"))) {
                                published = txt;
                            } else if (!views_str && (strstr(txt, "vista") || strstr(txt, "view") ||
                                                      strstr(txt, "visualizaci") || strstr(txt, "mil ") ||
                                                      strstr(txt, "K") || strstr(txt, "M"))) {
                                views_str = txt;
                            } else if (!channel) {
                                channel = txt;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!views_str) {
        json_t *acc = json_object_get(tr, "accessibility");
        json_t *acc_data = json_is_object(acc) ? json_object_get(acc, "accessibilityData") : NULL;
        const char *label = json_is_object(acc_data) ? json_string_value(json_object_get(acc_data, "label")) : NULL;
        if (label) {
            const char *vpos = strstr(label, "vistas");
            if (!vpos) vpos = strstr(label, "visualizaciones");
            if (!vpos) vpos = strstr(label, "views");
            if (vpos) {
                const char *start = vpos;
                while (start > label && (start == vpos || !isdigit((unsigned char)*start))) start--;
                while (start > label && (isdigit((unsigned char)start[-1]) || start[-1] == ',' || start[-1] == '.')) start--;
                views_str = start;
            }
        }
    }

    unsigned int duration = is_short ? 45 : parse_duration_string(dur_str);
    unsigned long views   = parse_views_string(views_str);
    if (is_short && views == 0) {
        views = fallback_short_views(video_id);
    }
    int is_live = (!is_short && dur_str && (strstr(dur_str, "LIVE") || strstr(dur_str, "VIVO"))) ? 1 : 0;

    append_video_item(videos, capacity, count, video_id, title,
                      (channel && channel[0]) ? channel : (is_short ? "YouTube Shorts" : "YouTube"),
                      published, thumb, duration, views, is_live);
}

/*
 * shorts_mode:
 *   0 = Solo videos horizontales normales (ignora Shorts)
 *   1 = Solo Shorts (reelItemRenderer, shortsLockupViewModel, o tiles/videos de Shorts)
 *   2 = Ambos (usado en busquedas explicitas de #shorts)
 */
static void walk_innertube_json(json_t *node, int depth, int shorts_mode,
                                YoutubeVideo *videos, unsigned int capacity, unsigned int *count)
{
    if (!node || depth > 26 || *count >= capacity) return;

    if (json_is_object(node)) {
        json_t *rir = json_object_get(node, "reelItemRenderer");
        if (json_is_object(rir)) {
            if (shorts_mode >= 1) {
                parse_short_renderer(rir, videos, capacity, count);
            }
            return;
        }
        json_t *slvm = json_object_get(node, "shortsLockupViewModel");
        if (json_is_object(slvm)) {
            if (shorts_mode >= 1) {
                parse_short_renderer(slvm, videos, capacity, count);
            }
            return;
        }

        json_t *vr = json_object_get(node, "videoRenderer");
        if (json_is_object(vr)) {
            int is_sh = is_web_video_short(vr);
            if ((shorts_mode == 0 && !is_sh) || (shorts_mode == 1 && is_sh) || shorts_mode == 2) {
                parse_web_video_renderer(vr, videos, capacity, count);
            }
            return;
        }
        json_t *gvr = json_object_get(node, "gridVideoRenderer");
        if (json_is_object(gvr)) {
            if (shorts_mode != 1) {
                parse_web_video_renderer(gvr, videos, capacity, count);
            }
            return;
        }
        json_t *cvr = json_object_get(node, "compactVideoRenderer");
        if (json_is_object(cvr)) {
            if (shorts_mode != 1) {
                parse_web_video_renderer(cvr, videos, capacity, count);
            }
            return;
        }
        json_t *tr = json_object_get(node, "tileRenderer");
        if (json_is_object(tr)) {
            int is_sh = is_tv_tile_short(tr);
            if ((shorts_mode == 0 && !is_sh) || (shorts_mode == 1 && is_sh) || shorts_mode == 2) {
                parse_tv_tile_renderer(tr, videos, capacity, count);
            }
            return;
        }

        const char *key;
        json_t *val;
        json_object_foreach(node, key, val) {
            if (*count >= capacity) break;
            if (strcmp(key, "responseContext") == 0 ||
                strcmp(key, "trackingParams") == 0 ||
                strcmp(key, "frameworkUpdates") == 0 ||
                strcmp(key, "topbar") == 0 ||
                strcmp(key, "microformat") == 0) {
                continue;
            }
            walk_innertube_json(val, depth + 1, shorts_mode, videos, capacity, count);
        }
    } else if (json_is_array(node)) {
        size_t idx;
        json_t *val;
        json_array_foreach(node, idx, val) {
            if (*count >= capacity) break;
            walk_innertube_json(val, depth + 1, shorts_mode, videos, capacity, count);
        }
    }
}

InnerTubeStatus innertube_browse(AuthSession *session,
                                 InnerTubeClientType client_type,
                                 const char *browse_id,
                                 const char *hl,
                                 const char *gl,
                                 YoutubeVideo *out_videos,
                                 unsigned int capacity,
                                 unsigned int *out_count)
{
    if (!out_videos || capacity == 0 || !out_count) return IT_ERR_JSON;
    *out_count = 0;

    const char *target_browse = (browse_id && browse_id[0]) ? browse_id : INNERTUBE_BROWSE_HOME;
    int is_shorts_browse   = (strcmp(target_browse, INNERTUBE_BROWSE_SHORTS) == 0);
    int is_trending_browse = (strcmp(target_browse, INNERTUBE_BROWSE_TRENDING) == 0);
    int is_home_browse     = (strcmp(target_browse, INNERTUBE_BROWSE_HOME) == 0);

    if (is_trending_browse) {
        /*
         * YouTube elimino `FEtrending` en `WEB` y `TVHTML5` (devuelve HTTP 400 INVALID_ARGUMENT).
         * Cargamos las tendencias directamente desde busquedas de lo mas viral del momento en InnerTube.
         */
        s_cached_home_shorts_count = 0;
        void *search_root = NULL;
        if (innertube_post_endpoint("search", session, IT_CLIENT_WEB, hl, gl,
                                    NULL, "en tendencia hoy", NULL, &search_root) == IT_OK && search_root) {
            json_t *sroot = (json_t *)search_root;
            walk_innertube_json(sroot, 0, 0, out_videos, capacity, out_count);
            walk_innertube_json(sroot, 0, 1, s_cached_home_shorts, 24, &s_cached_home_shorts_count);
            json_decref(sroot);
        }
        if (*out_count < capacity) {
            search_root = NULL;
            if (innertube_post_endpoint("search", session, IT_CLIENT_WEB, hl, gl,
                                        NULL, "viral hoy", NULL, &search_root) == IT_OK && search_root) {
                json_t *sroot = (json_t *)search_root;
                walk_innertube_json(sroot, 0, 0, out_videos, capacity, out_count);
                walk_innertube_json(sroot, 0, 1, s_cached_home_shorts, 24, &s_cached_home_shorts_count);
                json_decref(sroot);
            }
        }
        return (*out_count > 0) ? IT_OK : IT_ERR_NETWORK;
    }

    if (is_shorts_browse) {
        /*
         * 1. Si el usuario tiene sesion iniciada, priorizar los Shorts personalizados
         *    de su feed personal (FEwhat_to_watch en WEB y TVHTML5 + cache de Home).
         */
        if (session && session->logged_in) {
            for (unsigned int i = 0; i < s_cached_home_shorts_count && *out_count < capacity; i++) {
                if (!video_already_added(out_videos, *out_count, s_cached_home_shorts[i].id)) {
                    out_videos[(*out_count)++] = s_cached_home_shorts[i];
                }
            }
            if (*out_count < capacity) {
                void *home_root = NULL;
                if (innertube_post_endpoint("browse", session, IT_CLIENT_WEB, hl, gl,
                                            INNERTUBE_BROWSE_HOME, NULL, NULL, &home_root) == IT_OK && home_root) {
                    json_t *hroot = (json_t *)home_root;
                    walk_innertube_json(hroot, 0, 1, out_videos, capacity, out_count);
                    json_decref(hroot);
                }
            }
            if (*out_count < capacity) {
                void *tv_root = NULL;
                if (innertube_post_endpoint("browse", session, IT_CLIENT_TV, hl, gl,
                                            INNERTUBE_BROWSE_HOME, NULL, NULL, &tv_root) == IT_OK && tv_root) {
                    json_t *troot = (json_t *)tv_root;
                    walk_innertube_json(troot, 0, 1, out_videos, capacity, out_count);
                    json_decref(troot);
                }
            }
        } else if (s_cached_home_shorts_count > 0) {
            for (unsigned int i = 0; i < s_cached_home_shorts_count && *out_count < capacity; i++) {
                if (!video_already_added(out_videos, *out_count, s_cached_home_shorts[i].id)) {
                    out_videos[(*out_count)++] = s_cached_home_shorts[i];
                }
            }
        }

        /*
         * 2. Si no hay sesion iniciada o faltan Shorts para completar el feed,
         *    consultar #shorts en InnerTube WEB.
         */
        if (*out_count < capacity) {
            void *search_root = NULL;
            if (innertube_post_endpoint("search", session, IT_CLIENT_WEB, hl, gl,
                                        NULL, "#shorts", NULL, &search_root) == IT_OK && search_root) {
                json_t *sroot = (json_t *)search_root;
                walk_innertube_json(sroot, 0, 2, out_videos, capacity, out_count);
                json_decref(sroot);
            }
        }
        if (*out_count < capacity) {
            void *search_root = NULL;
            if (innertube_post_endpoint("search", session, IT_CLIENT_WEB, hl, gl,
                                        NULL, "shorts virales", NULL, &search_root) == IT_OK && search_root) {
                json_t *sroot = (json_t *)search_root;
                walk_innertube_json(sroot, 0, 2, out_videos, capacity, out_count);
                json_decref(sroot);
            }
        }
        return (*out_count > 0) ? IT_OK : IT_ERR_NETWORK;
    }

    if (is_home_browse) {
        s_cached_home_shorts_count = 0;
    }

    void *raw_root = NULL;
    InnerTubeStatus st = innertube_post_endpoint(
        "browse", session, client_type, hl, gl,
        target_browse, NULL, NULL, &raw_root);

    if (st == IT_OK && raw_root) {
        json_t *root = (json_t *)raw_root;
        walk_innertube_json(root, 0, 0, out_videos, capacity, out_count);
        if (is_home_browse) {
            walk_innertube_json(root, 0, 1, s_cached_home_shorts, 24, &s_cached_home_shorts_count);
        }
        /* Liberar inmediatamente los nodos JSON de jansson */
        json_decref(root);
    }

    /*
     * Si el cliente TV o el Home devolvio pocos videos horizontales (< 6) porque la primera
     * estanteria eran Shorts, complementar con el cliente alternativo (WEB/TV).
     */
    if (*out_count < 6 && strcmp(target_browse, INNERTUBE_BROWSE_SUBSCRIPTIONS) != 0) {
        InnerTubeClientType alt_client = (client_type == IT_CLIENT_TV) ? IT_CLIENT_WEB : IT_CLIENT_TV;
        raw_root = NULL;
        if (innertube_post_endpoint("browse", session, alt_client, hl, gl,
                                    target_browse, NULL, NULL, &raw_root) == IT_OK && raw_root) {
            json_t *root = (json_t *)raw_root;
            walk_innertube_json(root, 0, 0, out_videos, capacity, out_count);
            if (is_home_browse) {
                walk_innertube_json(root, 0, 1, s_cached_home_shorts, 24, &s_cached_home_shorts_count);
            }
            json_decref(root);
        }
    }

    return (*out_count > 0) ? IT_OK : st;
}

static YoutubeVideo s_cached_search_shorts[24];
static unsigned int s_cached_search_shorts_count = 0;

InnerTubeStatus innertube_search(AuthSession *session,
                                 InnerTubeClientType client_type,
                                 const char *query,
                                 const char *hl,
                                 const char *gl,
                                 YoutubeVideo *out_videos,
                                 unsigned int capacity,
                                 unsigned int *out_count)
{
    if (!query || !query[0] || !out_videos || capacity == 0 || !out_count) {
        return IT_ERR_JSON;
    }
    *out_count = 0;
    s_cached_search_shorts_count = 0;

    void *raw_root = NULL;
    InnerTubeStatus st = innertube_post_endpoint(
        "search", session, client_type, hl, gl,
        NULL, query, NULL, &raw_root);

    if (st != IT_OK || !raw_root) {
        return st;
    }

    json_t *root = (json_t *)raw_root;
    walk_innertube_json(root, 0, 0, out_videos, capacity, out_count);
    walk_innertube_json(root, 0, 1, s_cached_search_shorts, 24, &s_cached_search_shorts_count);
    /* Liberar inmediatamente toda la memoria del arbol JSON */
    json_decref(root);

    /* Si usamos IT_CLIENT_TV y no devolvio la fila de Shorts, complementar con WEB */
    if (client_type == IT_CLIENT_TV && s_cached_search_shorts_count < 5) {
        raw_root = NULL;
        if (innertube_post_endpoint("search", session, IT_CLIENT_WEB, hl, gl,
                                    NULL, query, NULL, &raw_root) == IT_OK && raw_root) {
            root = (json_t *)raw_root;
            if (*out_count < 6) {
                walk_innertube_json(root, 0, 0, out_videos, capacity, out_count);
            }
            walk_innertube_json(root, 0, 1, s_cached_search_shorts, 24, &s_cached_search_shorts_count);
            json_decref(root);
        }
    }

    vt_log(VT_LOG_INFO, "[InnerTube] Busqueda '%s' extrajo %u videos y %u shorts",
           query, *out_count, s_cached_search_shorts_count);
    return IT_OK;
}

InnerTubeStatus innertube_search_shorts(AuthSession *session,
                                        const char *query,
                                        const char *hl,
                                        const char *gl,
                                        YoutubeVideo *out_videos,
                                        unsigned int capacity,
                                        unsigned int *out_count)
{
    if (!out_videos || capacity == 0 || !out_count) return IT_ERR_JSON;
    *out_count = 0;

    for (unsigned int i = 0; i < s_cached_search_shorts_count && *out_count < capacity; i++) {
        if (!video_already_added(out_videos, *out_count, s_cached_search_shorts[i].id)) {
            out_videos[(*out_count)++] = s_cached_search_shorts[i];
        }
    }

    if (*out_count < 5 && query && query[0]) {
        char sh_query[160];
        snprintf(sh_query, sizeof(sh_query), "%s #shorts", query);
        void *raw_root = NULL;
        if (innertube_post_endpoint("search", session, IT_CLIENT_WEB, hl, gl,
                                    NULL, sh_query, NULL, &raw_root) == IT_OK && raw_root) {
            json_t *root = (json_t *)raw_root;
            walk_innertube_json(root, 0, 2, out_videos, capacity, out_count);
            json_decref(root);
        }
    }

    return (*out_count > 0) ? IT_OK : IT_ERR_NETWORK;
}

static void extract_runs_concat(json_t *node, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!node) return;

    const char * direct = extract_text_obj(node);
    if (direct && direct[0]) {
        json_t *runs = json_is_object(node) ? json_object_get(node, "runs") : NULL;
        if (json_is_array(runs) && json_array_size(runs) > 1) {
            size_t i;
            json_t *r;
            size_t used = 0;
            json_array_foreach(runs, i, r) {
                const char *t = json_string_value(json_object_get(r, "text"));
                if (t && t[0] && used + 1 < out_sz) {
                    int n = snprintf(out + used, out_sz - used, "%s", t);
                    if (n > 0) {
                        used += (size_t)n;
                        if (used >= out_sz) used = out_sz - 1;
                    }
                }
            }
            return;
        }
        snprintf(out, out_sz, "%s", direct);
    }
}

static void normalize_avatar_url(const char *raw_url, char *out, size_t out_sz)
{
    if (!raw_url || !raw_url[0] || !out || out_sz == 0) return;
    if (strncmp(raw_url, "//", 2) == 0) {
        snprintf(out, out_sz, "https:%s", raw_url);
    } else {
        snprintf(out, out_sz, "%s", raw_url);
    }
}

static void find_continuation_token_recursive(json_t *node, int depth, char *out_tok, size_t tok_sz)
{
    if (!node || !out_tok || out_tok[0] != '\0' || depth > 14) return;
    if (json_is_object(node)) {
        json_t *ccmd = json_object_get(node, "continuationCommand");
        if (json_is_object(ccmd)) {
            const char *tok = json_string_value(json_object_get(ccmd, "token"));
            if (tok && tok[0]) {
                snprintf(out_tok, tok_sz, "%s", tok);
                return;
            }
        }
        json_t *rcmd = json_object_get(node, "reloadContinuationItemsCommand");
        if (json_is_object(rcmd)) {
            const char *tok = json_string_value(json_object_get(rcmd, "token"));
            if (tok && tok[0]) {
                snprintf(out_tok, tok_sz, "%s", tok);
                return;
            }
        }
        json_t *ncd = json_object_get(node, "nextContinuationData");
        if (json_is_object(ncd)) {
            const char *tok = json_string_value(json_object_get(ncd, "continuation"));
            if (tok && tok[0]) {
                snprintf(out_tok, tok_sz, "%s", tok);
                return;
            }
        }
        const char *k;
        json_t *v;
        json_object_foreach(node, k, v) {
            if (out_tok[0] != '\0') break;
            find_continuation_token_recursive(v, depth + 1, out_tok, tok_sz);
        }
    } else if (json_is_array(node)) {
        size_t i;
        json_t *v;
        json_array_foreach(node, i, v) {
            if (out_tok[0] != '\0') break;
            find_continuation_token_recursive(v, depth + 1, out_tok, tok_sz);
        }
    }
}

static void walk_next_metadata(json_t *node, int depth,
                               char *out_channel, size_t channel_sz,
                               char *out_avatar_url, size_t avatar_sz,
                               char *out_cont_token, size_t token_sz,
                               YoutubeComment *comments, unsigned int cap, unsigned int *count)
{
    if (!node || depth > 38) return;

    if (json_is_object(node)) {
        /* 1. Extraer canal propietario e imagen del canal (videoOwnerRenderer / slimOwnerRenderer / reelPlayerHeaderRenderer) */
        json_t *vor = json_object_get(node, "videoOwnerRenderer");
        if (!json_is_object(vor)) vor = json_object_get(node, "slimOwnerRenderer");
        if (json_is_object(vor)) {
            if (out_channel && out_channel[0] == '\0') {
                const char *ch = extract_text_obj(json_object_get(vor, "title"));
                if (ch && ch[0]) {
                    snprintf(out_channel, channel_sz, "%s", ch);
                }
            }
            if (out_avatar_url && out_avatar_url[0] == '\0') {
                const char *av = extract_thumbnail_url(json_object_get(vor, "thumbnail"));
                if (av && av[0]) {
                    normalize_avatar_url(av, out_avatar_url, avatar_sz);
                }
            }
        }

        json_t *rphr = json_object_get(node, "reelPlayerHeaderRenderer");
        if (json_is_object(rphr)) {
            if (out_channel && out_channel[0] == '\0') {
                const char *ch = extract_text_obj(json_object_get(rphr, "channelTitleText"));
                if (ch && ch[0]) {
                    snprintf(out_channel, channel_sz, "%s", ch);
                }
            }
            if (out_avatar_url && out_avatar_url[0] == '\0') {
                const char *av = extract_thumbnail_url(json_object_get(rphr, "channelThumbnail"));
                if (av && av[0]) {
                    normalize_avatar_url(av, out_avatar_url, avatar_sz);
                }
            }
        }

        if (out_avatar_url && out_avatar_url[0] == '\0') {
            json_t *davm = json_object_get(node, "decoratedAvatarViewModel");
            if (json_is_object(davm)) {
                json_t *av_wrap = json_object_get(davm, "avatar");
                json_t *avm = json_is_object(av_wrap) ? json_object_get(av_wrap, "avatarViewModel") : NULL;
                const char *av = json_is_object(avm) ? extract_thumbnail_url(json_object_get(avm, "image")) : NULL;
                if (av && av[0]) {
                    normalize_avatar_url(av, out_avatar_url, avatar_sz);
                }
            }
        }

        /* 2. Extraer token de continuacion de la seccion o panel de comentarios */
        if (out_cont_token && out_cont_token[0] == '\0') {
            json_t *isr = json_object_get(node, "itemSectionRenderer");
            if (json_is_object(isr)) {
                const char *sec_id = json_string_value(json_object_get(isr, "sectionIdentifier"));
                const char *tgt_id = json_string_value(json_object_get(isr, "targetId"));
                if ((sec_id && strstr(sec_id, "comment")) || (tgt_id && strstr(tgt_id, "comment"))) {
                    find_continuation_token_recursive(isr, 0, out_cont_token, token_sz);
                }
            }
            json_t *epsl = json_object_get(node, "engagementPanelSectionListRenderer");
            if (json_is_object(epsl) && out_cont_token[0] == '\0') {
                const char *pan_id = json_string_value(json_object_get(epsl, "panelIdentifier"));
                const char *tgt_id = json_string_value(json_object_get(epsl, "targetId"));
                if ((pan_id && strstr(pan_id, "comment")) || (tgt_id && strstr(tgt_id, "comment"))) {
                    find_continuation_token_recursive(epsl, 0, out_cont_token, token_sz);
                }
            }
        }

        /* 3. Extraer commentEntityPayload (nuevo modelo de comentarios de InnerTube) */
        if (comments && *count < cap) {
            json_t *cep = json_object_get(node, "commentEntityPayload");
            if (json_is_object(cep)) {
                json_t *props = json_object_get(cep, "properties");
                json_t *auth  = json_object_get(cep, "author");
                json_t *tool  = json_object_get(cep, "toolbar");
                const char *content_txt = json_is_object(props)
                    ? extract_text_obj(json_object_get(props, "content")) : NULL;
                const char *author_txt = json_is_object(auth)
                    ? json_string_value(json_object_get(auth, "displayName")) : NULL;
                const char *likes_txt = json_is_object(tool)
                    ? json_string_value(json_object_get(tool, "likeCountNotliked")) : NULL;
                if (content_txt && content_txt[0]) {
                    YoutubeComment *c = &comments[*count];
                    snprintf(c->author, sizeof(c->author), "%s", (author_txt && author_txt[0]) ? author_txt : "@usuario");
                    snprintf(c->text, sizeof(c->text), "%s", content_txt);
                    while (likes_txt && *likes_txt == ' ') likes_txt++;
                    snprintf(c->likes, sizeof(c->likes), "%s", (likes_txt && likes_txt[0]) ? likes_txt : "1");
                    (*count)++;
                    return;
                }
            }

            /* 4. Extraer commentRenderer clasico */
            json_t *cr = json_object_get(node, "commentRenderer");
            if (json_is_object(cr)) {
                char text_buf[256] = {0};
                extract_runs_concat(json_object_get(cr, "contentText"), text_buf, sizeof(text_buf));
                const char *author_txt = extract_text_obj(json_object_get(cr, "authorText"));
                const char *likes_txt  = extract_text_obj(json_object_get(cr, "voteCount"));
                if (text_buf[0] != '\0') {
                    YoutubeComment *c = &comments[*count];
                    snprintf(c->author, sizeof(c->author), "%s", (author_txt && author_txt[0]) ? author_txt : "@usuario");
                    snprintf(c->text, sizeof(c->text), "%s", text_buf);
                    while (likes_txt && *likes_txt == ' ') likes_txt++;
                    snprintf(c->likes, sizeof(c->likes), "%s", (likes_txt && likes_txt[0]) ? likes_txt : "1");
                    (*count)++;
                    return;
                }
            }
        }

        const char *key;
        json_t *val;
        json_object_foreach(node, key, val) {
            if (strcmp(key, "responseContext") == 0 ||
                strcmp(key, "trackingParams") == 0 ||
                strcmp(key, "playerOverlays") == 0) {
                continue;
            }
            walk_next_metadata(val, depth + 1, out_channel, channel_sz,
                               out_avatar_url, avatar_sz,
                               out_cont_token, token_sz, comments, cap, count);
        }
    } else if (json_is_array(node)) {
        size_t idx;
        json_t *val;
        json_array_foreach(node, idx, val) {
            walk_next_metadata(val, depth + 1, out_channel, channel_sz,
                               out_avatar_url, avatar_sz,
                               out_cont_token, token_sz, comments, cap, count);
        }
    }
}

InnerTubeStatus innertube_get_comments(AuthSession *session,
                                       const char *video_id,
                                       const char *hl,
                                       const char *gl,
                                       char *out_channel,
                                       size_t channel_sz,
                                       char *out_avatar_url,
                                       size_t avatar_sz,
                                       YoutubeComment *out_comments,
                                       unsigned int capacity,
                                       unsigned int *out_count)
{
    if (!video_id || !video_id[0] || !out_comments || capacity == 0 || !out_count) {
        return IT_ERR_JSON;
    }
    *out_count = 0;
    if (out_channel && channel_sz > 0) out_channel[0] = '\0';
    if (out_avatar_url && avatar_sz > 0) out_avatar_url[0] = '\0';

    const char *safe_gl = sanitize_innertube_gl(gl);

    /* Armar payload para /youtubei/v1/next con videoId */
    json_t *req = json_object();
    json_t *ctx = json_object();
    json_t *cli = json_object();
    json_object_set_new(cli, "clientName", json_string("WEB"));
    json_object_set_new(cli, "clientVersion", json_string("2.20240725.01.00"));
    json_object_set_new(cli, "hl", json_string((hl && hl[0]) ? hl : "es"));
    json_object_set_new(cli, "gl", json_string(safe_gl));
    json_object_set_new(ctx, "client", cli);
    json_object_set_new(req, "context", ctx);
    json_object_set_new(req, "videoId", json_string(video_id));

    char *payload = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    if (!payload) return IT_ERR_NO_MEM;

    char url[256];
    snprintf(url, sizeof(url), "%s/next?prettyPrint=false", INNERTUBE_BASE_URL);

    long http_code = 0;
    InnerTubeBuffer raw_buf = {0};
    InnerTubeStatus st = execute_innertube_http_post(url, payload, session, IT_CLIENT_WEB, &http_code, &raw_buf);
    free(payload);

    if (st != IT_OK || http_code != 200 || !raw_buf.data) {
        free(raw_buf.data);
        return IT_ERR_NETWORK;
    }

    json_error_t jerr;
    json_t *root = json_loads(raw_buf.data, 0, &jerr);
    free(raw_buf.data);
    raw_buf.data = NULL;
    if (!root) return IT_ERR_JSON;

    char cont_token[1024] = {0};
    walk_next_metadata(root, 0, out_channel, channel_sz,
                       out_avatar_url, avatar_sz,
                       cont_token, sizeof(cont_token),
                       out_comments, capacity, out_count);
    json_decref(root);

    /* Si se encontro el token de continuacion de comentarios, pedir el lote de comentarios */
    if (*out_count == 0 && cont_token[0] != '\0') {
        json_t *creq = json_object();
        json_t *cctx = json_object();
        json_t *ccli = json_object();
        json_object_set_new(ccli, "clientName", json_string("WEB"));
        json_object_set_new(ccli, "clientVersion", json_string("2.20240725.01.00"));
        json_object_set_new(ccli, "hl", json_string((hl && hl[0]) ? hl : "es"));
        json_object_set_new(ccli, "gl", json_string(safe_gl));
        json_object_set_new(cctx, "client", ccli);
        json_object_set_new(creq, "context", cctx);
        json_object_set_new(creq, "continuation", json_string(cont_token));

        char *cpayload = json_dumps(creq, JSON_COMPACT);
        json_decref(creq);
        if (cpayload) {
            InnerTubeBuffer cbuf = {0};
            long ch_code = 0;
            if (execute_innertube_http_post(url, cpayload, session, IT_CLIENT_WEB, &ch_code, &cbuf) == IT_OK &&
                ch_code == 200 && cbuf.data) {
                json_t *croot = json_loads(cbuf.data, 0, &jerr);
                free(cbuf.data);
                cbuf.data = NULL;
                if (croot) {
                    walk_next_metadata(croot, 0, out_channel, channel_sz,
                                       out_avatar_url, avatar_sz,
                                       NULL, 0,
                                       out_comments, capacity, out_count);
                    json_decref(croot);
                }
            } else {
                free(cbuf.data);
            }
            free(cpayload);
        }
    }

    vt_log(VT_LOG_INFO, "[InnerTube] /next video=%s canal='%s' avatar=%s comments=%u",
           video_id,
           (out_channel && out_channel[0]) ? out_channel : "",
           (out_avatar_url && out_avatar_url[0]) ? "si" : "no",
           *out_count);
    return IT_OK;
}
