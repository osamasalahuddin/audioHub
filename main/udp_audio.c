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
#include "freertos/semphr.h"

#define TAG "UDP_AUDIO"
#define SOCKET_SIZE                 16384       /* Worst Case when 3 packets are to be sent together*/
// #define SOCKET_SIZE                 32768       /* Worst Case when 3 packets are to be sent together*/
                                                /* Should never happen */
#define MAX_AUDIO_PACKET_SIZE       1200
#define MAX_CHANNEL_BUFFER_SIZE     4096        /* Max size per channel */
#define UDP_TASK_STACK_SIZE         4096
#define UDP_TASK_PRIORITY           3

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

/* Double buffering for thread safety */
typedef struct
{
    uint8_t left_channel[MAX_CHANNEL_BUFFER_SIZE];
    uint8_t right_channel[MAX_CHANNEL_BUFFER_SIZE];
    uint32_t left_len;
    uint32_t right_len;
} audio_buffer_set_t;


/* Send state tracking for chunked sending */
typedef struct
{
    uint32_t left_offset;       /* Current offset in left channel buffer */
    uint32_t right_offset;      /* Current offset in right channel buffer */
    bool left_complete;         /* All left chunks sent */
    bool right_complete;        /* All right chunks sent */
} send_state_t;

/* Static buffers for packet construction */
static uint8_t s_left_packet_buffer[MAX_AUDIO_PACKET_SIZE + sizeof(udp_audio_header_t)];
static uint8_t s_right_packet_buffer[MAX_AUDIO_PACKET_SIZE + sizeof(udp_audio_header_t)];

/* Double buffer sets - ping-pong between BT callback and UDP task */
static audio_buffer_set_t s_buffer_sets[2];
static uint8_t s_write_buffer_idx = 0;     /* Buffer being written by BT callback */
static uint8_t s_read_buffer_idx = 1;      /* Buffer being read by UDP task */

/* Send state for current buffer */
static send_state_t s_send_state = {0};

/* Socket handles */
static int s_udp_socket_left = -1;
static int s_udp_socket_right = -1;

/* Destination addresses */
static struct sockaddr_in s_dest_addr_left;
static struct sockaddr_in s_dest_addr_right;

/* Synchronization */
static SemaphoreHandle_t s_data_ready_sem = NULL;   /* Signals data ready to send */
static SemaphoreHandle_t s_buffer_mutex = NULL;     /* Protects buffer swap */
static TaskHandle_t s_udp_task_handle = NULL;

/* Sequence numbers */
uint32_t sequence_num = 0;

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

static esp_err_t udp_send_single_packet(const uint8_t *data, uint32_t offset,
                                        uint32_t total_len, bool is_left);
static esp_err_t udp_send_channel_chunked(const uint8_t *data, uint32_t len, bool is_left);

esp_err_t udp_audio_init(const char *dest_ip_left, uint16_t left_port,
                         const char *dest_ip_right, uint16_t right_port)
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
    int sndbuf_size = SOCKET_SIZE;
    setsockopt(s_udp_socket_left, SOL_SOCKET, SO_SNDBUF,
               &sndbuf_size, sizeof(sndbuf_size));
    setsockopt(s_udp_socket_right, SOL_SOCKET, SO_SNDBUF,
               &sndbuf_size, sizeof(sndbuf_size));

    /* Set non-blocking mode */
    int flags = fcntl(s_udp_socket_left, F_GETFL, 0);
    fcntl(s_udp_socket_left, F_SETFL, flags | O_NONBLOCK);

    flags = fcntl(s_udp_socket_right, F_GETFL, 0);
    fcntl(s_udp_socket_right, F_SETFL, flags | O_NONBLOCK);

    /* Configure destinations */
    memset(&s_dest_addr_left, 0, sizeof(s_dest_addr_left));
    s_dest_addr_left.sin_family = AF_INET;
    s_dest_addr_left.sin_port = htons(left_port);
    s_dest_addr_left.sin_addr.s_addr = inet_addr(dest_ip_left);

    memset(&s_dest_addr_right, 0, sizeof(s_dest_addr_right));
    s_dest_addr_right.sin_family = AF_INET;
    s_dest_addr_right.sin_port = htons(right_port);
    s_dest_addr_right.sin_addr.s_addr = inet_addr(dest_ip_right);

    /* Create synchronization primitives */
    s_data_ready_sem = xSemaphoreCreateBinary();
    if (s_data_ready_sem == NULL)
    {
        ESP_LOGE(TAG, "Failed to create semaphore");
        close(s_udp_socket_left);
        close(s_udp_socket_right);
        return ESP_FAIL;
    }

    s_buffer_mutex = xSemaphoreCreateMutex();
    if (s_buffer_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        vSemaphoreDelete(s_data_ready_sem);
        close(s_udp_socket_left);
        close(s_udp_socket_right);
        return ESP_FAIL;
    }

    /* Initialize buffer indices */
    s_write_buffer_idx = 0;
    s_read_buffer_idx = 1;

    /* Reset send state */
    memset(&s_send_state, 0, sizeof(s_send_state));

    /* Reset statistics */
    s_sequence_num_left = 0;
    s_sequence_num_right = 0;
    s_packets_sent_left = 0;
    s_packets_sent_right = 0;
    s_packets_dropped = 0;

    s_initialized = true;

    ESP_LOGI(TAG, "UDP audio initialized: L:%s::%d R:%s::%d)", dest_ip_left, left_port, dest_ip_right, right_port);
    ESP_LOGI(TAG, "Using direct buffer access - no queue, memory: 2 x %d bytes",
             sizeof(audio_buffer_set_t));

    return ESP_OK;
}

static uint32_t cb_counter = 0;
bool data_ready = false;
static uint32_t avg_bt_time = 0 ;
static uint32_t prev_bt_time = 0 ;
static uint32_t current_time = 0;
esp_err_t udp_queue_audio_data(const uint8_t *data, uint32_t len)
{
    current_time = esp_log_timestamp();
    if (data_ready)
    {
        /* Previous data not yet processed */
        return ESP_OK;;
    }

    data_ready = false;

    if (cb_counter == 0)
    {
        avg_bt_time = esp_log_timestamp();
    }
    else
    {
        float current_time = esp_log_timestamp();
        avg_bt_time = (avg_bt_time + (current_time - prev_bt_time)) / 2;
    }

    ESP_LOGI(TAG, "Pkt Time : %lu(ms)", current_time - prev_bt_time);
    prev_bt_time = current_time;

    /* 100 BT Packets receieved */
    if (++cb_counter % 100 == 0)
    {
        // static uint32_t free_heap;
        // uint32_t heap = esp_get_free_heap_size();

        // ESP_LOGI(TAG, "Current Time: %lu:%03lu(ms) Sent - L:%lu R:%lu Dropped:%lu Sequence:%lu Free:%lu Before:%lu",
        //             esp_log_timestamp()/1000,esp_log_timestamp()%1000,
        //             s_packets_sent_left, s_packets_sent_right, s_packets_dropped, sequence_num,
        //             heap, free_heap);

        // free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG, "Average BT Pkt Time : %lu(ms)", avg_bt_time);
        avg_bt_time = 0.0f;
        prev_bt_time = 0.0f ;
    }

    if (!s_initialized || s_data_ready_sem == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* Get write buffer pointer */
    audio_buffer_set_t *write_buf = &s_buffer_sets[s_write_buffer_idx];

    /* De-interleave stereo PCM into left and right channels
     * Input: [L0][R0][L1][R1][L2][R2]... (16-bit stereo)
     */
    uint32_t num_samples = len / 4;  /* 4 bytes per stereo frame */
    uint32_t channel_len = num_samples * 2;  /* 2 bytes per mono sample */

    if (channel_len > MAX_CHANNEL_BUFFER_SIZE)
    {
        ESP_LOGW(TAG, "Data too large: %lu bytes, truncating", len);
        channel_len = MAX_CHANNEL_BUFFER_SIZE;
        num_samples = channel_len / 2;
    }

    /* De-interleave directly into write buffer */
    int16_t *stereo = (int16_t *)data;
    int16_t *left = (int16_t *)write_buf->left_channel;
    int16_t *right = (int16_t *)write_buf->right_channel;

    for (uint32_t i = 0; i < num_samples; i++)
    {
        left[i] = stereo[i * 2];        /* Left channel */
        right[i] = stereo[i * 2 + 1];   /* Right channel */
    }

    write_buf->left_len = channel_len;
    write_buf->right_len = channel_len;

    /* Swap buffers (critical section) */
    // if (xSemaphoreTake(s_buffer_mutex, 0) == pdTRUE)
    // {
        /* Swap read/write buffer indices */
        uint8_t temp = s_write_buffer_idx;
        s_write_buffer_idx = s_read_buffer_idx;
        s_read_buffer_idx = temp;

        // xSemaphoreGive(s_buffer_mutex);

        /* Signal UDP task that data is ready */
        // xSemaphoreGive(s_data_ready_sem);

        data_ready = true;
        return ESP_OK;
    // }
    // else
    // {
    //     /* UDP task is still processing previous buffer - drop this packet */
    //     s_packets_dropped++;
    //     return ESP_ERR_TIMEOUT;
    // }
}

typedef enum {
    UDP_TX_CH_STATE_PKT_1,
    UDP_TX_CH_STATE_PKT_2,
    UDP_TX_CH_STATE_IDLE
}udp_tx_ch_state_t;

typedef enum {
    UDP_TX_CH_NONE,
    UDP_TX_CH_LEFT_FRONT,
    UDP_TX_CH_RIGHT_FRONT,
    UDP_TX_CH_LEFT_REAR,
    UDP_TX_CH_RIGHT_REAR,
}udp_tx_state_t;

static void udp_audio_task(void *pvParameters)
{
    uint32_t stats_counter = 0;
    audio_buffer_set_t *current_buf = NULL;
    udp_tx_state_t udp_tx_ch = UDP_TX_CH_NONE;
    udp_tx_ch_state_t udp_tx_ch_state = UDP_TX_CH_STATE_IDLE;

    ESP_LOGI(TAG, "UDP audio task started");

    TickType_t last_wake_time;

    while (s_task_running)
    {

        last_wake_time = xTaskGetTickCount();

        if (UDP_TX_CH_STATE_IDLE == udp_tx_ch_state)
        {
            /* Free to start a new Tx */
            if (UDP_TX_CH_NONE == udp_tx_ch)
            {
                /* Wait for new data*/
                if (data_ready)
                {
                    ESP_LOGI(TAG, "Starting new UDP transmission, Seq L:%lu R:%lu",
                             s_sequence_num_left, s_sequence_num_right);
                    /* Get new read buffer */
                    current_buf = &s_buffer_sets[s_read_buffer_idx];

                    udp_tx_ch = UDP_TX_CH_LEFT_FRONT;
                    udp_tx_ch_state = UDP_TX_CH_STATE_PKT_1;
                    s_send_state.left_offset = 0;
                    s_send_state.right_offset = 0;
                    s_send_state.left_complete = false;
                    // s_send_state.right_complete = true;
                }
                else
                {
                    /* No data available, continue waiting */
                    xTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(1));
                    continue;
                }
            }
        }
        else if (UDP_TX_CH_STATE_PKT_1 == udp_tx_ch_state)
        {
            if (UDP_TX_CH_LEFT_FRONT == udp_tx_ch)
            {
                /* Send the first Packet */
                ESP_LOGI(TAG, "Sending First Packet, Seq L:%lu", s_sequence_num_left);
                esp_err_t err = udp_send_single_packet(current_buf->left_channel,
                                                        s_send_state.left_offset,
                                                        current_buf->left_len,
                                                        true);

                if (ESP_OK == err)
                {
                }
                else
                {
                }

                /* Update offset for next packet */
                s_send_state.left_offset += MAX_AUDIO_PACKET_SIZE;

                /* Check if all left data sent */
                if (s_send_state.left_offset >= current_buf->left_len)
                {
                    s_send_state.left_complete = true;
                    udp_tx_ch_state = UDP_TX_CH_STATE_IDLE;
                }
                else
                {
                    udp_tx_ch_state = UDP_TX_CH_STATE_PKT_2;
                }

                ESP_LOGI(TAG, "Sent First Packet, Seq L:%lu", s_sequence_num_left);

                sequence_num++;
                s_sequence_num_left++;
            }
            // else if (UDP_TX_CH_RIGHT_FRONT == udp_tx_ch)
            // {
            //     /* Send the first Packet */
            //     esp_err_t err = udp_send_single_packet(current_buf->right_channel,
            //                                             s_send_state.right_offset,
            //                                             current_buf->right_len,
            //                                             false);

            //     if (err == ESP_OK)
            //     {
            //         /* Update offset for next packet */
            //         s_send_state.right_offset += MAX_AUDIO_PACKET_SIZE;

            //         /* Check if all right data sent */
            //         if (s_send_state.right_offset >= current_buf->right_len)
            //         {
            //             s_send_state.right_complete = true;
            //         }

            //         udp_tx_ch_state = UDP_TX_CH_STATE_PKT_2;
            //     }

            //     s_sequence_num_right++;
            // }

            /* Wait a couple of milliseconds for UDP Packet to be sent*/
            xTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(1));

        }
        else if (UDP_TX_CH_STATE_PKT_2 == udp_tx_ch_state)
        {
            if (UDP_TX_CH_LEFT_FRONT == udp_tx_ch)
            {
                /* Send the second Packet */
                ESP_LOGI(TAG, "Sending Second Packet, Seq L:%lu", s_sequence_num_left);
                esp_err_t err = udp_send_single_packet(current_buf->left_channel,
                                                        s_send_state.left_offset,
                                                        current_buf->left_len,
                                                        true);

                if (err == ESP_OK)
                {
                }
                else
                {
                }

                /* Update offset for next packet */
                s_send_state.left_offset = 0;

                /* Check if all left data sent */
                if (s_send_state.left_offset >= current_buf->left_len)
                {
                    s_send_state.left_complete = true;
                }

                /* Move to next channel */
                udp_tx_ch = UDP_TX_CH_NONE;
                udp_tx_ch_state = UDP_TX_CH_STATE_IDLE;

                ESP_LOGI(TAG, "Sent Second Packet, Seq L:%lu", s_sequence_num_left);

                sequence_num++;
                s_sequence_num_left++;
            }
            // else if (UDP_TX_CH_RIGHT_FRONT == udp_tx_ch)
            // {
            //     /* Send the second Packet */
            //     esp_err_t err = udp_send_single_packet(current_buf->right_channel,
            //                                             s_send_state.right_offset,
            //                                             current_buf->right_len,
            //                                             false);

            //     if (err == ESP_OK)
            //     {
            //         /* Update offset for next packet */
            //         s_send_state.right_offset += MAX_AUDIO_PACKET_SIZE;

            //         /* Check if all right data sent */
            //         if (s_send_state.right_offset >= current_buf->right_len)
            //         {
            //             s_send_state.right_complete = true;
            //         }

            //         /* All channels done for this buffer */
            //         udp_tx_ch = UDP_TX_CH_NONE;
            //         udp_tx_ch_state = UDP_TX_CH_STATE_IDLE;
            //     }

            //     s_sequence_num_right++;
            // }

            /* Clear the flag so that new data can be processed from BT */
            data_ready = false;

            /* Wait a couple of milliseconds for UDP Packet to be sent*/
            xTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(1));

        }

        /* Check if we need new data */
        // if (current_buf == NULL || (s_send_state.left_complete && s_send_state.right_complete))
        // {
        //     /* Wait for new data */
        //     // if (xSemaphoreTake(s_data_ready_sem, pdMS_TO_TICKS(5)) == pdTRUE)
        //     if (data_ready)
        //     {
        //         /* Get new read buffer */
        //         current_buf = &s_buffer_sets[s_read_buffer_idx];

        //         /* Reset send state for new buffer */
        //         s_send_state.left_offset = 0;
        //         s_send_state.right_offset = 0;
        //         s_send_state.left_complete = false;
        //         s_send_state.right_complete = true;
        //         data_ready = false;
        //     }
        //     else
        //     {
        //         /* No data available, continue waiting */
        //         vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(5));
        //         continue;
        //     }
        // }

        // /* Send one packet from each channel (interleaved) */

        // /* Send one LEFT packet if not complete */
        // if (!s_send_state.left_complete)
        // {
        //     esp_err_t err = udp_send_single_packet(current_buf->left_channel,
        //                                             s_send_state.left_offset,
        //                                             current_buf->left_len,
        //                                             true);
        //     sequence_num++;
        //     s_sequence_num_left++;
        //     if (err == ESP_OK)
        //     {
        //         /* Update offset for next packet */
        //         s_send_state.left_offset += MAX_AUDIO_PACKET_SIZE;

        //         /* Check if all left data sent */
        //         if (s_send_state.left_offset >= current_buf->left_len)
        //         {
        //             s_send_state.left_complete = true;
        //         }
        //     }
        //     /* On error, we'll retry next iteration */
        // }

        /* Send one RIGHT packet if not complete */
        // if (!s_send_state.right_complete)
        // {
        //     esp_err_t err = udp_send_single_packet(current_buf->right_channel,
        //                                             s_send_state.right_offset,
        //                                             current_buf->right_len,
        //                                             false);
        //     s_sequence_num_right++;

        //     if (err == ESP_OK)
        //     {
        //         /* Update offset for next packet */
        //         s_send_state.right_offset += MAX_AUDIO_PACKET_SIZE;

        //         /* Check if all right data sent */
        //         if (s_send_state.right_offset >= current_buf->right_len)
        //         {
        //             s_send_state.right_complete = true;
        //         }
        //     }
        //     /* On error, we'll retry next iteration */
        // }

        // /* Periodic statistics */
        // if (++stats_counter % 100 == 0)
        // {
        //     static uint32_t free_heap;
        //     uint32_t heap = esp_get_free_heap_size();

        //     ESP_LOGI(TAG, "Sent - L:%lu R:%lu Dropped:%lu Sequence:%lu Free:%lu Before:%lu",
        //                 s_packets_sent_left, s_packets_sent_right, s_packets_dropped, sequence_num,
        //                 heap, free_heap);

        //     free_heap = esp_get_free_heap_size();
        // }

    }

    ESP_LOGI(TAG, "UDP audio task stopped");
    vTaskDelete(NULL);
}


static esp_err_t udp_send_single_packet(const uint8_t *data, uint32_t offset,
                                        uint32_t total_len, bool is_left)
{
    int socket;
    struct sockaddr_in *dest_addr;
    uint8_t *packet_buffer;
    uint32_t *packets_sent;

    /* Select channel parameters */
    if (is_left)
    {
        socket = s_udp_socket_left;
        dest_addr = &s_dest_addr_left;
        packet_buffer = s_left_packet_buffer;
        // sequence_num = &s_sequence_num_left;
        packets_sent = &s_packets_sent_left;
    }
    else
    {
        socket = s_udp_socket_right;
        dest_addr = &s_dest_addr_right;
        packet_buffer = s_right_packet_buffer;
        // sequence_num = &s_sequence_num_right;
        packets_sent = &s_packets_sent_right;
    }

    /* Calculate chunk size for this packet */
    uint32_t remaining = total_len - offset;
    uint16_t chunk_len = (remaining > MAX_AUDIO_PACKET_SIZE) ?
                          MAX_AUDIO_PACKET_SIZE : remaining;

    if (chunk_len == 0)
    {
        /* Nothing to send */
        return ESP_OK;
    }

    /* Build header */
    udp_audio_header_t *header = (udp_audio_header_t *)packet_buffer;
    header->sequence_num    = sequence_num;
    header->timestamp       = esp_log_timestamp();
    header->data_len        = chunk_len;
    header->flags           = is_left ? CHANNEL_FLAG_LEFT : CHANNEL_FLAG_RIGHT;

    /* Copy chunk from offset */
    memcpy(packet_buffer + sizeof(udp_audio_header_t), data + offset, chunk_len);

    /* Send single packet */
    int total_packet_len = sizeof(udp_audio_header_t) + chunk_len;
    int sent = sendto(socket, packet_buffer, total_packet_len, 0,
                      (struct sockaddr *)dest_addr, sizeof(struct sockaddr_in));

    // int sent = socket_send(socket, packet_buffer, total_packet_len, 0,
    //                   (struct sockaddr *)dest_addr, sizeof(struct sockaddr_in));

    if (sent < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM)
        {
            // /* Buffer full - will retry next iteration */
            // ESP_LOGE(TAG, "Failed to send packet pkt:%d errno 0x%03X", sequence_num, errno);
            // return ESP_ERR_TIMEOUT;
        }

        /* Real error */
        static uint32_t err_count = 0;
        if (++err_count % 100 == 0)
        {
            ESP_LOGE(TAG, "%s send error %d, count: %lu",
                     is_left ? "LEFT" : "RIGHT", errno, err_count);
        }
        return ESP_FAIL;
    }

    (*packets_sent)++;
    return ESP_OK;
}

// static esp_err_t udp_send_channel_chunked(const uint8_t *data, uint32_t len, bool is_left)
// {
//     int socket;
//     struct sockaddr_in *dest_addr;
//     uint8_t *packet_buffer;
//     uint32_t *sequence_num;
//     uint32_t *packets_sent;

//     /* Select channel parameters */
//     if (is_left)
//     {
//         socket = s_udp_socket_left;
//         dest_addr = &s_dest_addr_left;
//         packet_buffer = s_left_packet_buffer;
//         sequence_num = &s_sequence_num_left;
//         packets_sent = &s_packets_sent_left;
//     }
//     else
//     {
//         socket = s_udp_socket_right;
//         dest_addr = &s_dest_addr_right;
//         packet_buffer = s_right_packet_buffer;
//         sequence_num = &s_sequence_num_right;
//         packets_sent = &s_packets_sent_right;
//     }

//     /* Send data in chunks of MAX_AUDIO_PACKET_SIZE */
//     uint32_t offset = 0;
//     while (offset < len)
//     {
//         uint16_t chunk_len = (len - offset) > MAX_AUDIO_PACKET_SIZE ?
//                               MAX_AUDIO_PACKET_SIZE : (len - offset);

//         /* Build header */
//         udp_audio_header_t *header = (udp_audio_header_t *)packet_buffer;
//         header->sequence_num = (*sequence_num)++;
//         header->timestamp = xTaskGetTickCount();
//         header->data_len = chunk_len;
//         header->flags = is_left ? CHANNEL_FLAG_LEFT : CHANNEL_FLAG_RIGHT;

//         /* Copy chunk */
//         memcpy(packet_buffer + sizeof(udp_audio_header_t), data + offset, chunk_len);

//         /* Send */
//         int total_len = sizeof(udp_audio_header_t) + chunk_len;
//         int sent = sendto(socket, packet_buffer, total_len, 0,
//                           (struct sockaddr *)dest_addr, sizeof(struct sockaddr_in));

//         if (sent < 0)
//         {
//             if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOMEM)
//             {
//                 static uint32_t err_count = 0;
//                 if (++err_count % 100 == 0)
//                 {
//                     ESP_LOGE(TAG, "%s send error %d, count: %lu",
//                              is_left ? "LEFT" : "RIGHT", errno, err_count);
//                 }
//                 return ESP_FAIL;
//             }
//             /* Buffer full - continue anyway, some data loss acceptable */
//         }
//         else
//         {
//             (*packets_sent)++;
//         }

//         offset += chunk_len;
//     }

//     return ESP_OK;
// }

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

    ESP_LOGI(TAG, "UDP audio task Created");
    return ESP_OK;
}

void udp_audio_task_stop(void)
{
    if (s_task_running)
    {
        s_task_running = false;
        /* Signal semaphore to unblock task */
        xSemaphoreGive(s_data_ready_sem);
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

    if (s_data_ready_sem != NULL)
    {
        vSemaphoreDelete(s_data_ready_sem);
        s_data_ready_sem = NULL;
    }

    if (s_buffer_mutex != NULL)
    {
        vSemaphoreDelete(s_buffer_mutex);
        s_buffer_mutex = NULL;
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