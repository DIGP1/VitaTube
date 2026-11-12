#include "stream_api.h"
#include "http.h"
#include "log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STREAM_RESPONSE_BUFFER_SIZE (64 * 1024)

/* URL activa en memoria (por defecto la constante definida en stream_api.h) */
static char s_api_base_url[256] = STREAM_API_BASE_URL;

void stream_api_set_url(const char *url)
{
    if (url && url[0] != '\0') {
        strncpy(s_api_base_url, url, sizeof(s_api_base_url) - 1);
        s_api_base_url[sizeof(s_api_base_url) - 1] = '\0';
    } else {
        strncpy(s_api_base_url, STREAM_API_BASE_URL, sizeof(s_api_base_url) - 1);
        s_api_base_url[sizeof(s_api_base_url) - 1] = '\0';
    }
}

const char *stream_api_get_active_url(void)
{
    return s_api_base_url;
}

/**
 * Asegura que la entrada tenga el formato "https://www.youtube.com/watch?v=[VIDEO_ID]".
 * Si se pasa solo el ID (ej: "dQw4w9WgXcQ"), antepone el prefijo requerido.
 */
static void normalize_youtube_url(const char *input, char *output, size_t max_len)
{
    if (!input || !output || max_len == 0) return;

    if (strncmp(input, "http://", 7) == 0 || strncmp(input, "https://", 8) == 0) {
        strncpy(output, input, max_len - 1);
        output[max_len - 1] = '\0';
    } else {
        snprintf(output, max_len, "https://www.youtube.com/watch?v=%s", input);
    }
}

static char s_stream_quality[16] = "720";

void stream_api_set_quality(const char *quality)
{
    if (quality && quality[0]) {
        strncpy(s_stream_quality, quality, sizeof(s_stream_quality) - 1);
        s_stream_quality[sizeof(s_stream_quality) - 1] = '\0';
    }
}

const char *stream_api_get_quality(void)
{
    return s_stream_quality;
}

/**
 * Construye el payload JSON estricto para el decodificador de la PS Vita:
 * {
 *   "url": "https://www.youtube.com/watch?v=...",
 *   "videoQuality": "720",
 *   "youtubeVideoCodec": "h264"
 * }
 */
static char *build_request_payload(const char *full_url, const char *quality)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    const char *q = (quality && quality[0]) ? quality : s_stream_quality;
    cJSON_AddStringToObject(root, "url", full_url);
    cJSON_AddStringToObject(root, "videoQuality", q);
    cJSON_AddStringToObject(root, "youtubeVideoCodec", "h264");

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

/**
 * Procesa la respuesta JSON de la API privada.
 * Valida que "status" == "redirect" y extrae la clave "url".
 */
static int parse_api_response(const char *json_data, char *out_url, unsigned int out_size)
{
    if (!json_data || !out_url || out_size == 0) return -1;

    cJSON *root = cJSON_Parse(json_data);
    if (!root) {
        vt_log(VT_LOG_ERROR, "[StreamAPI] Error parseando JSON de respuesta: no es valido");
        return -2;
    }

    /* Validar que el campo "status" exista y sea exactamente "redirect" */
    cJSON *status_item = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (!status_item || !cJSON_IsString(status_item) || !status_item->valuestring) {
        vt_log(VT_LOG_ERROR, "[StreamAPI] Respuesta carece de campo 'status' valido");
        cJSON_Delete(root);
        return -3;
    }

    if (strcmp(status_item->valuestring, "redirect") != 0) {
        cJSON *msg_item = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (msg_item && cJSON_IsString(msg_item) && msg_item->valuestring) {
            vt_log(VT_LOG_ERROR, "[StreamAPI] Error de la API: %s", msg_item->valuestring);
        } else {
            vt_log(VT_LOG_ERROR, "[StreamAPI] Status inesperado: '%s' (esperado: 'redirect')",
                   status_item->valuestring);
        }
        cJSON_Delete(root);
        return -4;
    }

    /* Extraer la clave "url" del flujo de video directo */
    cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!url_item || !cJSON_IsString(url_item) || !url_item->valuestring || url_item->valuestring[0] == '\0') {
        vt_log(VT_LOG_ERROR, "[StreamAPI] Clave 'url' no encontrada o vacia en el JSON");
        cJSON_Delete(root);
        return -5;
    }

    const char *raw_url = url_item->valuestring;
    const char *stream_path = strstr(raw_url, "/stream/");
    if (stream_path) {
        /* Reemplazar host por el dominio base activo (s_api_base_url) */
        char base_clean[160];
        strncpy(base_clean, s_api_base_url, sizeof(base_clean) - 1);
        base_clean[sizeof(base_clean) - 1] = '\0';
        size_t blen = strlen(base_clean);
        if (blen > 0 && base_clean[blen - 1] == '/') {
            base_clean[blen - 1] = '\0';
        }
        snprintf(out_url, out_size, "%s%s", base_clean, stream_path);
    } else {
        strncpy(out_url, raw_url, out_size - 1);
        out_url[out_size - 1] = '\0';
    }

    cJSON_Delete(root);
    return 0;
}

int stream_api_get_url_quality(const char *video_id_or_url, char *out_url, unsigned int out_size, const char *quality)
{
    if (!video_id_or_url || !out_url || out_size == 0) return -1;

    char full_youtube_url[256];
    normalize_youtube_url(video_id_or_url, full_youtube_url, sizeof(full_youtube_url));

    const char *target_q = (quality && quality[0]) ? quality : s_stream_quality;
    char *payload = build_request_payload(full_youtube_url, target_q);
    if (!payload) {
        vt_log(VT_LOG_ERROR, "[StreamAPI] Fallo al construir payload JSON");
        return -2;
    }

    char *response_buf = (char *)malloc(STREAM_RESPONSE_BUFFER_SIZE);
    if (!response_buf) {
        free(payload);
        vt_log(VT_LOG_ERROR, "[StreamAPI] Error de memoria (malloc) para buffer de respuesta");
        return -3;
    }

    vt_log(VT_LOG_INFO, "[StreamAPI] Enviando POST a: %s (calidad %s)", s_api_base_url, target_q);
    vt_log(VT_LOG_DEBUG, "[StreamAPI] Payload: %s", payload);

    /*
     * Realiza la peticion HTTP POST via SceHttp (http_post_json agrega
     * cabeceras obligatorias Accept: application/json y Content-Type: application/json).
     */
    int bytes = http_post_json(s_api_base_url, payload, NULL, response_buf, STREAM_RESPONSE_BUFFER_SIZE);

    free(payload);

    if (bytes <= 0) {
        vt_log(VT_LOG_ERROR, "[StreamAPI] Error en peticion HTTP (codigo: %d [0x%08X])",
               bytes, (unsigned int)bytes);
        free(response_buf);
        return -4; /* Error de conexion o codigo HTTP distinto a 200 */
    }

    vt_log(VT_LOG_DEBUG, "[StreamAPI] Respuesta recibida (%d bytes)", bytes);

    int parse_result = parse_api_response(response_buf, out_url, out_size);
    free(response_buf);

    if (parse_result == 0) {
        vt_log(VT_LOG_INFO, "[StreamAPI] Stream H.264/%sp extraido con exito", target_q);
    } else {
        vt_log(VT_LOG_ERROR, "[StreamAPI] Error al procesar respuesta JSON: %d", parse_result);
    }

    return parse_result;
}

int stream_api_get_url(const char *video_id_or_url, char *out_url, unsigned int out_size)
{
    return stream_api_get_url_quality(video_id_or_url, out_url, out_size, s_stream_quality);
}

char *get_video_stream_url(const char *video_id_or_url)
{
    char temp_url[4096];
    if (stream_api_get_url(video_id_or_url, temp_url, sizeof(temp_url)) == 0) {
        return strdup(temp_url);
    }
    return NULL;
}
