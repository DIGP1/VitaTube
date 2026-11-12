#include "http.h"
#include "log.h"
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>
#include <string.h>
#include <stdio.h>

#define BROWSER_USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

static int http_ready;
static int http_template;

int http_init(void)
{
    SceNetInitParam net_param;
    static unsigned char net_memory[256 * 1024];

    memset(&net_param, 0, sizeof(net_param));
    net_param.memory = net_memory;
    net_param.size = sizeof(net_memory);
    net_param.flags = 0;

    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0 ||
        sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP) < 0 ||
        sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS) < 0 ||
        sceNetInit(&net_param) < 0 ||
        sceHttpInit(128 * 1024) < 0) {
        return -1;
    }

    http_template = sceHttpCreateTemplate(BROWSER_USER_AGENT,
                                           SCE_HTTP_VERSION_1_1, 0);
    if (http_template < 0) {
        sceHttpTerm();
        sceNetTerm();
        return -1;
    }

    http_ready = 1;
    return 0;
}

static int http_request(const char *url, const char *method, const char *body,
                        const char *token, char *response,
                        unsigned int response_size)
{
    int connection;
    int request;
    int status;
    int total = 0;
    int received;

    if (!http_ready || !url || !response || response_size < 2) {
        return -1;
    }

    vt_log(VT_LOG_DEBUG, "[HTTP] %s %s", method, url);
    connection = sceHttpCreateConnectionWithURL(http_template, url, 0);
    if (connection < 0) {
        vt_log(VT_LOG_ERROR, "[HTTP] Error creando conexion: %d", connection);
        return connection;
    }
    request = sceHttpCreateRequestWithURL(connection,
                                          strcmp(method, "POST") == 0
                                              ? SCE_HTTP_METHOD_POST
                                              : SCE_HTTP_METHOD_GET,
                                          url, body ? (unsigned int)strlen(body)
                                                    : 0);
    if (request < 0) {
        vt_log(VT_LOG_ERROR, "[HTTP] Error creando request: %d", request);
        sceHttpDeleteConnection(connection);
        return request;
    }

    sceHttpSetAutoRedirect(request, 1);
    sceHttpAddRequestHeader(request, "User-Agent", BROWSER_USER_AGENT, SCE_HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(request, "Accept", "*/*", SCE_HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(request, "Range", "bytes=0-", SCE_HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(request, "Connection", "keep-alive", SCE_HTTP_HEADER_OVERWRITE);
    sceHttpSetConnectTimeOut(request, 15000000);
    sceHttpSetSendTimeOut(request, 15000000);
    sceHttpSetRecvTimeOut(request, 35000000);

    sceHttpAddRequestHeader(request, "Accept", "application/json", 0);
    if (body) {
        sceHttpAddRequestHeader(request, "Content-Type",
                                "application/json", 0);
    }
    if (token && token[0]) {
        char authorization[1024];
        snprintf(authorization, sizeof(authorization), "Bearer %s", token);
        sceHttpAddRequestHeader(request, "Authorization", authorization, 0);
    }
    status = sceHttpSendRequest(request, body, body ? strlen(body) : 0);
    if (status >= 0) {
        sceHttpGetStatusCode(request, &status);
    }
    vt_log(VT_LOG_DEBUG, "[HTTP] status=%d url=%s", status, url);
    if (status >= 0 && status < 400) {
        do {
            received = sceHttpReadData(request, response + total,
                                       response_size - 1 - total);
            if (received > 0) total += received;
        } while (received > 0 && total < (int)response_size - 1);
        response[total >= 0 && total < (int)response_size ? total : 0] = '\0';
    } else {
        vt_log(VT_LOG_ERROR, "[HTTP] respuesta fallida status=%d (0x%08X) url=%s",
               status, (unsigned int)status, url);
        response[0] = '\0';
        total = (status < 0) ? status : -status;
    }

    sceHttpDeleteRequest(request);
    sceHttpDeleteConnection(connection);
    return (total > 0) ? total : (total < 0 ? total : -1);
}

int http_get(const char *url, char *response, unsigned int response_size)
{
    return http_request(url, "GET", NULL, NULL, response, response_size);
}

int http_get_auth(const char *url, const char *token, char *response,
                  unsigned int response_size)
{
    return http_request(url, "GET", NULL, token, response, response_size);
}

int http_post_json(const char *url, const char *body, const char *token,
                   char *response, unsigned int response_size)
{
    return http_request(url, "POST", body, token, response, response_size);
}

int http_get_binary(const char *url, void *response, unsigned int response_size)
{
    int connection;
    int request;
    int status;
    int total = 0;
    int received;

    if (!http_ready || !url || !response || response_size == 0) return -1;
    vt_log(VT_LOG_DEBUG, "[HTTP] GET binario %s", url);
    connection = sceHttpCreateConnectionWithURL(http_template, url, 0);
    if (connection < 0) {
        vt_log(VT_LOG_ERROR, "[HTTP] Error creando conexion binaria: %d", connection);
        return connection;
    }
    request = sceHttpCreateRequestWithURL(connection, SCE_HTTP_METHOD_GET,
                                          url, 0);
    if (request < 0) {
        sceHttpDeleteConnection(connection);
        return request;
    }
    sceHttpSetAutoRedirect(request, 1);
    sceHttpAddRequestHeader(request, "User-Agent", BROWSER_USER_AGENT, SCE_HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(request, "Accept", "*/*", SCE_HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(request, "Range", "bytes=0-", SCE_HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(request, "Connection", "keep-alive", SCE_HTTP_HEADER_OVERWRITE);
    sceHttpSetConnectTimeOut(request, 15000000);
    sceHttpSetSendTimeOut(request, 15000000);
    sceHttpSetRecvTimeOut(request, 25000000);
    status = sceHttpSendRequest(request, NULL, 0);
    if (status >= 0) sceHttpGetStatusCode(request, &status);
    vt_log(VT_LOG_DEBUG, "[HTTP] status=%d url=%s", status, url);
    if (status >= 0 && status < 400) {
        do {
            received = sceHttpReadData(request, (char *)response + total,
                                       response_size - total);
            if (received > 0) total += received;
        } while (received > 0 && total < (int)response_size);
    } else {
        total = -1;
    }
    sceHttpDeleteRequest(request);
    sceHttpDeleteConnection(connection);
    return total;
}

int http_download_file(const char *url, const char *dest_path, volatile int *stop_flag, volatile unsigned int *bytes_downloaded)
{
    int connection;
    int request;
    int status;
    unsigned int total = 0;
    int received;
    FILE *fp;
    static char chunk_buf[64 * 1024];

    if (!http_ready || !url || !dest_path) return -1;

    vt_log(VT_LOG_INFO, "[HTTP] Descargando stream continuo desde '%s' a %s", url, dest_path);
    fp = fopen(dest_path, "wb");
    if (!fp) {
        vt_log(VT_LOG_ERROR, "[HTTP] No se pudo abrir %s para escritura", dest_path);
        return -1;
    }

    connection = sceHttpCreateConnectionWithURL(http_template, url, 0);
    if (connection < 0) {
        vt_log(VT_LOG_ERROR, "[HTTP] Error conexion stream: %d", connection);
        fclose(fp);
        return connection;
    }

    request = sceHttpCreateRequestWithURL(connection, SCE_HTTP_METHOD_GET, url, 0);
    if (request < 0) {
        vt_log(VT_LOG_ERROR, "[HTTP] Error request stream: %d", request);
        sceHttpDeleteConnection(connection);
        fclose(fp);
        return request;
    }

    sceHttpSetAutoRedirect(request, 1);
    sceHttpAddRequestHeader(request, "User-Agent", BROWSER_USER_AGENT, SCE_HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(request, "Accept", "*/*", SCE_HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(request, "Range", "bytes=0-", SCE_HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(request, "Connection", "keep-alive", SCE_HTTP_HEADER_OVERWRITE);
    sceHttpSetConnectTimeOut(request, 15000000);
    sceHttpSetSendTimeOut(request, 15000000);
    sceHttpSetRecvTimeOut(request, 30000000);

    status = sceHttpSendRequest(request, NULL, 0);
    if (status >= 0) sceHttpGetStatusCode(request, &status);
    vt_log(VT_LOG_INFO, "[HTTP] Descarga stream iniciada. status=%d", status);

    if (status >= 200 && status < 400) {
        while (!stop_flag || !(*stop_flag)) {
            received = sceHttpReadData(request, chunk_buf, sizeof(chunk_buf));
            if (received <= 0) break;

            size_t written = fwrite(chunk_buf, 1, received, fp);
            if (written > 0) {
                total += (unsigned int)written;
                if (bytes_downloaded) *bytes_downloaded = total;
                fflush(fp);
            }
        }
    } else {
        total = 0;
    }

    fclose(fp);
    sceHttpDeleteRequest(request);
    sceHttpDeleteConnection(connection);
    return (int)total;
}

void http_shutdown(void)
{
    if (http_template >= 0) sceHttpDeleteTemplate(http_template);
    if (http_ready) {
        sceHttpTerm();
        sceNetTerm();
    }
    sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTPS);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTP);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    http_template = -1;
    http_ready = 0;
}
