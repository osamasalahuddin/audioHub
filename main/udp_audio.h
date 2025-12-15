/* udp_audio.h */
#ifndef UDP_AUDIO_H
#define UDP_AUDIO_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t udp_audio_init(const char *dest_ip, uint16_t dest_port);
esp_err_t udp_send_audio(const uint8_t *data, uint32_t len);
void udp_audio_deinit(void);

#endif