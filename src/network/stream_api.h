#ifndef STREAM_API_H
#define STREAM_API_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Dominio y puerto base de la API privada de extraccion de streams (VPS con yt-dlp + Cloudflare WARP).
 * Definida como macro para facilitar su modificacion o reemplazo.
 */
#define STREAM_API_BASE_URL "http://vitatubeextractor.duckdns.org:9000/"

/**
 * Realiza una petición POST a la API privada para obtener la URL directa de video (H.264/MP4).
 *
 * @param video_id_or_url ID del video de YouTube (ej. "dQw4w9WgXcQ") o URL completa.
 * @return Puntero a cadena con la URL directa alojada en el heap (malloc), o NULL si hubo un error.
 *         IMPORTANTE: El llamador es responsable de liberar la memoria con free().
 */
char *get_video_stream_url(const char *video_id_or_url);

/**
 * Versión segura para evitar fragmentación de heap (escribe en un buffer preasignado).
 *
 * @param video_id_or_url ID del video o URL completa.
 * @param out_url Buffer destino para la URL del stream.
 * @param out_size Tamaño del buffer (se recomiendan al menos 4096 bytes).
 * @return 0 si fue exitoso, código negativo si ocurrió algún error.
 */
int stream_api_get_url(const char *video_id_or_url, char *out_url, unsigned int out_size);

/**
 * Obtiene el stream especificando la calidad deseada (ej: "720", "480", "360", "240").
 */
int stream_api_get_url_quality(const char *video_id_or_url, char *out_url, unsigned int out_size, const char *quality);

/**
 * Configura la calidad predeterminada para las peticiones de stream.
 */
void stream_api_set_quality(const char *quality);

/**
 * Devuelve la calidad configurada actualmente.
 */
const char *stream_api_get_quality(void);

/**
 * Permite cambiar la URL base en tiempo de ejecución (ej. leída desde config.ini).
 * Si es NULL o vacía, se restablece el valor predeterminado (STREAM_API_BASE_URL).
 */
void stream_api_set_url(const char *url);

/**
 * Devuelve la URL base actualmente en uso.
 */
const char *stream_api_get_active_url(void);

#ifdef __cplusplus
}
#endif

#endif /* STREAM_API_H */
