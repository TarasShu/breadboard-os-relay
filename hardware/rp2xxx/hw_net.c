/******************************************************************************
 * @file hw_net.c
 *
 * @brief Settings, definitions, and function prototypes for various network
 *        stacks, much of it tied to lwIP. Hardware and SDK-specific
 *        implementation resides within these functions, hence its location
 *        within the hardware/[platform] directory.
 *
 * @author Cavin McKinley (MCKNLY LLC)
 *
 * @date 02-06-2025
 *
 * @copyright Copyright (c) 2025 Cavin McKinley (MCKNLY LLC)
 *            Released under the MIT License
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <string.h>
#include "hw_net.h"
#include "hardware_config.h"
#include "version.h"
#include "FreeRTOS.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/mdns.h"
#include "lwip/apps/httpd.h"
#include "lwip/arch.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


/************************
 * lwIP mDNS responder
*************************/

// mDNS responder callback function for adding service text records
static void srv_txt(struct mdns_service *service, void *txt_userdata) {
    err_t res;
    LWIP_UNUSED_ARG(txt_userdata);

    res = mdns_resp_add_service_txtitem(service, "path=/", 6);
    LWIP_ERROR("mdns add service txt failed\n", (res == ERR_OK), return);
}

void net_mdns_init(void) {
    // initialize lwIP mDNS
    mdns_resp_init();
    // bind mDNS to the network interface and responder text record service
    mdns_resp_add_netif(&cyw43_state.netif[CYW43_ITF_STA], CYW43_HOST_NAME);
    mdns_resp_add_service(&cyw43_state.netif[CYW43_ITF_STA], "bbos-httpd", "_http", DNSSD_PROTO_TCP, 80, srv_txt, NULL);
}


/**************************
 * lwIP HTTPD (http server)
***************************/

#ifdef ENABLE_HTTPD

// cgi handler function example - refer to tCGIHandler typedef in lwIP httpd.h for prototype description
static const char *httpd_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    if (strcmp(pcParam[0], "led-state") == 0) {
        if (!strcmp(pcValue[0], "on")) {
            onboard_led_set(1);
        } else if (!strcmp(pcValue[0], "off")) {
            onboard_led_set(0);
        } else {
            return "/404.html";
        }
        return "/test.shtml";
    } else {
        // Relay handler for gpio16-19
        int relay_pins[4] = {16, 17, 18, 19};
        for (int idx = 0; idx < 4; idx++) {
            char gpio_name[8];
            snprintf(gpio_name, sizeof(gpio_name), "gpio%d", relay_pins[idx]);
            if (strcmp(pcParam[0], gpio_name) == 0) {
                if (strcmp(pcValue[0], "on") == 0) {
                    gpio_write_single(idx, 1);
                } else if (strcmp(pcValue[0], "off") == 0) {
                    gpio_write_single(idx, 0);
                }
                return "/test.shtml";
            }
        }
        // If not matched, return default test page
        return "/test.shtml";
    }
}

// Server-side storage for relay ON timestamps and names
static uint32_t relay_on_timestamps[4] = {0, 0, 0, 0}; // seconds since boot
static char relay_names[4][32] = {"Relay 1", "Relay 2", "Relay 3", "Relay 4"};

// Add static buffer for relay JSON
static char last_relay_status_json[512] = "{}";

// Helper: get current time in seconds since boot
static uint32_t get_seconds_since_boot() {
    return (uint32_t)(get_time_us() / 1000000ULL);
}

// New CGI handler for relay status and names
static const char *relay_status_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    // Set relay name if requested
    int setname = 0, relay_idx = -1;
    const char *new_name = NULL;
    for (int i = 0; i < iNumParams; i++) {
        if (strcmp(pcParam[i], "setname") == 0 && strcmp(pcValue[i], "1") == 0) setname = 1;
        if (strcmp(pcParam[i], "relay") == 0) relay_idx = atoi(pcValue[i]) - 1;
        if (strcmp(pcParam[i], "name") == 0) new_name = pcValue[i];
    }
    if (setname && relay_idx >= 0 && relay_idx < 4 && new_name) {
        strncpy(relay_names[relay_idx], new_name, sizeof(relay_names[relay_idx]) - 1);
        relay_names[relay_idx][sizeof(relay_names[relay_idx]) - 1] = '\0';
    }

    // Build JSON response
    int len = snprintf(last_relay_status_json, sizeof(last_relay_status_json), "{\"relays\":[");
    for (int i = 0; i < 4; i++) {
        int state = gpio_read_single(i);
        uint32_t on_time = relay_on_timestamps[i];
        if (state && on_time == 0) relay_on_timestamps[i] = on_time = get_seconds_since_boot();
        if (!state) relay_on_timestamps[i] = 0;
        len += snprintf(last_relay_status_json + len, sizeof(last_relay_status_json) - len,
            "%s{\"state\":%d,\"on_time\":%u,\"name\":\"%s\"}",
            (i ? "," : ""), state, on_time, relay_names[i]);
    }
    snprintf(last_relay_status_json + len, sizeof(last_relay_status_json) - len, "]}");
    return "/relay_status.shtml";
}

// cgi handler path assignments for any functions above
static tCGI httpd_cgi_paths[] = {
    { "/test.shtml", httpd_cgi_handler },
    { "/relay_status.cgi", relay_status_cgi_handler }
};

// ssi tags, handled by index # in the ssi handler function
// Note that maximum tag length is 8 characters (LWIP_HTTPD_MAX_TAG_NAME_LEN)
// There will be no warning if exceeded! The tag will simply not be processed!
static const char * httpd_ssi_tags[] = {
    "bbosver",
    "projinfo",
    "platform",
    "uptime",
    "freeram",
    "freeflsh",
    "ledstate",
    "relay1",
    "relay2",
    "relay3",
    "relay4",
    "relayjson"
};

// ssi handler function - refer to tSSIHandler typedef in lwIP httpd.h for prototype description
// note that the integer case values in the switch need to match the index of the tag in the httpd_ssi_tags array
u16_t httpd_ssi_handler(int iIndex, char *pcInsert, int iInsertLen, uint16_t current_tag_part, uint16_t *next_tag_part) {
    size_t tag_print;
    switch (iIndex) {
        case 0: { /* "version" */
            tag_print = snprintf(pcInsert, iInsertLen, xstr(BBOS_VERSION_MAJOR) "." xstr(BBOS_VERSION_MINOR) "%c", BBOS_VERSION_MOD);
            break;
        }
        case 1: {
            tag_print = snprintf(pcInsert, iInsertLen, xstr(PROJECT_NAME) " v" xstr(PROJECT_VERSION));
            break;
        }
        case 2: {
            tag_print = snprintf(pcInsert, iInsertLen, xstr(BOARD) " - " xstr(MCU_NAME));
            break;
        }
        case 3: { /* uptime */
            // get current microsecond timer value
            uint64_t current_time_us = get_time_us();
            // calculate time divisions, i.e. maths
            uint16_t num_seconds = (uint16_t) (current_time_us / 1E6) % 60;
            uint16_t num_minutes = (uint16_t) (((current_time_us / 1E6) - num_seconds) / 60) % 60;
            uint16_t num_hours   = (uint16_t) (((((current_time_us / 1E6) - num_seconds) / 60) - num_minutes) / 60) % 24;
            uint16_t num_days    = (uint16_t) ((((((current_time_us / 1E6) - num_seconds) / 60) - num_minutes) / 60) - num_hours) / 24;
            tag_print = snprintf(pcInsert, iInsertLen, "%dd %dh %dm %ds", num_days, num_hours, num_minutes, num_seconds);
            break;
        }
        case 4: { /* freeram */
            HeapStats_t *heap_stats = pvPortMalloc(sizeof(HeapStats_t)); // structure to hold heap stats results
            vPortGetHeapStats(heap_stats);  // get the heap stats
            tag_print = snprintf(pcInsert, iInsertLen, "%.1f KB / %.1f KB",
                                                        ( (float)heap_stats->xAvailableHeapSpaceInBytes / 1024 ),
                                                        ( (float)(configTOTAL_HEAP_SIZE) / 1024 ));
            vPortFree(heap_stats);
            break;
        }
        case 5: { /* freeflsh <-- notice no 'a' to fit into 8 characters */
            flash_usage_t flash_usage;
            // get the flash usage data struct
            flash_usage = onboard_flash_usage();
            tag_print = snprintf(pcInsert, iInsertLen, "%.1f KB / %.1f KB",
                                                        ( (float)flash_usage.flash_free_size / 1024 ),
                                                        ( (float)flash_usage.flash_total_size / 1024 ));
            break;
        }
        case 6: { /* ledstate */
            tag_print = snprintf(pcInsert, iInsertLen, onboard_led_get() ? "ON" : "OFF");
            break;
        }
        case 7: { /* relay1 */
            tag_print = snprintf(pcInsert, iInsertLen, gpio_read_single(0) ? "ON" : "OFF");
            break;
        }
        case 8: { /* relay2 */
            tag_print = snprintf(pcInsert, iInsertLen, gpio_read_single(1) ? "ON" : "OFF");
            break;
        }
        case 9: { /* relay3 */
            tag_print = snprintf(pcInsert, iInsertLen, gpio_read_single(2) ? "ON" : "OFF");
            break;
        }
        case 10: { /* relay4 */
            tag_print = snprintf(pcInsert, iInsertLen, gpio_read_single(3) ? "ON" : "OFF");
            break;
        }
        case 11: { /* relayjson */
            tag_print = snprintf(pcInsert, iInsertLen, "%s", last_relay_status_json);
            break;
        }
        default: { /* unknown tag */
            tag_print = 0;
            break;
        }
    }
    return (u16_t)tag_print;
}

void net_httpd_stack_init(void) {
    // set hostname for lwIP
    char hostname[sizeof(CYW43_HOST_NAME)];
    memcpy(&hostname[0], CYW43_HOST_NAME, sizeof(CYW43_HOST_NAME));
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], hostname);
    
    // initialize lwIP httpd stack
    httpd_init();

    // register cgi and ssi handlers with lwIP
    http_set_cgi_handlers(httpd_cgi_paths, LWIP_ARRAYSIZE(httpd_cgi_paths));
    http_set_ssi_handler(httpd_ssi_handler, httpd_ssi_tags, LWIP_ARRAYSIZE(httpd_ssi_tags));
}

#endif /* ENABLE_HTTPD */