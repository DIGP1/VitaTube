#include "youtube.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PIPED_API "https://vitatubeapi.duckdns.org"
#define PIPED_PROXY "https://vitatubeproxy.duckdns.org"

static char api_base[160] = PIPED_API;
static char proxy_base[160] = PIPED_PROXY;
static char region_code[3] = "US";
static int region_auto = 1;
static char auth_token[1024];
static char auth_username[64];
static unsigned int max_quality = 480;
static int autoplay_enabled = 1;

static int parse_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static unsigned int parse_hex_4(const char *s)
{
    int d0 = parse_hex_digit(s[0]);
    int d1 = parse_hex_digit(s[1]);
    int d2 = parse_hex_digit(s[2]);
    int d3 = parse_hex_digit(s[3]);
    if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0) return 0;
    return (unsigned int)((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);
}

static int extract_country_code(const char *json, char *out_code)
{
    if (!json || !out_code) return 0;

    /* 1. Buscar "countryCode":"XX" (ip-api.com) */
    const char *p = strstr(json, "\"countryCode\":\"");
    if (p) {
        p += 15;
        if (p[0] >= 'A' && p[0] <= 'Z' && p[1] >= 'A' && p[1] <= 'Z') {
            out_code[0] = p[0];
            out_code[1] = p[1];
            out_code[2] = '\0';
            return 1;
        }
    }

    /* 2. Buscar "country_code":"XX" (ipapi.co / ipwho.is) */
    p = strstr(json, "\"country_code\":\"");
    if (p) {
        p += 16;
        if (p[0] >= 'A' && p[0] <= 'Z' && p[1] >= 'A' && p[1] <= 'Z') {
            out_code[0] = p[0];
            out_code[1] = p[1];
            out_code[2] = '\0';
            return 1;
        }
    }

    /* 3. Buscar "country":"XX" (country.is) */
    p = strstr(json, "\"country\":\"");
    if (p) {
        p += 11;
        if (p[0] >= 'A' && p[0] <= 'Z' && p[1] >= 'A' && p[1] <= 'Z' && (p[2] == '"' || p[2] == '\0' || p[2] == ',')) {
            out_code[0] = p[0];
            out_code[1] = p[1];
            out_code[2] = '\0';
            return 1;
        }
    }

    return 0;
}

static int json_field(const char *start, const char *key, char *output,
                      unsigned int output_size)
{
    char marker[32];
    const char *value;
    unsigned int out_len = 0;
    unsigned int in_idx = 0;
    int written = snprintf(marker, sizeof(marker), "\"%s\":\"", key);

    if (written < 0 || written >= (int)sizeof(marker)) return -1;
    value = strstr(start, marker);
    if (!value) return -1;
    value += written;

    while (value[in_idx] && value[in_idx] != '"' && out_len + 1 < output_size) {
        if (value[in_idx] == '\\' && value[in_idx + 1]) {
            char esc = value[in_idx + 1];
            if (esc == 'u' && value[in_idx + 2] && value[in_idx + 3] &&
                value[in_idx + 4] && value[in_idx + 5]) {
                unsigned int u1 = parse_hex_4(value + in_idx + 2);
                unsigned int codepoint = u1;
                in_idx += 6;

                /* Check for UTF-16 surrogate pair (common for emojis) */
                if (u1 >= 0xD800 && u1 <= 0xDBFF &&
                    value[in_idx] == '\\' && value[in_idx + 1] == 'u' &&
                    value[in_idx + 2] && value[in_idx + 3] &&
                    value[in_idx + 4] && value[in_idx + 5]) {
                    unsigned int u2 = parse_hex_4(value + in_idx + 2);
                    if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                        codepoint = 0x10000 + ((u1 - 0xD800) << 10) + (u2 - 0xDC00);
                        in_idx += 6;
                    }
                }

                /* Encode codepoint to valid UTF-8 */
                if (codepoint < 0x80) {
                    if (out_len + 1 < output_size) {
                        output[out_len++] = (char)codepoint;
                    }
                } else if (codepoint < 0x800) {
                    if (out_len + 2 < output_size) {
                        output[out_len++] = (char)(0xC0 | (codepoint >> 6));
                        output[out_len++] = (char)(0x80 | (codepoint & 0x3F));
                    }
                } else if (codepoint < 0x10000) {
                    if (out_len + 3 < output_size) {
                        output[out_len++] = (char)(0xE0 | (codepoint >> 12));
                        output[out_len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        output[out_len++] = (char)(0x80 | (codepoint & 0x3F));
                    }
                } else if (codepoint <= 0x10FFFF) {
                    if (out_len + 4 < output_size) {
                        output[out_len++] = (char)(0xF0 | (codepoint >> 18));
                        output[out_len++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                        output[out_len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        output[out_len++] = (char)(0x80 | (codepoint & 0x3F));
                    }
                }
                continue;
            } else if (esc == 'n') {
                output[out_len++] = ' ';
                in_idx += 2;
                continue;
            } else if (esc == 'r') {
                in_idx += 2;
                continue;
            } else if (esc == 't') {
                output[out_len++] = ' ';
                in_idx += 2;
                continue;
            } else if (esc == '"' || esc == '\\' || esc == '/') {
                output[out_len++] = esc;
                in_idx += 2;
                continue;
            }
        }
        output[out_len++] = value[in_idx++];
    }

    output[out_len] = 0;
    return value[in_idx] == '"' ? (int)out_len : -1;
}

static unsigned long json_number(const char *start, const char *key)
{
    char marker[32];
    const char *value;
    int written = snprintf(marker, sizeof(marker), "\"%s\":", key);

    if (written < 0 || written >= (int)sizeof(marker)) return 0;
    value = strstr(start, marker);
    if (!value) return 0;
    value += written;
    while (*value == ' ') value++;
    if (*value == '-') return 0;
    return strtoul(value, NULL, 10);
}

static int json_boolean(const char *start, const char *key)
{
    char marker[32];
    const char *value;
    int written = snprintf(marker, sizeof(marker), "\"%s\":", key);

    if (written < 0 || written >= (int)sizeof(marker)) return 0;
    value = strstr(start, marker);
    return value && strncmp(value + written, "true", 4) == 0;
}

static int json_live_value(const char *start)
{
    const char *dur = strstr(start, "\"duration\":");
    if (dur && (dur[11] == '-' || (dur[11] == ' ' && dur[12] == '-'))) {
        return 1;
    }
    const char *upl = strstr(start, "\"uploaded\":");
    if (upl && (upl[11] == '-' || (upl[11] == ' ' && upl[12] == '-'))) {
        return 1;
    }
    return json_boolean(start, "isLive") ||
           json_boolean(start, "isLiveNow") ||
           json_boolean(start, "liveNow");
}

static int is_safe_query_char(char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '-' || value == '_' || value == '.' || value == '~';
}

static int encode_query(const char *source, char *target, unsigned int size)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned int used = 0;

    while (*source && used + 4 < size) {
        unsigned char value = (unsigned char)*source++;
        if (is_safe_query_char((char)value)) {
            target[used++] = (char)value;
        } else if (value == ' ') {
            target[used++] = '+';
        } else {
            target[used++] = '%';
            target[used++] = hex[value >> 4];
            target[used++] = hex[value & 15];
        }
    }
    if (*source != 0 || used + 1 >= size) return -1;
    target[used] = 0;
    return 0;
}

static void load_config(void)
{
    FILE *file = fopen("ux0:data/VitaTube/config.ini", "r");
    char line[192];

    if (!file) {
        mkdir("ux0:data/VitaTube", 0777);
        file = fopen("ux0:data/VitaTube/config.ini", "w");
        if (file) {
            fprintf(file, "api_url=%s\nproxy_url=%s\nstream_api_url=%s\nregion=auto\n"
                    "max_quality=480\nautoplay=1\n", PIPED_API, PIPED_PROXY, STREAM_API_BASE_URL);
            fclose(file);
        }
        return;
    }
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "api_url=", 8) == 0) {
            sscanf(line + 8, "%159s", api_base);
        } else if (strncmp(line, "proxy_url=", 10) == 0) {
            sscanf(line + 10, "%159s", proxy_base);
        } else if (strncmp(line, "stream_api_url=", 15) == 0) {
            char custom_api[160];
            if (sscanf(line + 15, "%159s", custom_api) == 1) {
                if (strstr(custom_api, "147.224.168.110") != NULL) {
                    stream_api_set_url(STREAM_API_BASE_URL);
                } else {
                    stream_api_set_url(custom_api);
                }
            }
        } else if (strncmp(line, "cobalt_url=", 11) == 0) {
            char custom_api[160];
            if (sscanf(line + 11, "%159s", custom_api) == 1) {
                stream_api_set_url(custom_api);
            }
        } else if (strncmp(line, "region=", 7) == 0 &&
                   strncmp(line + 7, "auto", 4) != 0) {
            if (sscanf(line + 7, "%2[A-Za-z]", region_code) != 1) {
                strcpy(region_code, "US");
            } else {
                region_auto = 0;
            }
        } else if (strncmp(line, "max_quality=", 12) == 0) {
            unsigned int q = 720;
            if (sscanf(line + 12, "%u", &q) == 1) {
                if (q == 360 || q == 480 || q == 720) {
                    max_quality = q;
                }
            }
        } else if (strncmp(line, "autoplay=", 9) == 0) {
            int a = 1;
            if (sscanf(line + 9, "%d", &a) == 1) {
                autoplay_enabled = (a != 0);
            }
        }
    }
    fclose(file);
}

int youtube_init(void)
{
    FILE *token_file;
    FILE *user_file;

    load_config();
    auth_token[0] = 0;
    token_file = fopen("ux0:data/VitaTube/auth.token", "r");
    if (token_file) {
        fgets(auth_token, sizeof(auth_token), token_file);
        auth_token[strcspn(auth_token, "\r\n")] = 0;
        fclose(token_file);
    }
    auth_username[0] = 0;
    user_file = fopen("ux0:data/VitaTube/auth.user", "r");
    if (user_file) {
        fgets(auth_username, sizeof(auth_username), user_file);
        auth_username[strcspn(auth_username, "\r\n")] = 0;
        fclose(user_file);
    }
    if (region_auto) {
        char detected_buf[1024];
        int detected_ok = 0;

        /* Intento 1: ip-api.com (HTTP directo sin problemas de certificados TLS, devuelve JSON) */
        int r = http_get("http://ip-api.com/json/?fields=countryCode", detected_buf, sizeof(detected_buf));
        if (r > 0 && extract_country_code(detected_buf, region_code)) {
            detected_ok = 1;
        }

        /* Intento 2: api.country.is (HTTP formato JSON) */
        if (!detected_ok) {
            r = http_get("http://api.country.is/", detected_buf, sizeof(detected_buf));
            if (r > 0 && extract_country_code(detected_buf, region_code)) {
                detected_ok = 1;
            }
        }

        /* Intento 3: ipwho.is (HTTP formato JSON) */
        if (!detected_ok) {
            r = http_get("http://ipwho.is/?fields=country_code", detected_buf, sizeof(detected_buf));
            if (r > 0 && extract_country_code(detected_buf, region_code)) {
                detected_ok = 1;
            }
        }

        /* Intento 4: ipapi.co con endpoint /json/ (compatible con Accept: application/json) */
        if (!detected_ok) {
            r = http_get("https://ipapi.co/json/", detected_buf, sizeof(detected_buf));
            if (r > 0 && extract_country_code(detected_buf, region_code)) {
                detected_ok = 1;
            }
        }

        if (!detected_ok) {
            strcpy(region_code, "US");
            vt_log(VT_LOG_WARN, "[YouTube] No se pudo detectar region automatica, usando US por defecto");
        } else {
            vt_log(VT_LOG_INFO, "[YouTube] Region detectada exitosamente por IP: %s", region_code);
        }
    }
    vt_log(VT_LOG_INFO, "[YouTube] Inicializado region=%s auth=%d",
           region_code, auth_token[0] != 0);
    return 0;
}

void youtube_shutdown(void)
{
}

static int auth_request(const char *path, const char *username,
                        const char *password)
{
    char url[320];
    char body[512];
    char response[HTTP_RESPONSE_MAX];
    int written;
    int result;
    char token[1024];
    FILE *file;
    FILE *user_file;

    if (!username || !password || !username[0] || !password[0] ||
        strchr(username, '"') || strchr(username, '\\') ||
        strchr(password, '"') || strchr(password, '\\')) {
        return -1;
    }

    written = snprintf(url, sizeof(url), "%s%s", api_base, path);
    if (written < 0 || written >= (int)sizeof(url)) return -1;
    written = snprintf(body, sizeof(body),
                       "{\"username\":\"%s\",\"password\":\"%s\"}",
                       username, password);
    if (written < 0 || written >= (int)sizeof(body)) return -1;
    result = http_post_json(url, body, NULL, response, sizeof(response));
    if (result <= 0 || json_field(response, "token", token,
                                   sizeof(token)) < 0) {
        vt_log(VT_LOG_ERROR, "[Auth] Fallo en %s status=%d", path, result);
        return -1;
    }
    file = fopen("ux0:data/VitaTube/auth.token", "w");
    if (!file) return -1;
    fputs(token, file);
    fputc('\n', file);
    fclose(file);
    strncpy(auth_token, token, sizeof(auth_token) - 1);
    auth_token[sizeof(auth_token) - 1] = 0;
    strncpy(auth_username, username, sizeof(auth_username) - 1);
    auth_username[sizeof(auth_username) - 1] = 0;
    user_file = fopen("ux0:data/VitaTube/auth.user", "w");
    if (user_file) {
        fputs(auth_username, user_file);
        fputc('\n', user_file);
        fclose(user_file);
    }
    vt_log(VT_LOG_INFO, "[Auth] Operacion completada: %s", path);
    return 0;
}

int youtube_login(const char *username, const char *password)
{
    return auth_request("/login", username, password);
}

int youtube_register(const char *username, const char *password)
{
    return auth_request("/register", username, password);
}

int youtube_is_logged_in(void)
{
    return auth_token[0] != 0;
}

void youtube_logout(void)
{
    auth_token[0] = 0;
    auth_username[0] = 0;
    remove("ux0:data/VitaTube/auth.token");
    remove("ux0:data/VitaTube/auth.user");
}

const char *youtube_region(void)
{
    return region_code;
}

const char *youtube_username(void)
{
    return auth_username;
}

int youtube_search(const char *query, char *response, unsigned int size)
{
    char url[512];
    char encoded_query[256];
    int written;

    if (encode_query(query, encoded_query, sizeof(encoded_query)) < 0) {
        return -1;
    }
    written = snprintf(url, sizeof(url), "%s/search?q=%s&filter=videos",
                       api_base, encoded_query);
    if (written < 0 || written >= (int)sizeof(url)) return -1;
    vt_log(VT_LOG_INFO, "[YouTube] Busqueda iniciada");
    return auth_token[0]
        ? http_get_auth(url, auth_token, response, size)
        : http_get(url, response, size);
}

int youtube_parse_suggestions(const char *json, char suggestions[][64], unsigned int max_count)
{
    if (!json) return 0;
    const char *p = strchr(json, '[');
    if (!p) return 0;
    p++;

    unsigned int count = 0;
    while (*p && *p != ']' && count < max_count) {
        while (*p && (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n' || *p == '\t')) p++;
        if (*p == '"') {
            p++;
            char item[64];
            unsigned int len = 0;
            while (*p && *p != '"' && len + 1 < sizeof(item)) {
                if (*p == '\\' && *(p + 1)) {
                    p++;
                }
                item[len++] = *p++;
            }
            item[len] = 0;
            if (*p == '"') p++;
            if (len > 0) {
                strncpy(suggestions[count], item, sizeof(suggestions[0]) - 1);
                suggestions[count][sizeof(suggestions[0]) - 1] = 0;
                count++;
            }
        } else {
            p++;
        }
    }
    return (int)count;
}

int youtube_suggestions(const char *query, char suggestions[][64], unsigned int max_count)
{
    char url[512];
    char encoded_query[256];
    char response[4096];
    int written;
    int result;

    if (!query || !query[0]) return 0;

    if (encode_query(query, encoded_query, sizeof(encoded_query)) < 0) {
        return -1;
    }

    written = snprintf(url, sizeof(url), "%s/suggestions?query=%s", api_base, encoded_query);
    if (written < 0 || written >= (int)sizeof(url)) return -1;

    result = http_get(url, response, sizeof(response));
    if (result <= 0) return 0;

    return youtube_parse_suggestions(response, suggestions, max_count);
}

int youtube_streams(const char *video_id, char *response, unsigned int size)
{
    char url[320];
    int written = snprintf(url, sizeof(url), "%s/streams/%s",
                           api_base, video_id);
    if (written < 0 || written >= (int)sizeof(url)) return -1;
    vt_log(VT_LOG_INFO, "[YouTube] Streams solicitados");
    return auth_token[0]
        ? http_get_auth(url, auth_token, response, size)
        : http_get(url, response, size);
}

int youtube_select_stream(const char *json, char *url, unsigned int size,
                          unsigned int *quality)
{
    const char *cursor;
    const char *best_url = NULL;
    unsigned int best_quality = 0;
    size_t length;

    if (!json || !url || size == 0) return -1;
    cursor = strstr(json, "\"videoStreams\":[");
    if (!cursor) return -1;
    while ((cursor = strstr(cursor, "\"url\":\"")) != NULL) {
        const char *end = strchr(cursor, '}');
        const char *height;
        unsigned int candidate_quality = 0;
        if (!end) break;
        if (strstr(cursor, "\"videoOnly\":false") &&
            (height = strstr(cursor, "\"height\":")) != NULL &&
            height < end) {
            sscanf(height + 9, "%u", &candidate_quality);
            if (candidate_quality <= max_quality &&
                candidate_quality >= best_quality) {
                best_url = cursor + 7;
                best_quality = candidate_quality;
            }
        }
        cursor = end + 1;
    }
    if (!best_url) return -1;
    length = 0;
    while (best_url[length] && best_url[length] != '"' &&
           length + 1 < size) {
        url[length] = best_url[length];
        ++length;
    }
    if (best_url[length] != '"') return -1;
    url[length] = 0;
    if (quality) *quality = best_quality;
    return (int)length;
}

int youtube_get_stream_url(const char *video_id, char *url, unsigned int size)
{
    if (!video_id || !url || size == 0) return -1;

    char q_str[16];
    snprintf(q_str, sizeof(q_str), "%u", max_quality > 0 ? max_quality : 480);

    vt_log(VT_LOG_INFO, "[YouTube] Solicitando stream para %s (%sp) a la API privada...", video_id, q_str);
    if (stream_api_get_url_quality(video_id, url, size, q_str) == 0 && url[0] != '\0') {
        vt_log(VT_LOG_INFO, "[YouTube] Stream %sp obtenido exitosamente de la API privada", q_str);
        return 0;
    }

    vt_log(VT_LOG_WARN, "[YouTube] API privada fallo, intentando fallback con Piped...");
    char *piped_buf = (char *)malloc(HTTP_RESPONSE_MAX);
    if (piped_buf) {
        int bytes = youtube_streams(video_id, piped_buf, HTTP_RESPONSE_MAX);
        if (bytes > 0) {
            unsigned int q = 0;
            if (youtube_select_stream(piped_buf, url, size, &q) > 0) {
                free(piped_buf);
                vt_log(VT_LOG_INFO, "[YouTube] Stream obtenido mediante fallback de Piped");
                return 0;
            }
        }
        free(piped_buf);
    }

    vt_log(VT_LOG_ERROR, "[YouTube] No se pudo obtener el stream por ninguna via");
    return -1;
}

int youtube_parse_videos(const char *json, YoutubeVideo *videos,
                         unsigned int capacity)
{
    const char *cursor = json;
    unsigned int count = 0;

    while (count < capacity) {
        const char *item = strstr(cursor, "\"url\":\"/watch?v=");
        const char *video_id;
        if (!item) break;
        video_id = strstr(item, "?v=");
        if (!video_id) break;
        video_id += 3;
        sscanf(video_id, "%31[^\"]", videos[count].id);
        if (json_field(item, "title", videos[count].title,
                       sizeof(videos[count].title)) < 0) break;
        if (json_field(item, "thumbnail", videos[count].thumbnail,
                       sizeof(videos[count].thumbnail)) < 0) {
            videos[count].thumbnail[0] = 0;
        }
        if (json_field(item, "uploaderName", videos[count].channel,
                       sizeof(videos[count].channel)) < 0) {
            videos[count].channel[0] = 0;
        }
        unsigned long dur = json_number(item, "duration");
        if (dur == 0) dur = json_number(item, "lengthSeconds");
        videos[count].duration = (unsigned int)dur;
        videos[count].views = json_number(item, "views");
        videos[count].is_live = json_live_value(item);
        if (videos[count].is_live) {
            videos[count].duration = 0;
        }
        ++count;
        cursor = item + 1;
    }
    return (int)count;
}

int youtube_trending(YoutubeVideo *videos, unsigned int capacity)
{
    static char response[HTTP_RESPONSE_MAX * 2];
    char url[256];
    int result;

    if (auth_token[0]) {
        snprintf(url, sizeof(url), "%s/feed", api_base);
        result = http_get_auth(url, auth_token, response, sizeof(response));
        if (result <= 0) {
            snprintf(url, sizeof(url), "%s/trending?region=%s", api_base,
                     region_code);
            result = http_get(url, response, sizeof(response));
        }
    } else {
        snprintf(url, sizeof(url), "%s/trending?region=%s", api_base,
                 region_code);
        result = http_get(url, response, sizeof(response));
    }
    if (result < 0) return result;
    return youtube_parse_videos(response, videos, capacity);
}

int youtube_subscriptions(YoutubeVideo *videos, unsigned int capacity)
{
    static char response[HTTP_RESPONSE_MAX * 2];
    char url[256];
    int result;

    if (!auth_token[0]) {
        return -1;
    }
    snprintf(url, sizeof(url), "%s/feed", api_base);
    result = http_get_auth(url, auth_token, response, sizeof(response));
    if (result <= 0) return -1;
    return youtube_parse_videos(response, videos, capacity);
}

int youtube_download_thumbnail(const YoutubeVideo *video, void *buffer,
                               unsigned int size)
{
    char url[128];

    if (!video || !video->thumbnail[0]) return -1;
    /*
     * The configured Piped proxy currently negotiates WebP for its thumbnail
     * endpoint. vita2d's JPEG loader cannot decode that response, so use the
     * canonical JPEG thumbnail while retaining Piped for API and playback.
     */
    if (snprintf(url, sizeof(url),
                 "https://i.ytimg.com/vi/%s/hqdefault.jpg", video->id) < 0) {
        return -1;
    }
    return http_get_binary(url, buffer, size);
}

unsigned int youtube_max_quality(void)
{
    return max_quality;
}

void youtube_set_max_quality(unsigned int quality)
{
    if (quality == 360 || quality == 480 || quality == 720) {
        max_quality = quality;
    }
}

const char *youtube_quality_label(void)
{
    if (max_quality == 240) return "240p";
    if (max_quality == 360) return "360p";
    if (max_quality == 480) return "480p";
    return "720p";
}

void youtube_cycle_quality(void)
{
    if (max_quality == 720) {
        max_quality = 480;
    } else if (max_quality == 480) {
        max_quality = 360;
    } else if (max_quality == 360) {
        max_quality = 240;
    } else {
        max_quality = 720;
    }
}

int youtube_autoplay(void)
{
    return autoplay_enabled;
}

void youtube_toggle_autoplay(void)
{
    autoplay_enabled = !autoplay_enabled;
}

void youtube_save_config(void)
{
    mkdir("ux0:data/VitaTube", 0777);
    FILE *file = fopen("ux0:data/VitaTube/config.ini", "w");
    if (!file) return;

    fprintf(file, "api_url=%s\n", api_base);
    fprintf(file, "proxy_url=%s\n", proxy_base);
    fprintf(file, "stream_api_url=%s\n", stream_api_get_active_url());
    if (region_auto) {
        fprintf(file, "region=auto\n");
    } else {
        fprintf(file, "region=%s\n", region_code);
    }
    fprintf(file, "max_quality=%u\n", max_quality);
    fprintf(file, "autoplay=%d\n", autoplay_enabled ? 1 : 0);
    fclose(file);
    vt_log(VT_LOG_INFO, "[Config] Guardado quality=%up autoplay=%d",
           max_quality, autoplay_enabled);
}
