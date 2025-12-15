/* tcp_audio.c */
#include "tcp_audio.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "TCP_AUDIO"
#define MAX_AUDIO_PACKET_SIZE 4096
#define TCP_CONNECT_TIMEOUT_MS 5000
#define TCP_SEND_TIMEOUT_MS 100

typedef struct {
    uint32_t sequence_num;
    uint32_t timestamp;
    uint32_t data_len;  /* Changed to 32-bit for larger packets */
    uint16_t flags;
    uint16_t reserved;
} __attribute__((packed)) tcp_audio_header_t;

static uint8_t s_packet_buffer[MAX_AUDIO_PACKET_SIZE + sizeof(tcp_audio_header_t)];
static int s_tcp_socket = -1;
static struct sockaddr_in s_dest_addr;
static uint32_t s_sequence_num = 0;
static bool s_initialized = false;
static bool s_connected = false;

static TaskHandle_t s_tcp_app_task_handle = NULL;  /* handle of application task  */

/* Statistics */
static uint32_t s_packets_sent = 0;
static uint32_t s_send_errors = 0;

/* Forward declaration */
esp_err_t tcp_reconnect(void);

static void tcp_app_task_handler(void *arg)
{
    while (1)
    {
        if (!s_initialized)
        {
            // return ESP_ERR_INVALID_STATE;
        }

        if (!s_connected)
        {
            /* Attempt reconnection (non-blocking) */
            static uint32_t last_reconnect_attempt = 0;
            uint32_t now = xTaskGetTickCount();
            if (now - last_reconnect_attempt > pdMS_TO_TICKS(5000)) {
                last_reconnect_attempt = now;
                ESP_LOGW(TAG, "Not connected, attempting reconnect...");
                tcp_reconnect();
            }
            // return ESP_ERR_INVALID_STATE;
        }
        else
        {
            /* Nothing to do for now */
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "TCP Audio Stats: Packets Sent=%" PRIu32 ", Send Errors=%" PRIu32,
                 s_packets_sent, s_send_errors);
    }
}

esp_err_t tcp_audio_init(const char *dest_ip, uint16_t dest_port)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* Configure destination address */
    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family = AF_INET;
    s_dest_addr.sin_port = htons(dest_port);
    s_dest_addr.sin_addr.s_addr = inet_addr(dest_ip);

    s_sequence_num = 0;
    s_initialized = true;
    s_connected = false;

    ESP_LOGI(TAG, "TCP audio initialized for: %s:%d", dest_ip, dest_port);

    xTaskCreate(tcp_app_task_handler, "TcpAppTask", 3072, NULL, 10, &s_tcp_app_task_handle);

    /* Attempt initial connection */
    return tcp_reconnect();
}

esp_err_t tcp_reconnect(void)
{
    /* Close existing socket if any */
    if (s_tcp_socket >= 0) {
        close(s_tcp_socket);
        s_tcp_socket = -1;
        s_connected = false;
    }

    /* Create TCP socket */
    s_tcp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_tcp_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        return ESP_FAIL;
    }

    /* Set socket to non-blocking for connect timeout */
    int flags = fcntl(s_tcp_socket, F_GETFL, 0);
    fcntl(s_tcp_socket, F_SETFL, flags | O_NONBLOCK);

    /* Disable Nagle's algorithm for low latency */
    int nodelay = 1;
    setsockopt(s_tcp_socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* Set keepalive */
    int keepalive = 1;
    setsockopt(s_tcp_socket, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    /* Set send buffer size */
    int sndbuf_size = 32768;  /* 32KB */
    setsockopt(s_tcp_socket, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size));

    /* Set send timeout (important for non-blocking) */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = TCP_SEND_TIMEOUT_MS * 1000;
    setsockopt(s_tcp_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Attempt connection */
    ESP_LOGI(TAG, "Connecting to server...");
    int ret = connect(s_tcp_socket, (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));

    if (ret < 0) {
        if (errno == EINPROGRESS) {
            /* Connection in progress, wait for completion */
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(s_tcp_socket, &write_fds);

            struct timeval timeout;
            timeout.tv_sec = TCP_CONNECT_TIMEOUT_MS / 1000;
            timeout.tv_usec = (TCP_CONNECT_TIMEOUT_MS % 1000) * 1000;

            ret = select(s_tcp_socket + 1, NULL, &write_fds, NULL, &timeout);
            if (ret <= 0) {
                ESP_LOGE(TAG, "Connection timeout");
                close(s_tcp_socket);
                s_tcp_socket = -1;
                return ESP_ERR_TIMEOUT;
            }

            /* Check if connection succeeded */
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(s_tcp_socket, SOL_SOCKET, SO_ERROR, &error, &len);
            if (error != 0) {
                ESP_LOGE(TAG, "Connection failed: errno %d", error);
                close(s_tcp_socket);
                s_tcp_socket = -1;
                return ESP_FAIL;
            }
        } else {
            ESP_LOGE(TAG, "Connect failed immediately: errno %d", errno);
            close(s_tcp_socket);
            s_tcp_socket = -1;
            return ESP_FAIL;
        }
    }

    s_connected = true;
    ESP_LOGI(TAG, "TCP connection established");
    return ESP_OK;
}

esp_err_t tcp_send_audio(const uint8_t *data, uint32_t len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_connected) {
        /* Attempt reconnection (non-blocking) */
        static uint32_t last_reconnect_attempt = 0;
        uint32_t now = xTaskGetTickCount();
        if (now - last_reconnect_attempt > pdMS_TO_TICKS(5000)) {
            last_reconnect_attempt = now;
            ESP_LOGW(TAG, "Not connected, attempting reconnect...");
            tcp_reconnect();
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (len > MAX_AUDIO_PACKET_SIZE) {
        len = MAX_AUDIO_PACKET_SIZE;
    }

    /* Build header */
    tcp_audio_header_t *header = (tcp_audio_header_t *)s_packet_buffer;
    header->sequence_num = s_sequence_num++;
    header->timestamp = xTaskGetTickCount();
    header->data_len = len;
    header->flags = 0;
    header->reserved = 0;

    /* Copy audio data */
    memcpy(s_packet_buffer + sizeof(tcp_audio_header_t), data, len);

    /* Send via TCP */
    int total_len = sizeof(tcp_audio_header_t) + len;
    int sent = send(s_tcp_socket, s_packet_buffer, total_len, MSG_DONTWAIT);

    if (sent < 0) {
        s_send_errors++;

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Send buffer full - expected in non-blocking mode */
            if (s_send_errors % 50 == 0) {
                ESP_LOGW(TAG, "Send buffer full, total errors: %lu", s_send_errors);
            }
            return ESP_ERR_TIMEOUT;
        } else if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
            /* Connection lost */
            ESP_LOGE(TAG, "Connection lost: errno %d", errno);
            s_connected = false;
            close(s_tcp_socket);
            s_tcp_socket = -1;
            return ESP_ERR_NOT_FOUND;
        } else {
            ESP_LOGE(TAG, "Send failed: errno %d", errno);
            return ESP_FAIL;
        }
    } else if (sent < total_len) {
        /* Partial send - TCP stream fragmented */
        ESP_LOGW(TAG, "Partial send: %d/%d bytes", sent, total_len);
        /* In production, you'd need to handle partial sends properly */
    }

    s_packets_sent++;

    /* Log statistics periodically */
    if (s_packets_sent % 100 == 0) {
        ESP_LOGI(TAG, "Sent: %lu packets, Errors: %lu (%.1f%%)",
                 s_packets_sent, s_send_errors,
                 (float)s_send_errors * 100.0 / s_packets_sent);
    }

    return ESP_OK;
}

bool tcp_audio_is_connected(void)
{
    return s_connected;
}

void tcp_audio_deinit(void)
{
    if (s_tcp_socket >= 0) {
        shutdown(s_tcp_socket, SHUT_RDWR);
        close(s_tcp_socket);
        s_tcp_socket = -1;
    }
    s_connected = false;
    s_initialized = false;
    ESP_LOGI(TAG, "TCP audio deinitialized");
}

