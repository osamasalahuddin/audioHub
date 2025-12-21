/* udp_audio.h */
#ifndef UDP_AUDIO_H
#define UDP_AUDIO_H

#include <stdint.h>
#include "esp_err.h"

/* Initialize UDP audio transmission with dual ports for stereo */
esp_err_t udp_audio_init(const char *dest_ip_left, uint16_t left_port,
                         const char *dest_ip_right, uint16_t right_port);

/* Queue stereo audio data for processing (called from BT callback) */
esp_err_t udp_queue_audio_data(const uint8_t *data, uint32_t len);

/* Start UDP transmission task */
esp_err_t udp_audio_task_start(void);

/* Stop UDP transmission task */
void udp_audio_task_stop(void);

/* Deinitialize UDP audio */
void udp_audio_deinit(void);

/* Get statistics */
void udp_audio_get_stats(uint32_t *sent_left, uint32_t *sent_right, uint32_t *dropped);

#endif /* UDP_AUDIO_H */
