#include "vps.h"
#include "../log.h"

#include <curl/curl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VPS_MAX_RESPONSE_SIZE (64 * 1024)

typedef struct {
    char  *data;
    size_t size;
} VpsCurlBuffer;

static size_t vps_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    VpsCurlBuffer *mem = (VpsCurlBuffer *)userp;

    if (mem->size + realsize >= VPS_MAX_RESPONSE_SIZE) {
        return 0;
    }

    char *ptr = (char *)realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

static int vps_request_endpoint(const char *endpoint_url,
                                const char *post_body,
                                char *out_url,
                                size_t out_size)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        return -1;
    }

    VpsCurlBuffer response_buf;
    response_buf.data = (char *)calloc(1, 1);
    response_buf.size = 0;
    if (!response_buf.data) {
        curl_easy_cleanup(curl);
        return -1;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vps_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);

    /* Estabilidad en PS Vita ante caidas de Wi-Fi */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200 || !response_buf.data) {
        vt_log(VT_LOG_WARN, "[VPS] Fallo POST a %s (curl=%d http=%ld)",
               endpoint_url, (int)res, http_code);
        free(response_buf.data);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(response_buf.data, 0, &jerr);
    /* Liberar inmediatamente el buffer HTTP crudo */
    free(response_buf.data);

    if (!root) {
        vt_log(VT_LOG_ERROR, "[VPS] Error parseando JSON con jansson: %s", jerr.text);
        return -2;
    }

    const char *status     = json_string_value(json_object_get(root, "status"));
    const char *stream_url = json_string_value(json_object_get(root, "url"));

    int rc = -3;
    if (status && stream_url && stream_url[0] != '\0') {
        if (strcmp(status, "redirect") == 0 ||
            strcmp(status, "tunnel") == 0 ||
            strcmp(status, "stream") == 0) {
            snprintf(out_url, out_size, "%s", stream_url);
            rc = 0;
        } else {
            const char *msg = json_string_value(json_object_get(root, "message"));
            vt_log(VT_LOG_ERROR, "[VPS] Servidor devolvio status='%s' msg='%s'",
                   status, msg ? msg : "");
        }
    }

    json_decref(root);
    return rc;
}

int vps_get_video_url_quality(const char *video_id, const char *quality,
                              char *out_url, size_t out_size)
{
    if (!video_id || !video_id[0] || !out_url || out_size == 0) {
        return -1;
    }
    out_url[0] = '\0';

    char watch_url[256];
    if (strncmp(video_id, "http://", 7) == 0 || strncmp(video_id, "https://", 8) == 0) {
        snprintf(watch_url, sizeof(watch_url), "%s", video_id);
    } else {
        snprintf(watch_url, sizeof(watch_url), "https://www.youtube.com/watch?v=%s", video_id);
    }

    const char *q_val = (quality && quality[0]) ? quality : "720";

    /* Construir el payload JSON con jansson */
    json_t *req = json_object();
    if (!req) return -1;

    json_object_set_new(req, "url", json_string(watch_url));
    json_object_set_new(req, "videoQuality", json_string(q_val));
    json_object_set_new(req, "youtubeVideoCodec", json_string("h264"));

    char *post_body = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    if (!post_body) {
        return -1;
    }

    vt_log(VT_LOG_INFO, "[VPS] Solicitando video %s (%sp H.264) a %s",
           video_id, q_val, VPS_ORACLE_URL);

    int rc = vps_request_endpoint(VPS_ORACLE_URL, post_body, out_url, out_size);
    if (rc != 0) {
        vt_log(VT_LOG_INFO, "[VPS] Intentando dominio secundario %s...", VPS_FALLBACK_URL);
        rc = vps_request_endpoint(VPS_FALLBACK_URL, post_body, out_url, out_size);
    }

    free(post_body);
    return rc;
}

char *get_video_url_from_vps(const char *video_id)
{
    char final_url[4096];
    if (vps_get_video_url_quality(video_id, "720", final_url, sizeof(final_url)) == 0 &&
        final_url[0] != '\0') {
        return strdup(final_url);
    }
    return NULL;
}
