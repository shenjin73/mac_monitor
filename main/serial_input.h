#pragma once

#include <stdint.h>
#include <stdbool.h>

/* One sample line pushed by host/mac_stats.py over USB serial.
 * Numeric fields use -1 (or negative) for "unavailable". */
typedef struct {
    int cpu;            // percent 0-100
    int gpu;            // percent 0-100
    int mem;            // percent 0-100
    int cpufreq_mhz;    // current CPU clock
    int gpufreq_mhz;    // current GPU clock
    float mem_used_gb;
    float mem_total_gb;
    int fan_rpm;        // -1 = no fan / unavailable
    float cpu_temp_c;
    float gpu_temp_c;
    float power_w;      // system power
    int kimi5h;         // Kimi 5-hour window usage, percent 0-100
    int kimiweek;       // Kimi weekly quota usage, percent 0-100
    float dsbal;        // DeepSeek balance (CNY)
    int32_t dstok;      // DeepSeek tokens used today
    int sess;           // CLI session state: 0 none, 1 idle, 2 working, 3 waiting_user
    int sessn;          // live session count
    int sessw;          // working session count
    int sessp;          // waiting_user session count
    int64_t last_update_ms; // esp_timer time of last parsed line, 0 = never
} mac_stats_t;

/* Start the stdin line-reader task (console must be USB Serial/JTAG). */
void serial_input_start(void);

/* Start the UDP datagram receiver (call after WiFi is connected). */
void udp_input_start(void);

/* Copy the latest snapshot. Returns false if no line was ever received. */
bool stats_get_snapshot(mac_stats_t *out);

/* True when the last received line is older than `ms` (or none received). */
bool stats_is_stale(int64_t ms);
