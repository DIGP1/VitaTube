#ifndef VIDEO_PLAYER_H
#define VIDEO_PLAYER_H

#include <stdint.h>
#include <vita2d.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VideoPlayerStatus {
    VPLAYER_IDLE = 0,
    VPLAYER_BUFFERING,
    VPLAYER_PLAYING,
    VPLAYER_PAUSED,
    VPLAYER_COMPLETED,
    VPLAYER_ERROR
} VideoPlayerStatus;

/**
 * Inicializa el subsistema de reproduccion de video y carga el modulo SCE_SYSMODULE_AVPLAYER.
 */
int video_player_init(void);

/**
 * Cierra todos los recursos del reproductor.
 */
void video_player_shutdown(void);

/**
 * Abre un stream de video desde una URL directa (H.264/AAC MP4).
 * Inicia la descarga progresiva en un hilo de background y el reproductor de hardware SceAvPlayer.
 */
int video_player_open(const char *stream_url);

/**
 * Detiene la reproduccion y cancela cualquier descarga activa.
 */
void video_player_close(void);

/**
 * Control de reproduccion
 */
void video_player_play(void);
void video_player_pause(void);
void video_player_toggle_pause(void);
void video_player_seek(int64_t delta_seconds);
void video_player_seek_to(uint64_t target_ms);
void video_player_set_volume(unsigned int volume); // 0 a 100

/**
 * Informacion de estado
 */
VideoPlayerStatus video_player_get_status(void);
uint64_t video_player_get_current_time(void); // En milisegundos
uint64_t video_player_get_duration(void);     // En milisegundos
uint32_t video_player_get_buffered_bytes(void);
int video_player_has_frame(void);

/**
 * Bucle de procesamiento por frame (debe llamarse en home_update())
 */
void video_player_update(void);

/**
 * Renderizado del cuadro de video decodificado actual usando vita2d
 * @param x Coordenada X superior izquierda
 * @param y Coordenada Y superior izquierda
 * @param width Ancho destino
 * @param height Alto destino
 */
void video_player_draw(int x, int y, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_PLAYER_H */
