#ifndef YOUTUBE_H
#define YOUTUBE_H

#include "http.h"
#include "stream_api.h"

typedef struct YoutubeVideo {
    char id[32];
    char title[128];
    char thumbnail[512];
    char channel[96];
    unsigned int duration;
    unsigned long views;
    int is_live;
} YoutubeVideo;

int youtube_init(void);
void youtube_shutdown(void);
int youtube_login(const char *username, const char *password);
int youtube_register(const char *username, const char *password);
int youtube_is_logged_in(void);
void youtube_logout(void);
int youtube_search(const char *query, char *response, unsigned int size);
int youtube_suggestions(const char *query, char suggestions[][64], unsigned int max_count);
int youtube_streams(const char *video_id, char *response, unsigned int size);
int youtube_select_stream(const char *json, char *url, unsigned int size,
                          unsigned int *quality);
int youtube_get_stream_url(const char *video_id, char *url, unsigned int size);
int youtube_trending(YoutubeVideo *videos, unsigned int capacity);
int youtube_subscriptions(YoutubeVideo *videos, unsigned int capacity);
int youtube_parse_videos(const char *json, YoutubeVideo *videos,
                         unsigned int capacity);
int youtube_download_thumbnail(const YoutubeVideo *video, void *buffer,
                               unsigned int size);
const char *youtube_region(void);
const char *youtube_username(void);
unsigned int youtube_max_quality(void);
void youtube_set_max_quality(unsigned int quality);
const char *youtube_quality_label(void);
void youtube_cycle_quality(void);
int youtube_autoplay(void);
void youtube_toggle_autoplay(void);
void youtube_save_config(void);

#endif
