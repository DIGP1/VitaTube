#include "auth.h"
#include "../log.h"

#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <curl/curl.h>
#include <jansson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OAUTH_DEVICE_CODE_URL "https://oauth2.googleapis.com/device/code"
#define OAUTH_TOKEN_URL       "https://oauth2.googleapis.com/token"
#define AUTH_MAX_BUF_SIZE     (64 * 1024)

typedef struct {
    char  *data;
    size_t size;
} AuthCurlBuffer;

static size_t auth_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    AuthCurlBuffer *mem = (AuthCurlBuffer *)userp;

    if (mem->size + realsize >= AUTH_MAX_BUF_SIZE) {
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

static AuthStatus oauth_post_urlencoded(const char *url, const char *post_fields,
                                        long *out_http_code, json_t **out_json)
{
    if (out_http_code) *out_http_code = 0;
    if (out_json) *out_json = NULL;

    CURL *curl = curl_easy_init();
    if (!curl) {
        vt_log(VT_LOG_ERROR, "[Auth] Fallo curl_easy_init()");
        return AUTH_ERR_NETWORK;
    }

    AuthCurlBuffer chunk;
    chunk.data = (char *)calloc(1, 1);
    chunk.size = 0;
    if (!chunk.data) {
        curl_easy_cleanup(curl);
        return AUTH_ERR_NETWORK;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, auth_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    /* Parametros esenciales para PS Vita: sin senales POSIX y timeouts contra caidas de Wi-Fi */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (out_http_code) *out_http_code = http_code;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        vt_log(VT_LOG_ERROR, "[Auth] Error libcurl (%d): %s", (int)res, curl_easy_strerror(res));
        free(chunk.data);
        return AUTH_ERR_NETWORK;
    }

    json_error_t jerr;
    json_t *root = json_loads(chunk.data, 0, &jerr);
    /* Liberar inmediatamente el texto crudo de la respuesta HTTP */
    free(chunk.data);

    if (!root) {
        vt_log(VT_LOG_ERROR, "[Auth] Error parseando JSON (linea %d): %s", jerr.line, jerr.text);
        return AUTH_ERR_JSON;
    }

    *out_json = root;
    return AUTH_OK;
}

int auth_init(AuthSession *session)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/VitaTube", 0777);
    sceIoMkdir(VITATUBE_AUTH_DIR, 0777);

    if (!session) return -1;
    memset(session, 0, sizeof(AuthSession));

    if (auth_load_session(session) == AUTH_OK) {
        vt_log(VT_LOG_INFO, "[Auth] Sesion OAuth 2.0 cargada desde %s", VITATUBE_AUTH_FILE);
        return 0;
    }
    return -1;
}

AuthStatus auth_request_device_code(AuthDeviceCode *out_code)
{
    if (!out_code) return AUTH_ERR_JSON;
    memset(out_code, 0, sizeof(AuthDeviceCode));

    char post_fields[512];
    snprintf(post_fields, sizeof(post_fields),
             "client_id=%s&scope=%s",
             YT_TV_CLIENT_ID, YT_OAUTH_SCOPE);

    vt_log(VT_LOG_INFO, "[Auth] Solicitando device_code a Google OAuth 2.0...");

    long http_code = 0;
    json_t *root = NULL;
    AuthStatus st = oauth_post_urlencoded(OAUTH_DEVICE_CODE_URL, post_fields, &http_code, &root);
    if (st != AUTH_OK) {
        return st;
    }

    if (http_code != 200) {
        const char *err_desc = json_string_value(json_object_get(root, "error_description"));
        vt_log(VT_LOG_ERROR, "[Auth] device/code HTTP %ld: %s",
               http_code, err_desc ? err_desc : "rechazado");
        json_decref(root);
        return AUTH_ERR_DENIED;
    }

    const char *device_code = json_string_value(json_object_get(root, "device_code"));
    const char *user_code   = json_string_value(json_object_get(root, "user_code"));
    const char *verif_url   = json_string_value(json_object_get(root, "verification_url"));
    if (!verif_url) {
        verif_url = json_string_value(json_object_get(root, "verification_uri"));
    }

    json_t *j_expires  = json_object_get(root, "expires_in");
    json_t *j_interval = json_object_get(root, "interval");

    if (!device_code || !user_code || !verif_url) {
        vt_log(VT_LOG_ERROR, "[Auth] Respuesta device/code incompleta");
        json_decref(root);
        return AUTH_ERR_JSON;
    }

    snprintf(out_code->device_code, sizeof(out_code->device_code), "%s", device_code);
    snprintf(out_code->user_code, sizeof(out_code->user_code), "%s", user_code);
    snprintf(out_code->verification_url, sizeof(out_code->verification_url), "%s", verif_url);
    out_code->expires_in = json_is_integer(j_expires) ? (int)json_integer_value(j_expires) : 1800;
    out_code->interval   = json_is_integer(j_interval) ? (int)json_integer_value(j_interval) : 5;
    if (out_code->interval < 5) out_code->interval = 5;

    json_decref(root);
    vt_log(VT_LOG_INFO, "[Auth] Codigo obtenido: user_code=%s url=%s interval=%ds",
           out_code->user_code, out_code->verification_url, out_code->interval);
    return AUTH_OK;
}

AuthStatus auth_poll_for_token(const AuthDeviceCode *code,
                               AuthSession *out_session,
                               volatile int *cancel_flag)
{
    if (!code || !out_session) return AUTH_ERR_JSON;

    int poll_interval_sec = (code->interval >= 5) ? code->interval : 5;
    int elapsed_sec = 0;
    int max_wait_sec = (code->expires_in > 0) ? code->expires_in : 900;
    int net_failures = 0;

    char post_fields[1024];
    snprintf(post_fields, sizeof(post_fields),
             "client_id=%s&client_secret=%s&code=%s"
             "&grant_type=http://oauth.net/grant_type/device/1.0",
             YT_TV_CLIENT_ID, YT_TV_CLIENT_SECRET, code->device_code);

    vt_log(VT_LOG_INFO, "[Auth] Iniciando polling cada %d segundos...", poll_interval_sec);

    while (elapsed_sec < max_wait_sec) {
        /* Esperar `poll_interval_sec` en pasos de 200ms para reaccionar rapido al boton Cancelar */
        int steps = poll_interval_sec * 5;
        for (int i = 0; i < steps; i++) {
            if (cancel_flag && *cancel_flag) {
                vt_log(VT_LOG_INFO, "[Auth] Polling cancelado por el usuario");
                return AUTH_ERR_CANCELLED;
            }
            sceKernelDelayThread(200 * 1000);
        }
        elapsed_sec += poll_interval_sec;

        if (cancel_flag && *cancel_flag) {
            return AUTH_ERR_CANCELLED;
        }

        long http_code = 0;
        json_t *root = NULL;
        AuthStatus st = oauth_post_urlencoded(OAUTH_TOKEN_URL, post_fields, &http_code, &root);

        if (st == AUTH_ERR_NETWORK) {
            net_failures++;
            vt_log(VT_LOG_WARN, "[Auth] Microcorte de red en polling (%d/5)", net_failures);
            if (net_failures >= 5) {
                return AUTH_ERR_NETWORK;
            }
            continue;
        }
        if (st != AUTH_OK) {
            return st;
        }
        net_failures = 0;

        const char *access_token  = json_string_value(json_object_get(root, "access_token"));
        const char *refresh_token = json_string_value(json_object_get(root, "refresh_token"));
        json_t *j_expires_in      = json_object_get(root, "expires_in");

        if (http_code == 200 && access_token && refresh_token) {
            int expires_in = json_is_integer(j_expires_in) ? (int)json_integer_value(j_expires_in) : 3600;

            memset(out_session, 0, sizeof(AuthSession));
            snprintf(out_session->access_token, sizeof(out_session->access_token), "%s", access_token);
            snprintf(out_session->refresh_token, sizeof(out_session->refresh_token), "%s", refresh_token);
            out_session->expires_at = time(NULL) + expires_in;
            out_session->logged_in  = 1;

            json_decref(root);
            vt_log(VT_LOG_INFO, "[Auth] Dispositivo autorizado exitosamente. Guardando tokens...");
            return auth_save_session(out_session);
        }

        const char *error_str = json_string_value(json_object_get(root, "error"));
        if (error_str) {
            if (strcmp(error_str, "authorization_pending") == 0) {
                json_decref(root);
                continue;
            } else if (strcmp(error_str, "slow_down") == 0) {
                poll_interval_sec += 5;
                json_decref(root);
                continue;
            } else if (strcmp(error_str, "access_denied") == 0) {
                vt_log(VT_LOG_WARN, "[Auth] Usuario denego la autorizacion");
                json_decref(root);
                return AUTH_ERR_DENIED;
            } else if (strcmp(error_str, "expired_token") == 0) {
                vt_log(VT_LOG_WARN, "[Auth] El codigo device_code ha expirado");
                json_decref(root);
                return AUTH_ERR_EXPIRED;
            }
        }

        json_decref(root);
        return AUTH_ERR_JSON;
    }

    return AUTH_ERR_EXPIRED;
}

AuthStatus auth_refresh_access_token(AuthSession *session)
{
    if (!session || session->refresh_token[0] == '\0') {
        return AUTH_ERR_EXPIRED;
    }

    vt_log(VT_LOG_INFO, "[Auth] Refrescando access_token con refresh_token...");

    char post_fields[1024];
    snprintf(post_fields, sizeof(post_fields),
             "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
             YT_TV_CLIENT_ID, YT_TV_CLIENT_SECRET, session->refresh_token);

    long http_code = 0;
    json_t *root = NULL;
    AuthStatus st = oauth_post_urlencoded(OAUTH_TOKEN_URL, post_fields, &http_code, &root);
    if (st != AUTH_OK) {
        return st;
    }

    const char *new_access  = json_string_value(json_object_get(root, "access_token"));
    const char *new_refresh = json_string_value(json_object_get(root, "refresh_token"));
    json_t *j_expires_in    = json_object_get(root, "expires_in");

    if (http_code != 200 || !new_access) {
        vt_log(VT_LOG_ERROR, "[Auth] Fallo al refrescar token (HTTP %ld)", http_code);
        json_decref(root);
        if (http_code == 400 || http_code == 401) {
            session->logged_in = 0;
        }
        return AUTH_ERR_EXPIRED;
    }

    int expires_in = json_is_integer(j_expires_in) ? (int)json_integer_value(j_expires_in) : 3600;
    snprintf(session->access_token, sizeof(session->access_token), "%s", new_access);
    if (new_refresh && new_refresh[0] != '\0') {
        snprintf(session->refresh_token, sizeof(session->refresh_token), "%s", new_refresh);
    }
    session->expires_at = time(NULL) + expires_in;
    session->logged_in  = 1;

    json_decref(root);
    vt_log(VT_LOG_INFO, "[Auth] access_token renovado exitosamente (valido por %ds)", expires_in);
    return auth_save_session(session);
}

AuthStatus auth_ensure_valid_token(AuthSession *session)
{
    if (!session || !session->logged_in) {
        return AUTH_ERR_EXPIRED;
    }
    if (session->expires_at > 0 && time(NULL) + 60 >= session->expires_at) {
        return auth_refresh_access_token(session);
    }
    return AUTH_OK;
}

AuthStatus auth_save_session(const AuthSession *session)
{
    if (!session) return AUTH_ERR_IO;

    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(VITATUBE_AUTH_DIR, 0777);

    json_t *root = json_object();
    if (!root) return AUTH_ERR_JSON;

    json_object_set_new(root, "access_token", json_string(session->access_token));
    json_object_set_new(root, "refresh_token", json_string(session->refresh_token));
    json_object_set_new(root, "expires_at", json_integer((json_int_t)session->expires_at));

    int rc = json_dump_file(root, VITATUBE_AUTH_FILE, JSON_INDENT(2));
    json_decref(root);

    if (rc != 0) {
        vt_log(VT_LOG_ERROR, "[Auth] No se pudo escribir %s", VITATUBE_AUTH_FILE);
        return AUTH_ERR_IO;
    }
    return AUTH_OK;
}

AuthStatus auth_load_session(AuthSession *out_session)
{
    if (!out_session) return AUTH_ERR_IO;

    json_error_t jerr;
    json_t *root = json_load_file(VITATUBE_AUTH_FILE, 0, &jerr);
    if (!root) {
        return AUTH_ERR_IO;
    }

    const char *access  = json_string_value(json_object_get(root, "access_token"));
    const char *refresh = json_string_value(json_object_get(root, "refresh_token"));
    json_t *j_exp       = json_object_get(root, "expires_at");

    if (!access || !refresh || refresh[0] == '\0') {
        json_decref(root);
        return AUTH_ERR_JSON;
    }

    memset(out_session, 0, sizeof(AuthSession));
    snprintf(out_session->access_token, sizeof(out_session->access_token), "%s", access);
    snprintf(out_session->refresh_token, sizeof(out_session->refresh_token), "%s", refresh);
    out_session->expires_at = json_is_integer(j_exp) ? (time_t)json_integer_value(j_exp) : 0;
    out_session->logged_in  = 1;

    json_decref(root);
    return AUTH_OK;
}

void auth_clear_session(AuthSession *session)
{
    if (session) {
        memset(session, 0, sizeof(AuthSession));
    }
    remove(VITATUBE_AUTH_FILE);
    vt_log(VT_LOG_INFO, "[Auth] Sesion cerrada y archivo %s eliminado", VITATUBE_AUTH_FILE);
}
