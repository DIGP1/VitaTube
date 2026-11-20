#ifndef VITATUBE_AUTH_H
#define VITATUBE_AUTH_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITATUBE_AUTH_DIR   "ux0:data/vitatube"
#define VITATUBE_AUTH_FILE  "ux0:data/vitatube/auth.json"

/*
 * Client ID y Secret estandar de YouTube para TVs / Dispositivos (OAuth 2.0 Device Flow).
 * Permite autenticar cuentas de Google y consultar InnerTube (Home, Suscripciones, Shorts).
 */
#define YT_TV_CLIENT_ID     "861556708454-d6dlm3lh05idd8npek18k6be8ba3oc68.apps.googleusercontent.com"
#define YT_TV_CLIENT_SECRET "SboVhoG9s0rNafixCSGGKXAT"
#define YT_OAUTH_SCOPE      "http://gdata.youtube.com https://www.googleapis.com/auth/youtube"

typedef enum {
    AUTH_OK = 0,
    AUTH_ERR_NETWORK      = -1, /* Error de red / perdida de conexion Wi-Fi */
    AUTH_ERR_JSON         = -2, /* Fallo al parsear JSON con jansson */
    AUTH_ERR_DENIED       = -3, /* Acceso denegado por el usuario o credenciales */
    AUTH_ERR_EXPIRED      = -4, /* Codigo de dispositivo o refresh_token expirado */
    AUTH_ERR_CANCELLED    = -5, /* Polling cancelado por el usuario */
    AUTH_ERR_IO           = -6  /* Error al guardar/leer ux0:data/vitatube/auth.json */
} AuthStatus;

typedef struct {
    char device_code[256];
    char user_code[32];
    char verification_url[128];
    int  expires_in;
    int  interval;
} AuthDeviceCode;

typedef struct {
    char   access_token[512];
    char   refresh_token[256];
    time_t expires_at;
    int    logged_in;
} AuthSession;

/* Inicializa el subsistema de autenticacion y carga ux0:data/vitatube/auth.json si existe */
int auth_init(AuthSession *session);

/* Paso 1: Envia un POST a https://oauth2.googleapis.com/device/code y extrae codigos */
AuthStatus auth_request_device_code(AuthDeviceCode *out_code);

/* Paso 2: Bucle de polling (cada 5 segundos) a https://oauth2.googleapis.com/token */
AuthStatus auth_poll_for_token(const AuthDeviceCode *code,
                               AuthSession *out_session,
                               volatile int *cancel_flag);

/* Refresca el access_token usando el refresh_token almacenado */
AuthStatus auth_refresh_access_token(AuthSession *session);

/* Comprueba si el access_token caduco (o esta por caducar) y lo refresca automaticamente */
AuthStatus auth_ensure_valid_token(AuthSession *session);

/* Guarda/carga tokens de forma segura en ux0:data/vitatube/auth.json */
AuthStatus auth_save_session(const AuthSession *session);
AuthStatus auth_load_session(AuthSession *out_session);

/* Cierra sesion y elimina ux0:data/vitatube/auth.json */
void auth_clear_session(AuthSession *session);

#ifdef __cplusplus
}
#endif

#endif /* VITATUBE_AUTH_H */
