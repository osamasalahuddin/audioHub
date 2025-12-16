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
#include "freertos/queue.h"

#define TAG "UDP_AUDIO"
#define MAX_AUDIO_PACKET_SIZE       1400        /* Maximum UDP payload per channel */
#define AUDIO_QUEUE_SIZE            20          /* Queue depth - increased for split packets */
#define UDP_TASK_STACK_SIZE         4096
#define UDP_TASK_PRIORITY           5

/* Audio packet header */
typedef struct
{
    uint32_t sequence_num;
    uint32_t timestamp;
    uint16_t data_len;
    uint16_t flags;
} __attribute__((packed)) udp_audio_header_t;

/* Channel identifier flags */
#define CHANNEL_FLAG_LEFT   0x01
#define CHANNEL_FLAG_RIGHT  0x02

/* Queue message structure - optimized for 1400 byte chunks */
typedef struct
{
    uint8_t data[MAX_AUDIO_PACKET_SIZE];    /* 1400 bytes max */
    uint16_t len;
    uint8_t channel;                         /* LEFT or RIGHT */
    uint8_t reserved;
} audio_queue_msg_t;

/* Static buffers for packet construction */
static uint8_t s_left_packet_buffer[MAX_AUDIO_PACKET_SIZE + sizeof(udp_audio_header_t)];
static uint8_t s_right_packet_buffer[MAX_AUDIO_PACKET_SIZE + sizeof(udp_audio_header_t)];

/* Temporary buffers for channel separation (used in callback) */
static uint8_t s_temp_left_buffer[4096];
static uint8_t s_temp_right_buffer[4096];

/* Socket handles */
static int s_udp_socket_left = -1;
static int s_udp_socket_right = -1;

/* Destination addresses */
static struct sockaddr_in s_dest_addr_left;
static struct sockaddr_in s_dest_addr_right;

/* Queue and task handles */
static QueueHandle_t s_audio_queue = NULL;
static TaskHandle_t s_udp_task_handle = NULL;

/* Sequence numbers */
static uint32_t s_sequence_num_left = 0;
static uint32_t s_sequence_num_right = 0;

/* Statistics */
static uint32_t s_packets_sent_left = 0;
static uint32_t s_packets_sent_right = 0;
static uint32_t s_packets_dropped = 0;

/* State flags */
static bool s_initialized = false;
static bool s_task_running = false;

/* Forward declarations */
static void udp_audio_task(void *pvParameters);
static esp_err_t udp_send_packet(const uint8_t *data, uint16_t len, bool is_left_channel);

esp_err_t udp_audio_init(const char *dest_ip, uint16_t left_port, uint16_t right_port)
{
    if (s_initialized)
    {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    /* Create UDP socket for LEFT channel */
    s_udp_socket_left = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_socket_left < 0)
    {
        ESP_LOGE(TAG, "Failed to create left socket: errno %d", errno);
        return ESP_FAIL;
    }

    /* Create UDP socket for RIGHT channel */
    s_udp_socket_right = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_socket_right < 0)
    {
        ESP_LOGE(TAG, "Failed to create right socket: errno %d", errno);
        close(s_udp_socket_left);
        s_udp_socket_left = -1;
        return ESP_FAIL;
    }

    /* Increase socket send buffers */
    int sndbuf_size = 16384;
    setsockopt(s_udp_socket_left, SOL_SOCKET, SO_SNDBUF,
               &sndbuf_size, sizeof(sndbuf_size));
    setsockopt(s_udp_socket_right, SOL_SOCKET, SO_SNDBUF,
               &sndbuf_size, sizeof(sndbuf_size));

    /* Set non-blocking mode for both sockets */
    int flags = fcntl(s_udp_socket_left, F_GETFL, 0);
    fcntl(s_udp_socket_left, F_SETFL, flags | O_NONBLOCK);

    flags = fcntl(s_udp_socket_right, F_GETFL, 0);
    fcntl(s_udp_socket_right, F_SETFL, flags | O_NONBLOCK);

    /* Configure LEFT channel destination */
    memset(&s_dest_addr_left, 0, sizeof(s_dest_addr_left));
    s_dest_addr_left.sin_family = AF_INET;
    s_dest_addr_left.sin_port = htons(left_port);
    s_dest_addr_left.sin_addr.s_addr = inet_addr(dest_ip);

    /* Configure RIGHT channel destination */
    memset(&s_dest_addr_right, 0, sizeof(s_dest_addr_right));
    s_dest_addr_right.sin_family = AF_INET;
    s_dest_addr_right.sin_port = htons(right_port);
    s_dest_addr_right.sin_addr.s_addr = inet_addr(dest_ip);

    /* Create audio queue - increased size for split packets */
    s_audio_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(audio_queue_msg_t));
    if (s_audio_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create audio queue");
        close(s_udp_socket_left);
        close(s_udp_socket_right);
        s_udp_socket_left = -1;
        s_udp_socket_right = -1;
        return ESP_FAIL;
    }

    /* Reset statistics */
    s_sequence_num_left = 0;
    s_sequence_num_right = 0;
    s_packets_sent_left = 0;
    s_packets_sent_right = 0;
    s_packets_dropped = 0;

    s_initialized = true;

    ESP_LOGI(TAG, "UDP audio initialized: %s (L:%d R:%d)", dest_ip, left_port, right_port);
    ESP_LOGI(TAG, "Queue size: %d items x %d bytes = %d bytes total",
             AUDIO_QUEUE_SIZE, sizeof(audio_queue_msg_t),
             AUDIO_QUEUE_SIZE * sizeof(audio_queue_msg_t));

    return ESP_OK;
}

esp_err_t udp_queue_audio_data(const uint8_t *data, uint32_t len)
{
    if (!s_initialized || s_audio_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* Step 1: De-interleave stereo PCM into separate L/R buffers
     * Input: [L0][R0][L1][R1][L2][R2]... (16-bit stereo)
     * Output: left_buffer = [L0][L1][L2]..., right_buffer = [R0][R1][R2]...
     */
    uint32_t num_samples = len / 4;  /* 4 bytes per stereo frame */
    uint32_t channel_len = num_samples * 2;  /* 2 bytes per mono sample */

    if (channel_len > sizeof(s_temp_left_buffer))
    {
        ESP_LOGW(TAG, "Data too large: %lu bytes, truncating", len);
        channel_len = sizeof(s_temp_left_buffer);
        num_samples = channel_len / 2;
    }

    /* De-interleave into temporary buffers */
    int16_t *stereo = (int16_t *)data;
    int16_t *left = (int16_t *)s_temp_left_buffer;
    int16_t *right = (int16_t *)s_temp_right_buffer;

    for (uint32_t i = 0; i < num_samples; i++)
    {
        left[i] = stereo[i * 2];        /* Extract left channel */
        right[i] = stereo[i * 2 + 1];   /* Extract right channel */
    }

    /* Step 2: Split each channel into MAX_AUDIO_PACKET_SIZE chunks and queue */
    esp_err_t result = ESP_OK;

    /* Queue LEFT channel chunks */
    for (uint32_t offset = 0; offset < channel_len; offset += MAX_AUDIO_PACKET_SIZE)
    {
        uint16_t chunk_len = (channel_len - offset) > MAX_AUDIO_PACKET_SIZE ?
                              MAX_AUDIO_PACKET_SIZE : (channel_len - offset);

        audio_queue_msg_t msg;
        memcpy(msg.data, s_temp_left_buffer + offset, chunk_len);
        msg.len = chunk_len;
        msg.channel = CHANNEL_FLAG_LEFT;
        msg.reserved = 0;

        if (xQueueSend(s_audio_queue, &msg, 0) != pdTRUE)
        {
            s_packets_dropped++;
            result = ESP_ERR_TIMEOUT;
            break;  /* Queue full, stop queuing */
        }
    }

    /* Queue RIGHT channel chunks */
    for (uint32_t offset = 0; offset < channel_len; offset += MAX_AUDIO_PACKET_SIZE)
    {
        uint16_t chunk_len = (channel_len - offset) > MAX_AUDIO_PACKET_SIZE ?
                              MAX_AUDIO_PACKET_SIZE : (channel_len - offset);

        audio_queue_msg_t msg;
        memcpy(msg.data, s_temp_right_buffer + offset, chunk_len);
        msg.len = chunk_len;
        msg.channel = CHANNEL_FLAG_RIGHT;
        msg.reserved = 0;

        if (xQueueSend(s_audio_queue, &msg, 0) != pdTRUE)
        {
            s_packets_dropped++;
            result = ESP_ERR_TIMEOUT;
            break;  /* Queue full, stop queuing */
        }
    }

    return result;
}

static void udp_audio_task(void *pvParameters)
{
    audio_queue_msg_t msg;
    uint32_t stats_counter = 0;

    ESP_LOGI(TAG, "UDP audio task started");

    while (s_task_running)
    {
        /* Wait for audio data from queue */
        if (xQueueReceive(s_audio_queue, &msg, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            /* Data is already split and chunked - just send it */
            bool is_left = (msg.channel == CHANNEL_FLAG_LEFT);

            esp_err_t err = udp_send_packet(msg.data, msg.len, is_left);

            if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
            {
                /* Log only non-timeout errors */
                static uint32_t err_count = 0;
                if (++err_count % 50 == 0)
                {
                    ESP_LOGW(TAG, "%s send error, count: %lu",
                             is_left ? "LEFT" : "RIGHT", err_count);
                }
            }

            /* Periodic statistics logging */
            if (++stats_counter % 200 == 0)
            {
                ESP_LOGI(TAG, "Sent - L:%lu R:%lu Dropped:%lu Queue:%d",
                         s_packets_sent_left, s_packets_sent_right,
                         s_packets_dropped, uxQueueMessagesWaiting(s_audio_queue));
            }
        }
    }

    ESP_LOGI(TAG, "UDP audio task stopped");
    vTaskDelete(NULL);
}

static esp_err_t udp_send_packet(const uint8_t *data, uint16_t len, bool is_left_channel)
{
    int socket;
    struct sockaddr_in *dest_addr;
    uint8_t *packet_buffer;
    uint32_t *sequence_num;
    uint32_t *packets_sent;

    /* Select left or right channel parameters */
    if (is_left_channel)
    {
        socket = s_udp_socket_left;
        dest_addr = &s_dest_addr_left;
        packet_buffer = s_left_packet_buffer;
        sequence_num = &s_sequence_num_left;
        packets_sent = &s_packets_sent_left;
    }
    else
    {
        socket = s_udp_socket_right;
        dest_addr = &s_dest_addr_right;
        packet_buffer = s_right_packet_buffer;
        sequence_num = &s_sequence_num_right;
        packets_sent = &s_packets_sent_right;
    }

    /* Build header */
    udp_audio_header_t *header = (udp_audio_header_t *)packet_buffer;
    header->sequence_num = (*sequence_num)++;
    header->timestamp = xTaskGetTickCount();
    header->data_len = len;
    header->flags = is_left_channel ? CHANNEL_FLAG_LEFT : CHANNEL_FLAG_RIGHT;

    /* Copy audio data */
    memcpy(packet_buffer + sizeof(udp_audio_header_t), data, len);

    /* Send packet */
    int total_len = sizeof(udp_audio_header_t) + len;
    int sent = sendto(socket, packet_buffer, total_len, 0,
                      (struct sockaddr *)dest_addr, sizeof(struct sockaddr_in));

    if (sent < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM)
        {
            /* Buffer full - expected in real-time streaming */
            return ESP_ERR_TIMEOUT;
        }
        return ESP_FAIL;
    }

    (*packets_sent)++;
    return ESP_OK;
}

esp_err_t udp_audio_task_start(void)
{
    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task_running)
    {
        ESP_LOGW(TAG, "Task already running");
        return ESP_OK;
    }

    s_task_running = true;

    /* Create UDP transmission task */
    BaseType_t ret = xTaskCreate(udp_audio_task,
                                  "udp_audio",
                                  UDP_TASK_STACK_SIZE,
                                  NULL,
                                  UDP_TASK_PRIORITY,
                                  &s_udp_task_handle);

    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create UDP task");
        s_task_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UDP audio task started");
    return ESP_OK;
}

void udp_audio_task_stop(void)
{
    if (s_task_running)
    {
        s_task_running = false;
        vTaskDelay(pdMS_TO_TICKS(200));
        s_udp_task_handle = NULL;
        ESP_LOGI(TAG, "UDP audio task stopped");
    }
}

void udp_audio_deinit(void)
{
    udp_audio_task_stop();

    if (s_udp_socket_left >= 0)
    {
        close(s_udp_socket_left);
        s_udp_socket_left = -1;
    }
    if (s_udp_socket_right >= 0)
    {
        close(s_udp_socket_right);
        s_udp_socket_right = -1;
    }

    if (s_audio_queue != NULL)
    {
        vQueueDelete(s_audio_queue);
        s_audio_queue = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "UDP audio deinitialized");
}

void udp_audio_get_stats(uint32_t *sent_left, uint32_t *sent_right, uint32_t *dropped)
{
    if (sent_left) *sent_left = s_packets_sent_left;
    if (sent_right) *sent_right = s_packets_sent_right;
    if (dropped) *dropped = s_packets_dropped;
}
