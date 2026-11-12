#include "history.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define HISTORY_FILE "ux0:data/VitaTube/history.dat"
#define HISTORY_MAGIC "VTH1"

static YoutubeVideo history_videos[HISTORY_MAX];
static unsigned int history_total = 0;

static void history_save_file(void)
{
    mkdir("ux0:data/VitaTube", 0777);
    FILE *file = fopen(HISTORY_FILE, "wb");
    if (!file) return;

    fwrite(HISTORY_MAGIC, 1, 4, file);
    fwrite(&history_total, sizeof(unsigned int), 1, file);
    if (history_total > 0) {
        fwrite(history_videos, sizeof(YoutubeVideo), history_total, file);
    }
    fclose(file);
}

int history_init(void)
{
    char magic[4];
    unsigned int count = 0;
    FILE *file = fopen(HISTORY_FILE, "rb");

    history_total = 0;
    memset(history_videos, 0, sizeof(history_videos));

    if (!file) {
        return 0;
    }

    if (fread(magic, 1, 4, file) != 4 || memcmp(magic, HISTORY_MAGIC, 4) != 0) {
        fclose(file);
        return -1;
    }

    if (fread(&count, sizeof(unsigned int), 1, file) != 1) {
        fclose(file);
        return -1;
    }

    if (count > HISTORY_MAX) {
        count = HISTORY_MAX;
    }

    if (count > 0) {
        size_t read_items = fread(history_videos, sizeof(YoutubeVideo), count, file);
        history_total = (unsigned int)read_items;
    }

    fclose(file);
    vt_log(VT_LOG_INFO, "[Historial] Cargados %u videos guardados", history_total);
    return (int)history_total;
}

void history_shutdown(void)
{
    history_save_file();
}

int history_add(const YoutubeVideo *video)
{
    int existing_index = -1;
    unsigned int i;

    if (!video || !video->id[0]) {
        return -1;
    }

    for (i = 0; i < history_total; ++i) {
        if (strcmp(history_videos[i].id, video->id) == 0) {
            existing_index = (int)i;
            break;
        }
    }

    if (existing_index >= 0) {
        /* Desplazar elementos anteriores hacia abajo para mover el existente a la cima */
        YoutubeVideo current = history_videos[existing_index];
        for (i = (unsigned int)existing_index; i > 0; --i) {
            history_videos[i] = history_videos[i - 1];
        }
        history_videos[0] = current;
    } else {
        /* Insertar nuevo al inicio desplazando los existentes */
        unsigned int limit = history_total < HISTORY_MAX ? history_total : HISTORY_MAX - 1;
        for (i = limit; i > 0; --i) {
            history_videos[i] = history_videos[i - 1];
        }
        history_videos[0] = *video;
        if (history_total < HISTORY_MAX) {
            ++history_total;
        }
    }

    history_save_file();
    vt_log(VT_LOG_INFO, "[Historial] Agregado video id=%s total=%u", video->id, history_total);
    return 0;
}

unsigned int history_count(void)
{
    return history_total;
}

const YoutubeVideo *history_get(unsigned int index)
{
    if (index >= history_total) {
        return NULL;
    }
    return &history_videos[index];
}

void history_clear(void)
{
    history_total = 0;
    memset(history_videos, 0, sizeof(history_videos));
    remove(HISTORY_FILE);
    history_save_file();
    vt_log(VT_LOG_INFO, "[Historial] Historial vaciado");
}
