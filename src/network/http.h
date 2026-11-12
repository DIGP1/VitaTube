#ifndef HTTP_H
#define HTTP_H

#define HTTP_RESPONSE_MAX 65536

int http_init(void);
void http_shutdown(void);
int http_get(const char *url, char *response, unsigned int response_size);
int http_get_auth(const char *url, const char *token, char *response,
                  unsigned int response_size);
int http_post_json(const char *url, const char *body, const char *token,
                   char *response, unsigned int response_size);
int http_get_binary(const char *url, void *response, unsigned int response_size);
int http_download_file(const char *url, const char *dest_path, volatile int *stop_flag, volatile unsigned int *bytes_downloaded);

#endif
