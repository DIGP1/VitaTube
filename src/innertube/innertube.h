#ifndef VITATUBE_INNERTUBE_H
#define VITATUBE_INNERTUBE_H

#include <stddef.h>
#include "../auth/auth.h"
#include "../youtube/youtube.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Browse IDs estandar de InnerTube */
#define INNERTUBE_BROWSE_HOME          "FEwhat_to_watch"
#define INNERTUBE_BROWSE_TRENDING      "FEtrending"
#define INNERTUBE_BROWSE_SUBSCRIPTIONS "FEsubscriptions"
#define INNERTUBE_BROWSE_SHORTS        "FEshorts"

typedef enum {
    IT_CLIENT_TV = 0, /* Cliente TV ("TVHTML5") */
    IT_CLIENT_WEB     /* Cliente Escritorio ("WEB") */
} InnerTubeClientType;

typedef enum {
    IT_OK = 0,
    IT_ERR_NETWORK      = -1,
    IT_ERR_JSON         = -2,
    IT_ERR_UNAUTHORIZED = -3,
    IT_ERR_NO_MEM       = -4
} InnerTubeStatus;

/*
 * Funcion base para hacer peticiones POST a cualquier endpoint de InnerTube
 * (ej. "browse", "search", "next").
 * Inyecta automaticamente `Authorization: Bearer <access_token>` si el usuario inicio sesion,
 * arma el bloque "context" ("TV" o "WEB"), y libera el buffer crudo de red inmediatamente
 * despues de construir el arbol `json_t *` con jansson.
 * IMPORTANTE: El llamador debe liberar `*out_root` con `json_decref(*out_root)`.
 */
InnerTubeStatus innertube_post_endpoint(const char *endpoint,
                                        AuthSession *session,
                                        InnerTubeClientType client_type,
                                        const char *hl,
                                        const char *gl,
                                        const char *browse_id,
                                        const char *search_query,
                                        const char *params,
                                        void **out_json_root);

/*
 * Consulta https://www.youtube.com/youtubei/v1/browse (Home, Suscripciones, Tendencias o Shorts),
 * extrae los videos en `out_videos` y libera inmediatamente toda la memoria JSON de jansson.
 */
InnerTubeStatus innertube_browse(AuthSession *session,
                                 InnerTubeClientType client_type,
                                 const char *browse_id,
                                 const char *hl,
                                 const char *gl,
                                 YoutubeVideo *out_videos,
                                 unsigned int capacity,
                                 unsigned int *out_count);

/*
 * Consulta https://www.youtube.com/youtubei/v1/search para buscar videos o Shorts directamente
 * en InnerTube y libera inmediatamente los nodos JSON de jansson.
 */
InnerTubeStatus innertube_search(AuthSession *session,
                                 InnerTubeClientType client_type,
                                 const char *query,
                                 const char *hl,
                                 const char *gl,
                                 YoutubeVideo *out_videos,
                                 unsigned int capacity,
                                 unsigned int *out_count);

InnerTubeStatus innertube_search_shorts(AuthSession *session,
                                        const char *query,
                                        const char *hl,
                                        const char *gl,
                                        YoutubeVideo *out_videos,
                                        unsigned int capacity,
                                        unsigned int *out_count);

/*
 * Consulta https://www.youtube.com/youtubei/v1/next para obtener el canal autor del video/Short,
 * su avatar (imagen de perfil) y la lista de comentarios reales desde InnerTube, liberando toda
 * la memoria JSON al instante.
 */
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
                                       unsigned int *out_count);

#ifdef __cplusplus
}
#endif

#endif /* VITATUBE_INNERTUBE_H */
