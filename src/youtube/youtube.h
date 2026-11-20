#ifndef YOUTUBE_H
#define YOUTUBE_H

#include "http.h"
#include "stream_api.h"
#include "../auth/auth.h"

typedef struct YoutubeVideo {
    char id[32];
    char title[128];
    char thumbnail[512];
    char channel[96];
    char published[48];
    unsigned int duration;
    unsigned long views;
    int is_live;
} YoutubeVideo;

typedef struct YoutubeComment {
    char author[64];
    char text[256];
    char likes[24];
} YoutubeComment;

int youtube_init(void);
void youtube_shutdown(void);

/* OAuth 2.0 Device Flow (Google / YouTube TV) */
AuthSession *youtube_auth_session(void);
int youtube_oauth_start(AuthDeviceCode *out_code);
int youtube_oauth_poll(const AuthDeviceCode *code, volatile int *cancel_flag);

int youtube_login(const char *username, const char *password);
int youtube_register(const char *username, const char *password);
int youtube_is_logged_in(void);
void youtube_logout(void);

/* Consultas a InnerTube (Home, Shorts, Tendencias, Suscripciones, Busqueda) */
int youtube_home(YoutubeVideo *videos, unsigned int capacity);
int youtube_shorts(YoutubeVideo *videos, unsigned int capacity);
int youtube_trending(YoutubeVideo *videos, unsigned int capacity);
int youtube_subscriptions(YoutubeVideo *videos, unsigned int capacity);
int youtube_search_videos(const char *query, YoutubeVideo *videos, unsigned int capacity);
int youtube_search_shorts(const char *query, YoutubeVideo *videos, unsigned int capacity);

/* Interacciones de canal, likes y comentarios (InnerTube + persistencia local) */
int youtube_is_channel_subscribed(const char *channel);
int youtube_toggle_channel_subscription(const char *channel);
int youtube_has_subscriptions(void);
int youtube_get_video_rating(const char *video_id);
void youtube_set_video_rating(const char *video_id, int rating);
int youtube_get_comments(const char *video_id,
                         char *out_channel, unsigned int channel_sz,
                         char *out_avatar_url, unsigned int avatar_sz,
                         YoutubeComment *out_comments, unsigned int max_comments);

int youtube_search(const char *query, char *response, unsigned int size);
int youtube_suggestions(const char *query, char suggestions[][64], unsigned int max_count);
int youtube_streams(const char *video_id, char *response, unsigned int size);
int youtube_select_stream(const char *json, char *url, unsigned int size,
                          unsigned int *quality);
int youtube_get_stream_url(const char *video_id, char *url, unsigned int size);
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
