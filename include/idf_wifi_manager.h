/*
 * SPDX-FileCopyrightText: 2024 Rossen Dobrinov
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Copyright 2024 Rossen Dobrinov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Test line for code signing */
/* Test line for code signing */

#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_system.h"

#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "esp_netif_types.h"
#include "../lwip/esp_netif_lwip_internal.h"


#define MACSTR "%X:%X:%X:%X:%X:%X"
#define MAC2STR(macaddr)    ((uint8_t)macaddr[0]), ((uint8_t)macaddr[1]), ((uint8_t)macaddr[2]), \
                            ((uint8_t)macaddr[3]), ((uint8_t)macaddr[4]), ((uint8_t)macaddr[5]) 

typedef enum wm_event_types {
    WM_EVENT_AP_START,          /*!< Access point start */
    WM_EVENT_AP_STOP,           /*!< Access point stop */
    WM_EVENT_STA_CONNECT,       /*!< Station connect to AP */
    WM_EVENT_STA_DISCONNECT,    /*!< Station disconnect from AP*/
    WM_EVENT_GOT_IP,            /*!< Interface got IP */
    WM_EVENT_GOT_TIME,          /*!< Time sync received from NTP */
    WM_EVENT_SCAN_TASK_START,   /*!< Scanning task created */
    /* Extended event notifications*/
    WM_EVENT_STA_MODE_FAIL = 0x100, /*!< Switching to STA only mode failed*/
    WM_EVENT_APSTA_MODE_FAIL,       /*!< Switchig to APSTA mode failed */
    WM_EVENT_IP_SET_OK,             /*!< Successful IP change */
    WM_EVENT_IP_SET_FAIL,           /*!< Unsuccessful IP change */
    WM_EVENT_CC_SET_OK,             /*!< Successful country code change */
    WM_EVENT_CC_SET_FAIL,           /*!< Unuccessful country code change */
    WM_EVENT_KN_ADD_OK,             /*!< Known network added */
    WM_EVENT_KN_ADD_NOMEM,          /*!< Known network add fail (no free memory)*/
    WM_EVENT_KN_ADD_MAX_REACHED,    /*!< Known network add fail (MAX netowork count)*/
    WM_EVENT_KN_DEL_OK,             /*!< Known network deleted */
    WM_EVENT_KN_DEL_FAIL,           /*!< Known network delete failed */
    WM_EVENT_BL_ADD_OK,             /*!< AP added to blacklist */
    WM_EVENT_BL_DEL_OK,             /*!< AP removed from blacklist */
    WM_EVENT_DNS_CHANGE_FAIL,       /*!< DNS address not changed */
    WM_EVENT_AP_STA_CONNECTED,      /*!< Station connected to softAP */
    WM_EVENT_AP_STA_DISCONNECTED,   /*!< Station disconnected from softAP */
    WM_EVENT_EVENT_TYPE_MAX         /*!< MAX EVENT */
} wm_event_t;

ESP_EVENT_DECLARE_BASE(WM_EVENT);

/**
 * @brief Type of full IP configuration used for AP or STA mode
*/
typedef struct wm_net_ip_config {
    esp_netif_ip_info_t static_ip;  /*!< Static IP config or IPADDR_ANY for DHCP */
    esp_ip4_addr_t pri_dns_server;  /*!< Primary DNS server for AP or STA        */
} wm_net_ip_config_t;

/**
 * @brief Type of Wireless network coniguration
*/
typedef struct wm_net_base_config {
    char ssid[33];                  /*!< WiFi SSID             */
    char password[64];              /*!< WiFi Password         */
    wm_net_ip_config_t ip_config;   /*!< Full IP configuration */
} wm_net_base_config_t;

/**
 * @brief Type of Wireless AP
*/
typedef struct wm_apmode_config {
    wm_net_base_config_t base_conf; /*!< Wireless IP and SSID Password config */
    wifi_country_t country;         /*!< Wireless driver country configuration*/
    uint32_t ap_channel;            /*!< Access point channel                 */
} wm_apmode_config_t;

/**
 * @brief Type of single known network configuration 
*/
typedef struct wm_known_net_config {
    wm_net_base_config_t net_config;    /*!< Wireless network configuration   */
    uint32_t net_config_id;             /*!< Configuration ID                 */
} wm_known_net_config_t;

/**
 * Control Interface functions
*/

/**
 * @brief Initialize WiFi manager
 * 
 * @param[in] full_ap_cfg Pointer to full access point configuration. NULL for default 
 * @param[in] p_uevent_loop Pointer to event loop handler for WiFi manager events
 *                          NULL for default default event loop
 * @return 
 *  - ESP_OK Initialization successful
 *  - ESP_FAIL General error
 *  - ESP_ERR_NO_MEM No memory available
 *  - Other - Refer to error codes in esp_err.h
*/
esp_err_t wm_init_wifi_manager( wm_apmode_config_t *full_ap_cfg, esp_event_loop_handle_t *p_uevent_loop);

/**
 * @brief Add new known netowrk by SSID and Password
 * 
 * @param[in] ssid Pointer to NULL terminated string for network SSID
 * @param[in] pwd Pointer to NULL terminated string for network SSID
 * 
 * @return 
 *  - ESP_OK Succeed
 *  - ESP_ERR_NO_MEM Out of memory
 *  - ESP_ERR_INVALID_ARG Network SSID and/or PASSWORD do not meet criteria i.e. PASSWORD not empty and less than 8 characters
 *  - ESP_ERR_NOT_ALLOWED MAX_KNOWN_NETWORKS reached
*/
esp_err_t wm_add_known_network( char *ssid, char *pwd );

/**
 * @brief Add known network with full configuration (i.e. Static IP, Custom DNS server address...)
 * 
 * @param[in] known_network Pointer to full Wireless network coniguration
 * 
 * @return 
 *  - ESP_OK Succeed
 *  - ESP_ERR_NO_MEM Out of memory
 *  - ESP_ERR_INVALID_ARG Network SSID and/or PASSWORD do not meet criteria i.e. PASSWORD not empty and less than 8 characters
 *  - ESP_ERR_NOT_ALLOWED MAX_KNOWN_NETWORKS reached
*/
esp_err_t wm_add_known_network_config( wm_net_base_config_t *known_network);

/**
 * @brief Set WiFi country code 
 * 
 * @param[in] cc Pointer to NULL terminated string for country code
 * 
 * @return
*/
void wm_set_country(char *cc);

/**
 * @brief Change AP mode configuration 
 * 
 * @param[in] ap_conf Pointer to full Wireless network configuration
 * 
 * @return
*/
void wm_change_ap_mode_config( wm_net_base_config_t *ap_conf );

/**
 * @brief Set Primary DNS server address in AP mode. 
 * 
 * @param[in] ap_conf Pointer to full Wireless network configuration
 * 
 * @return
*/
/*void wm_set_ap_primary_dns(esp_ip4_addr_t dns_ip);*/

/**
 * @brief Set DNS server address for known netowork id.
 * 
 * @param[in] dns_ip DNS server address
 * @param[in] known_network_id Internal network ID
 * 
 * @return
*/
void wm_set_sta_dns_by_id(esp_ip4_addr_t dns_ip, uint32_t known_network_id);

/**
 * @brief Set DNS server address for known netowork ssid.
 * 
 * @param[in] dns_ip DNS server address
 * @param[in] ssid Network SSID as NULL terminated string
 * 
 * @return
*/
void wm_set_sta_dns_by_ssid(esp_ip4_addr_t dns_ip, char *ssid);

/**
 * @brief Set secondary DNS server address. *** Read the docs. ***
 * 
 * @param[in] dns_ip DNS server address
 * 
 * @return
*/
void wm_set_secondary_dns(esp_ip4_addr_t dns_ip);

/**
 * @brief Remove known netowork by id.
 * 
 * @param[in] known_network_id Internal network ID
 * 
 * @return
*/
void wm_del_known_net_by_id( uint32_t known_network_id );

/**
 * @brief Remove known netowork by SSID
 * 
 * @param[in] ssid Network SSID as NULL terminated string
 * 
 * @return
*/
void wm_del_known_net_by_ssid( char *ssid );

/**
 * Info API functions
*/

/**
 * @brief Get list of active known networks for STA mode
 *        Free returned pointer after usage to avoid memory leaks
 * 
 * @param[out] size Number of active known networks.
 * @return 
 *      - Pointer to wm_known_net_config_t array with known networks config data
*/
wm_known_net_config_t *wm_get_known_networks(size_t *size);

/**
 * @brief Get wireless configuration of current AP mode internal settings
 * 
 * @param[out] ap_conf Full Wireless configuration
 * 
 * @return
*/
void wm_get_ap_config(wm_net_base_config_t *ap_conf);

/**
 * @brief Get internal ID for known network SSID
 * 
 * @param[in] ssid Network SSID as NULL terminated string
 * 
 * @return
 *      - Known network internal ID. 0 means Known network not found
*/
uint32_t wm_get_kn_config_id(char *ssid);

/**
 * Helper functions
*/

/**
 * @brief Get wireless configuration of current AP mode internal settings
 * 
 * @param[out] full_ap_cfg Variable to fill with default values for AP configuration
 * 
 * @return
*/
void wm_create_apmode_config( wm_apmode_config_t *full_ap_cfg);

/**
 * @brief Get internal ID for known network SSID
 * 
 * @param[in] nm 32 bit value of subnet netmask
 * @return 
 *      - Classless Inter-Domain Routing Prefix
*/
uint8_t wm_netmask_to_cidr(uint32_t nm);


/**
 * For test Only
*/
void wifimgr_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

void wifimgr_dump_ifaces();

#endif /* _WIFI_MANAGER_H_ */