#pragma once

/* Connect to the configured WiFi as a station (auto-reconnects). */
void wifi_sta_init(void);

/* Block until an IP address has been obtained. */
void wifi_sta_wait_connected(void);
