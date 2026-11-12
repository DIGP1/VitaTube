#include "audio.h"
#include "log.h"
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <math.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_BLOCK_SAMPLES 512

enum AudioSoundType {
    SOUND_NONE = 0,
    SOUND_NAV = 1,
    SOUND_SELECT = 2,
    SOUND_CANCEL = 3
};

static int audio_port = -1;
static int audio_thread_id = -1;
static volatile int audio_thread_running = 0;
static volatile int audio_pending_sound = SOUND_NONE;

/* Buffers pregenerados en formato estereo 16-bit (L + R) */
static short buffer_nav[AUDIO_BLOCK_SAMPLES * 2];
static short buffer_select[AUDIO_BLOCK_SAMPLES * 4]; /* 1024 muestras = 2 bloques */
static short buffer_cancel[AUDIO_BLOCK_SAMPLES * 2];

static void synthesize_sounds(void)
{
    int i;
    const double pi = 3.14159265358979323846;

    /* Sonido de navegacion: pulso corto y suave a 750 Hz */
    for (i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        double t = (double)i / AUDIO_SAMPLE_RATE;
        double env = exp(-t * 350.0);
        double sample = sin(2.0 * pi * 750.0 * t) * env * 12000.0;
        short s16 = (short)sample;
        buffer_nav[i * 2] = s16;     /* Left */
        buffer_nav[i * 2 + 1] = s16; /* Right */
    }

    /* Sonido de seleccion: acorde suave (880 Hz a 1320 Hz) */
    for (i = 0; i < AUDIO_BLOCK_SAMPLES * 2; ++i) {
        double t = (double)i / AUDIO_SAMPLE_RATE;
        double env = exp(-t * 120.0);
        double freq = (i < AUDIO_BLOCK_SAMPLES) ? 880.0 : 1320.0;
        double sample = sin(2.0 * pi * freq * t) * env * 14000.0;
        short s16 = (short)sample;
        buffer_select[i * 2] = s16;
        buffer_select[i * 2 + 1] = s16;
    }

    /* Sonido de cancelar/volver: tono grave corto a 420 Hz */
    for (i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        double t = (double)i / AUDIO_SAMPLE_RATE;
        double env = exp(-t * 280.0);
        double sample = sin(2.0 * pi * 420.0 * t) * env * 11000.0;
        short s16 = (short)sample;
        buffer_cancel[i * 2] = s16;
        buffer_cancel[i * 2 + 1] = s16;
    }
}

static int audio_worker_thread(SceSize args, void *arg)
{
    (void)args;
    (void)arg;

    while (audio_thread_running) {
        int sound = audio_pending_sound;
        if (sound != SOUND_NONE) {
            audio_pending_sound = SOUND_NONE;
            if (audio_port >= 0) {
                if (sound == SOUND_NAV) {
                    sceAudioOutOutput(audio_port, buffer_nav);
                } else if (sound == SOUND_SELECT) {
                    sceAudioOutOutput(audio_port, buffer_select);
                    sceAudioOutOutput(audio_port, buffer_select + AUDIO_BLOCK_SAMPLES * 2);
                } else if (sound == SOUND_CANCEL) {
                    sceAudioOutOutput(audio_port, buffer_cancel);
                }
            }
        } else {
            sceKernelDelayThread(10000); /* 10 ms */
        }
    }
    return 0;
}

int audio_init(void)
{
    int vols[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };

    synthesize_sounds();

    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,
                                     AUDIO_BLOCK_SAMPLES,
                                     AUDIO_SAMPLE_RATE,
                                     SCE_AUDIO_OUT_MODE_STEREO);
    if (audio_port < 0) {
        /* Intento secundario con MAIN */
        audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN,
                                         AUDIO_BLOCK_SAMPLES,
                                         AUDIO_SAMPLE_RATE,
                                         SCE_AUDIO_OUT_MODE_STEREO);
    }

    if (audio_port < 0) {
        vt_log(VT_LOG_ERROR, "[Audio] No se pudo abrir puerto de audio: %d", audio_port);
        return audio_port;
    }

    sceAudioOutSetVolume(audio_port,
                         SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
                         vols);

    audio_thread_running = 1;
    audio_pending_sound = SOUND_NONE;
    audio_thread_id = sceKernelCreateThread("VitaTubeAudio",
                                            audio_worker_thread,
                                            0x10000100,
                                            32 * 1024,
                                            0,
                                            0,
                                            NULL);
    if (audio_thread_id >= 0) {
        sceKernelStartThread(audio_thread_id, 0, NULL);
    }

    vt_log(VT_LOG_INFO, "[Audio] Audio inicializado correctamente");
    return 0;
}

void audio_shutdown(void)
{
    if (audio_thread_id >= 0) {
        audio_thread_running = 0;
        sceKernelWaitThreadEnd(audio_thread_id, NULL, NULL);
        sceKernelDeleteThread(audio_thread_id);
        audio_thread_id = -1;
    }

    if (audio_port >= 0) {
        sceAudioOutReleasePort(audio_port);
        audio_port = -1;
    }
}

void audio_play_nav(void)
{
    audio_pending_sound = SOUND_NAV;
}

void audio_play_select(void)
{
    audio_pending_sound = SOUND_SELECT;
}

void audio_play_cancel(void)
{
    audio_pending_sound = SOUND_CANCEL;
}
