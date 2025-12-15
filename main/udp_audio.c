/* udp_audio.c */
#include "udp_audio.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "UDP_AUDIO"
#define MAX_AUDIO_PACKET_SIZE 1400

typedef struct {
    uint32_t sequence_num;
    uint32_t timestamp;
    uint16_t data_len;
    uint16_t flags;
} __attribute__((packed)) udp_audio_header_t;

static uint8_t s_packet_buffer[MAX_AUDIO_PACKET_SIZE + sizeof(udp_audio_header_t)];
static int s_udp_socket = -1;
static struct sockaddr_in s_dest_addr;
static uint32_t s_sequence_num = 0;
static bool s_initialized = false;

esp_err_t udp_audio_init(const char *dest_ip, uint16_t dest_port)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* Create UDP socket */
    s_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        return ESP_FAIL;
    }


    /* Increase socket send buffer to handle bursts */
    int sndbuf_size = 16384;
    setsockopt(s_udp_socket, SOL_SOCKET, SO_SNDBUF,
               &sndbuf_size, sizeof(sndbuf_size));

    /* Non-blocking mode */
    int flags = fcntl(s_udp_socket, F_GETFL, 0);
    fcntl(s_udp_socket, F_SETFL, flags | O_NONBLOCK);

    /* Configure destination */
    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family = AF_INET;
    s_dest_addr.sin_port = htons(dest_port);
    s_dest_addr.sin_addr.s_addr = inet_addr(dest_ip);

    s_sequence_num = 0;
    s_initialized = true;

    ESP_LOGI(TAG, "UDP audio initialized: %s:%d", dest_ip, dest_port);
    return ESP_OK;
}

esp_err_t udp_send_audio(const uint8_t *data, uint32_t len)
{
    if (!s_initialized || s_udp_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (len > MAX_AUDIO_PACKET_SIZE) {
        len = MAX_AUDIO_PACKET_SIZE;
    }

    /* Build header */
    udp_audio_header_t *header = (udp_audio_header_t *)s_packet_buffer;
    header->sequence_num = s_sequence_num++;
    header->timestamp = xTaskGetTickCount();
    header->data_len = len;
    header->flags = 0;

    /* Copy audio data */
    memcpy(s_packet_buffer + sizeof(udp_audio_header_t), data, len);

    /* Send */
    int total_len = sizeof(udp_audio_header_t) + len;

    int sent = sendto(s_udp_socket, s_packet_buffer, total_len, 0,
                      (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));

    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM) {
            /* Buffer full - drop packet (expected for real-time streaming) */
            return ESP_ERR_TIMEOUT;
        }
        /* Only log real errors, not buffer full */
        static uint32_t err_count = 0;
        if (++err_count % 100 == 0) {
            ESP_LOGE(TAG, "Send failed: errno %d, count: %lu", errno, err_count);
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

void udp_audio_deinit(void)
{
    if (s_udp_socket >= 0) {
        close(s_udp_socket);
        s_udp_socket = -1;
    }
    s_initialized = false;
}