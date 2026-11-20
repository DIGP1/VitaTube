#ifndef VITATUBE_VPS_H
#define VITATUBE_VPS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Endpoint oficial del VPS en Oracle Cloud */
#define VPS_ORACLE_URL   "http://147.224.168.110:9000/"
#define VPS_FALLBACK_URL "http://vitatubeextractor.duckdns.org:9000/"

/*
 * Envia un POST a http://147.224.168.110:9000/ con el payload:
 * {"url": "https://www.youtube.com/watch?v=[video_id]", "videoQuality": "720", "youtubeVideoCodec": "h264"}
 * Extrae la URL final cuando el servidor devuelve {"status": "redirect", "url": "..."}.
 *
 * @param video_id ID del video de YouTube (ej. "dQw4w9WgXcQ").
 * @return Cadena alojada con malloc/strdup con la URL final del stream .mp4 (liberar con free()),
 *         o NULL si ocurre un error de red o parseo.
 */
char *get_video_url_from_vps(const char *video_id);

/*
 * Variante con soporte para calidad configurable ("720", "480", "360") y buffer preasignado.
 * Devuelve 0 en caso de exito, o un valor negativo en caso de error.
 */
int vps_get_video_url_quality(const char *video_id, const char *quality,
                              char *out_url, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* VITATUBE_VPS_H */
