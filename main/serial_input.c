#include "serial_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#define UDP_STATS_PORT 45678

static const char *TAG = "serial_input";

static mac_stats_t s_stats = {
    .cpu = -1, .gpu = -1, .mem = -1,
    .cpufreq_mhz = -1, .gpufreq_mhz = -1,
    .mem_used_gb = -1, .mem_total_gb = -1,
    .fan_rpm = -1,
    .cpu_temp_c = -1, .gpu_temp_c = -1, .power_w = -1,
    .kimi5h = -1, .kimiweek = -1, .dsbal = -1, .dstok = -1,
    .sess = -1, .sessn = 0, .sessw = 0, .sessp = 0,
    .last_update_ms = 0,
};
static SemaphoreHandle_t s_lock;

static void apply_kv(char *key, char *val, bool *any)
{
    if (strcmp(key, "cpu") == 0) s_stats.cpu = atoi(val);
    else if (strcmp(key, "gpu") == 0) s_stats.gpu = atoi(val);
    else if (strcmp(key, "mem") == 0) s_stats.mem = atoi(val);
    else if (strcmp(key, "cpufreq") == 0) s_stats.cpufreq_mhz = atoi(val);
    else if (strcmp(key, "gpufreq") == 0) s_stats.gpufreq_mhz = atoi(val);
    else if (strcmp(key, "memu") == 0) s_stats.mem_used_gb = strtof(val, NULL);
    else if (strcmp(key, "memt") == 0) s_stats.mem_total_gb = strtof(val, NULL);
    else if (strcmp(key, "fan") == 0) s_stats.fan_rpm = atoi(val);
    else if (strcmp(key, "cput") == 0) s_stats.cpu_temp_c = strtof(val, NULL);
    else if (strcmp(key, "gput") == 0) s_stats.gpu_temp_c = strtof(val, NULL);
    else if (strcmp(key, "pwr") == 0) s_stats.power_w = strtof(val, NULL);
    else if (strcmp(key, "kimi5h") == 0) s_stats.kimi5h = atoi(val);
    else if (strcmp(key, "kimiweek") == 0) s_stats.kimiweek = atoi(val);
    else if (strcmp(key, "dsbal") == 0) s_stats.dsbal = strtof(val, NULL);
    else if (strcmp(key, "dstok") == 0) s_stats.dstok = (int32_t)strtol(val, NULL, 10);
    else if (strcmp(key, "sess") == 0) s_stats.sess = atoi(val);
    else if (strcmp(key, "sessn") == 0) s_stats.sessn = atoi(val);
    else if (strcmp(key, "sessw") == 0) s_stats.sessw = atoi(val);
    else if (strcmp(key, "sessp") == 0) s_stats.sessp = atoi(val);
    else return; /* unknown key: ignore */
    *any = true;
}

static void parse_line(char *line)
{
    bool any = false;
    char *saveptr = NULL;
    for (char *tok = strtok_r(line, " \t\r\n", &saveptr); tok;
         tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
        char *eq = strchr(tok, '=');
        if (!eq || eq == tok || eq[1] == '\0') continue;
        *eq = '\0';
        apply_kv(tok, eq + 1, &any);
    }
    if (any) {
        s_stats.last_update_ms = esp_timer_get_time() / 1000;
    }
}

static void serial_task(void *arg)
{
    /* The console vfs is non-blocking: reads return 0 (EOF to stdio) whenever
     * no bytes are buffered, so fgets() would hand us arbitrary line
     * fragments. Assemble lines byte-by-byte and treat EOF as "no data yet". */
    char buf[256];
    size_t len = 0;
    while (1) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\n') {
            if (len > 0) {
                buf[len] = '\0';
                xSemaphoreTake(s_lock, portMAX_DELAY);
                parse_line(buf);
                ESP_LOGD(TAG, "parsed: cpu=%d fan=%d sess=%d kimiweek=%d",
                         s_stats.cpu, s_stats.fan_rpm, s_stats.sess, s_stats.kimiweek);
                xSemaphoreGive(s_lock);
                len = 0;
            }
            continue;
        }
        if (c == '\r') continue;
        if (len < sizeof(buf) - 1) {
            buf[len++] = (char)c;
        } else {
            len = 0; /* overflow: drop the line */
        }
    }
}

void serial_input_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(serial_task, "serial_in", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "listening for stats lines on stdin");
}

/* UDP stats receiver: mac_stats.py broadcasts one protocol line per datagram
 * to UDP_STATS_PORT on the LAN. WiFi must be connected before calling this. */
static void udp_task(void *arg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "udp socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_STATS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "udp bind :%d failed", UDP_STATS_PORT);
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening for stats datagrams on udp/%d", UDP_STATS_PORT);

    char buf[256];
    while (1) {
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        buf[n] = '\0';
        xSemaphoreTake(s_lock, portMAX_DELAY);
        parse_line(buf);
        ESP_LOGD(TAG, "udp parsed: cpu=%d fan=%d sess=%d",
                 s_stats.cpu, s_stats.fan_rpm, s_stats.sess);
        xSemaphoreGive(s_lock);
    }
}

void udp_input_start(void)
{
    xTaskCreate(udp_task, "udp_in", 4096, NULL, 5, NULL);
}

bool stats_get_snapshot(mac_stats_t *out)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    ok = s_stats.last_update_ms != 0;
    xSemaphoreGive(s_lock);
    return ok;
}

bool stats_is_stale(int64_t ms)
{
    bool stale;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stale = s_stats.last_update_ms == 0 ||
            (esp_timer_get_time() / 1000 - s_stats.last_update_ms) > ms;
    xSemaphoreGive(s_lock);
    return stale;
}
