/* tcp_audio.h */
#ifndef TCP_AUDIO_H
#define TCP_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Initialize TCP connection for audio transmission */
esp_err_t tcp_audio_init(const char *dest_ip, uint16_t dest_port);

/* Send audio data via TCP */
esp_err_t tcp_send_audio(const uint8_t *data, uint32_t len);

/* Check if TCP connection is active */
bool tcp_audio_is_connected(void);

/* Deinitialize TCP connection */
void tcp_audio_deinit(void);

esp_err_t tcp_reconnect(void);

#endif /* TCP_AUDIO_H */