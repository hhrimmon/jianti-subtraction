/* Wildcard DNS responder (captive portal piece #1).
 * Listens on UDP 53. Any well-formed query gets an A record pointing at the
 * SoftAP address 192.168.4.1, whatever name was asked for. Malformed packets
 * are ignored. This is the standard captive-portal trick: the phone asks
 * "where is captive.apple.com / connectivitycheck.gstatic.com", we answer
 * "here", and its HTTP probe then hits our portal page.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "dns_server.h"

static const char *TAG = "jt_dns";
static const uint8_t AP_IP[4] = {192, 168, 4, 1};

/* Skip the question section (QNAME labels + QTYPE/QCLASS). Returns the
 * offset just past it, or -1 when the packet is malformed. */
static int skip_question(const uint8_t *pkt, size_t len, int off)
{
    while (off < (int)len) {
        uint8_t n = pkt[off++];
        if (n == 0) break;
        if ((n & 0xC0) == 0xC0) {
            /* Compressed label pointer: one more byte, then done. */
            off += 1;
            break;
        }
        off += n;
    }
    if (off + 4 > (int)len) return -1;
    return off + 4; /* QTYPE (2) + QCLASS (2) */
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket create failed");
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind port 53 failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "wildcard DNS up on 53 -> 192.168.4.1");

    uint8_t in[512];
    uint8_t out[512];
    while (true) {
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, in, sizeof(in), 0,
                             (struct sockaddr *)&from, &from_len);
        if (n < 12) continue;
        int qdcount = (in[4] << 8) | in[5];
        if (qdcount < 1) continue;
        int qend = skip_question(in, (size_t)n, 12);
        if (qend < 0 || qend > (int)sizeof(out) - 16) continue;

        /* Header: same ID, flags 0x8180 (response, no error), QD=1, AN=1. */
        out[0] = in[0];
        out[1] = in[1];
        out[2] = 0x81;
        out[3] = 0x80;
        out[4] = 0x00;
        out[5] = 0x01;
        out[6] = 0x00;
        out[7] = 0x01;
        out[8] = 0x00;
        out[9] = 0x00;
        out[10] = 0x00;
        out[11] = 0x00;
        /* Echo the question section verbatim. */
        memcpy(out + 12, in + 12, (size_t)(qend - 12));
        int o = qend;
        /* Answer: NAME pointer to offset 12, TYPE A, CLASS IN, TTL 60. */
        out[o++] = 0xC0;
        out[o++] = 0x0C;
        out[o++] = 0x00;
        out[o++] = 0x01;
        out[o++] = 0x00;
        out[o++] = 0x01;
        out[o++] = 0x00;
        out[o++] = 0x00;
        out[o++] = 0x00;
        out[o++] = 0x3C;
        out[o++] = 0x00;
        out[o++] = 0x04;
        memcpy(out + o, AP_IP, 4);
        o += 4;
        sendto(sock, out, (size_t)o, 0,
               (struct sockaddr *)&from, from_len);
    }
}

esp_err_t dns_server_start(void)
{
    if (xTaskCreate(dns_task, "jt_dns", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
