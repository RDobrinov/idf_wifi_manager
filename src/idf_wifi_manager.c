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

#include "idf_wifi_manager.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "sdkconfig.h"

/**
 * @brief Type of Wireless AP/STA interface coniguration
*/
typedef struct wm_wifi_iface {
    esp_netif_t *iface;             /*!< Interface handler              */
    wifi_config_t *driver_config;   /*!< Interface driver configuration */
} wm_wifi_iface_t;

/**
 * @brief Type of Wireless network coniguration for known network node
*/
typedef struct wm_wifi_base_config {
    char *ssid;                     /*!< WiFi SSID             */
    char *password;                 /*!< WiFi Password         */
    wm_net_ip_config_t ip_config;   /*!< Full IPv4 config      */
} wm_wifi_base_config_t;

/**
 * @brief Type of single known network node
*/
typedef struct wm_ll_known_network_node {
    struct {                                 
        wm_wifi_base_config_t net_config;   /*!< Wireless network configuration   */
        uint32_t net_config_id;             /*!< Unique configuration ID          */
    } payload;                              /*!< Node payload structure           */
    struct wm_ll_known_network_node *next;  /*!< Pointer to next linked list node */
} wm_ll_known_network_node_t;

/**
 * @brief Type of blacklisted AP data
*/
typedef struct wm_blist_data {
    uint8_t bssid[6];       /*!< AP Basic Service Set Identifier    */
    uint32_t net_config_id; /*!< Node Identifier based on AP SSID   */
} wm_blist_data_t;

/**
 * @brief Type of single blacklist node
*/
typedef struct wm_ll_blacklist_node {
    wm_blist_data_t payload;            /*!< Blacklist node payload data        */
    struct wm_ll_blacklist_node *next;  /*!< Pointer to next linked list node   */
} wm_ll_blacklist_node_t;

#if (CONFIG_WIFIMGR_AP_CHANNEL == 0)
/**
 * @brief Type of Airband channel ranking
*/
typedef struct wm_airband_rank {
    uint8_t channel[13];    /*!< Count of all AP found in channel   */
    int8_t rssi[13];        /*!< MAX rssi for channel               */
} wm_airband_rank_t;
#endif

/**
 * @brief Type of manager internal running configuration
*/
typedef struct wm_wifi_mgr_config {
    wm_ll_known_network_node_t *known_networks_head;    /*!< Pointer to first node for known network linked list  */
    wm_ll_blacklist_node_t *blacklist_head;             /*!< Pointer to first node for blacklisted AP linked list */
    wm_net_base_config_t ap_conf;                       /*!< Access point mode WiFi configuration holder          */
    wifi_country_t country;                             /*!< Wireless Country Code information holder             */
    esp_ip4_addr_t sec_dns_server;                      /*!< Secondary DNS IPv4 Address                           */
    wm_wifi_iface_t ap;                                 /*!< AP mode interface and driver configuration           */
    wm_wifi_iface_t sta;                                /*!< STA mode interface and driver configuration          */
    TaskHandle_t scanTask_handle;                       /*!< Scan task handle ( Not Used)                         */
    esp_event_loop_handle_t uevent_loop;                /*!< User event loop handler for event notification       */
    union {
        struct {
            uint32_t sta_connected:1;           /*!< STA Conected flag          */
            uint32_t sta_connecting:1;          /*!< STA initiate connect to AP */
            uint32_t scanning:1;                /*!< Scanning enabled flag      */
            uint32_t known_net_count:5;         /*!< Active known networks count*/
            uint32_t sta_connect_retry:3;       /*!< Currend STA connect retry  */
            uint32_t max_sta_connect_retry:3;   /*!< MAX STA connect retry      */
            uint32_t known_ssid:1;              /*!< Known SSID found flag      */
            uint32_t ap_channel:4;              /*!< Configured AP channel      */
            uint32_t scanned_channel:4;         /*!< Number of scanned channel  */
            uint32_t blacklist_reason:1;        /*!< Blacklist reason flag      */
            uint32_t reserved_8:8;              /*!< Reserved                   */
        };
        uint32_t state;                         /*!< State wrapper              */
    }; 
    wifi_ap_record_t found_known_ap;            /*!< Found known AP record when scan finnished  */
} wm_wifi_mgr_config_t;

static wm_wifi_mgr_config_t *wm_run_conf = NULL; /*!< Running configuration */

/**
 * Internal event functions
*/

/**
 * @brief Process WiFi driver events and change internal states
 *  
 * @param[in] arg Data, aside from event data, that is passed when handler is registred
 * @param[in] event_base The base ID of event received
 * @param[in] event_id The ID of event received
 * @param[in] event_data The data, specific to the event
 * 
 * @return 
 * 
*/
static void wm_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

/**
 * @brief Process IP events
 *  
 * @param[in] arg Data, aside from event data, that is passed when handler is registred
 * @param[in] event_base The base ID of event received
 * @param[in] event_id The ID of event received
 * @param[in] event_data The data, specific to the event
 * 
 * @return 
 * 
*/
static void wm_ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

/**
 * @brief Post wifi manager notification event
 * This function post wifi manager event to user event loop passed to init function
 * or to default event loop in case of NULL handler passed in initialization
 *  
 * @param[in] event_id Event ID
 * @param[in] event_data The data, specific to the event
 * @param[in] event_data_size Event data size
 * 
 * @return 
 * 
*/
static void wm_event_post(int32_t event_id, const void *event_data, size_t event_data_size);

/**
 * Internal Known network functions
*/

/**
 * @brief Create an internal known network holder
 * 
 * @param[in] ssid Pointer to NULL terminated string for network SSID
 * @param[in] pwd Pointer to NULL terminated string for network SSID
 * 
 * @return 
 *  - Pointer to newly created holder
*/
static wm_wifi_base_config_t *wm_create_known_network(char *ssid, char *pwd);

/**
 * @brief Search for known network by SSID
 * 
 * @param[in] ssid Pointer to NULL terminated string for network SSID
 * 
 * @return 
 *  - Pointer to first found node or NULL if not found
*/
static wm_ll_known_network_node_t *wm_find_known_net_by_ssid( char *ssid );

/**
 * @brief Create an internal known network holder
 * 
 * @param[in] known_network_id Internal knonw network ID
 * 
 * @return 
 *  - Pointer to found node or NULL if not found
*/
static wm_ll_known_network_node_t *wm_find_known_net_by_id( uint32_t known_network_id );

/**
 * @brief Add new known networn node or replace existing with same ID.
 * Note: Calling this function will delete any blacklisted network with same SSID
 * i.e. calling wm_add_known_network or wm_add_known_network_config APIs will clear all
 * blacklisted MAC with same SSID
 * 
 * @param[in] known_network_id Internal knonw network ID
 * 
 * @return 
 *  - ESP_OK Succeed
 *  - ESP_ERR_NO_MEM Out of memory
 *  - ESP_ERR_INVALID_ARG Network SSID and/or PASSWORD do not meet criteria i.e. PASSWORD not empty and less than 8 characters
 *  - ESP_ERR_NOT_ALLOWED MAX_KNOWN_NETWORKS reached
*/
static esp_err_t wm_add_known_network_node( wm_wifi_base_config_t *known_network);

/**
 * Blacklist opperating functions
*/

/**
 * @brief Add new bssid to blacklisted Access Points
 * 
 * @param[in] bssid MAC address of AP
 * 
 * @return 
 * 
*/
static void wm_add_blist_bssid(wm_blist_data_t *bssid);

/**
 * @brief Remove all blacklisted Access Points with same ID
 * 
 * @param[in] net_config_id Internal ID of blacklisted SSID.
 * 
 * @return 
 * 
*/
static void wm_del_blist_bssid(uint32_t net_config_id);

/**
 * @brief Check presence of bssid in blacklist
 * 
 * @param[in] bssid MAC address of AP
 * 
 * @return 
 *  - Pointer to node of blacklisted AP
*/
static wm_ll_blacklist_node_t *wm_is_blacklisted(uint8_t *bssid);

/**
 * Other functions
*/

/**
 * @brief Set AP mode configuration to internal wifi_config_t holder
 * 
 * @param
 * 
 * @return 
 * 
*/
static void wm_apply_ap_driver_config();

/**
 * @brief Set DNS server address to netif interface
 * 
 * @param[in] iface Pointer to netif interface
 * @param[in] dns_server_ip IPv4 DNS server addres
 * @param[in] type Type of DNS server (ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP or ESP_NETIF_DNS_FALLBACK)
 * 
 * @return 
 * 
*/
static void wm_apply_netif_dns(esp_netif_t *iface, esp_ip4_addr_t *dns_server_ip, esp_netif_dns_type_t type );

/**
 * @brief Set IPv4 address. 
 * This function set static IP or switch to DHCP for specifed interface. 
 * Also trigger extended events IP_SET_OK / IP_SET_FAIL
 * 
 * @param[in] iface Netif interface handler
 * @param[in] ip_info Full IPv4 configuration 
 * 
 * @return 
 * 
*/
static void wm_set_interface_ip( wifi_interface_t iface, wm_net_ip_config_t *ip_info);

/**
 * @brief Free memory occupated by AP and/or STA driver configuration and 
 * running configuration itself. This function is called in case of critical 
 * initialization failure.
 * 
 * @param
 * 
 * @return 
 * 
*/
static void wm_clear_pointers(void);

/**
 * @brief Check SSID and PASSWORD for compliance
 * SSID must be 2-32 printable charachters, 
 * PASSWORD must be 0 or minimum 8 character in lenght
 * 
 * @param[in] ssid Pointer to NULL terminated string for network SSID
 * @param[in] pwd Pointer to NULL terminated string for network SSID
 * 
 * @return 
 *  - ESP_OK compliance check passed
 *  - Other compliance check error
*/
static esp_err_t wm_check_ssid_pwd(char *ssid, char *pwd);

/**
 * @brief Restart WiFi in WIFI_MODE_APSTA and apply working channel
 * Post event WM_EVENT_AP_START or extended event notifocation in case
 * of failed APSTA mode (APSTA_FAIL)
 * 
 * @param
 * 
 * @return 
 * 
*/
static void wm_restart_ap(void);

/**
 * System functions
*/

#if (CONFIG_WIFIMGR_RUN_SNTP_WHEN_STA == 1)
/**
 * @brief SNTP service callback called when tyme sync event occured
 * 
 * @param[in] tv Structure returned by gettimeofday system call
 * 
 * @return
*/
static void wm_sntp_sync_cb(struct timeval *tv);
#endif

/**
 * @brief Scan task function
 * 
 * @param[in] pvParameters A value NULL that is passed as the paramater to the created task
 * 
 * @return
*/
static void vScanTask(void *pvParameters);


ESP_EVENT_DEFINE_BASE(WM_EVENT);


esp_err_t wm_init_wifi_manager( wm_apmode_config_t *full_ap_cfg, esp_event_loop_handle_t *p_uevent_loop) {
    
    if(wm_run_conf) { return ESP_OK; }

    /* Disable wifi log info*/
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    /* Init NVS*/
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if( err != ESP_OK ) return err; 
        err = nvs_flash_init();
    }
    if( err != ESP_OK ) return err; 

    /* Init netif */
    err = esp_netif_init();
    if( err != ESP_OK ) return err;

    /* Create default event loop */
    err = esp_event_loop_create_default();
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) return err; 

    wm_run_conf = (wm_wifi_mgr_config_t *)calloc(1, sizeof(wm_wifi_mgr_config_t));
    if(wm_run_conf) {
        wm_run_conf->uevent_loop = (p_uevent_loop) ? *p_uevent_loop : NULL;
        wm_run_conf->state = 0UL;
        /* Init wifi interfaces */
        wm_run_conf->ap.iface = esp_netif_create_default_wifi_ap();
        wm_run_conf->sta.iface = esp_netif_create_default_wifi_sta();
        /* Setup initial driver configuration */
        wm_run_conf->ap.driver_config = (wifi_config_t *)calloc(1, sizeof(wifi_config_t));
        wm_run_conf->sta.driver_config = (wifi_config_t *)calloc(1, sizeof(wifi_config_t));
        if(!wm_run_conf->ap.driver_config || !wm_run_conf->sta.driver_config) {
            wm_clear_pointers();
            return ESP_ERR_NO_MEM;
        }

        /* Apply ap configuration - passed or default */
        if(!full_ap_cfg) {
            wm_run_conf->ap_conf = (wm_net_base_config_t) {
                .ssid = CONFIG_WIFIMGR_AP_SSID,
                .password = CONFIG_WIFIMGR_AP_PWD,
                .ip_config.static_ip = {{ IPADDR_ANY }, { IPADDR_ANY }, { IPADDR_ANY },},
                .ip_config.pri_dns_server = { IPADDR_ANY }
            };
            wm_run_conf->ap_channel = CONFIG_WIFIMGR_AP_CHANNEL;

            wm_run_conf->max_sta_connect_retry = CONFIG_WIFIMGR_MAX_STA_RETRY;
            wm_run_conf->country = (wifi_country_t ) {
                .cc = CONFIG_WIFIMGR_COUNTRY_CODE,
                .schan = 1,
                .nchan = ((0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "US")) || (0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "01")) )? 11 : 13,
                .policy = WIFI_COUNTRY_POLICY_AUTO
            };
        } else {
            wm_run_conf->ap_conf = full_ap_cfg->base_conf;
            wm_run_conf->ap_channel = full_ap_cfg->ap_channel;
            wm_run_conf->country = full_ap_cfg->country;
        }

        if(wm_run_conf->ap_channel)
            if(((0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "US")) || (0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "01"))) && wm_run_conf->ap_channel >11 ) wm_run_conf->ap_channel = 11;

        /* Apply static IP to AP if any */
        if( wm_run_conf->ap_conf.ip_config.static_ip.ip.addr != IPADDR_ANY ) {
            wm_set_interface_ip(WIFI_IF_AP, &wm_run_conf->ap_conf.ip_config);
        }

        /* Init default WIFI configuration*/
        wifi_init_config_t *_initconf = (wifi_init_config_t *)calloc(1, sizeof(wifi_init_config_t));
        *_initconf = (wifi_init_config_t)WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(_initconf);
        if( ESP_OK != err) {
            wm_clear_pointers();
            return err;
        }
        free(_initconf);

        /* Storage */
        if( esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ) {
            wm_clear_pointers();
            return ESP_FAIL;
        }

        /* Apply country data */
        if( esp_wifi_set_country(&(wm_run_conf->country))) {
            wm_clear_pointers();
            return ESP_FAIL;  
        }

        /* Event handlers registation */
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wm_wifi_event_handler, NULL, NULL);
        if( ESP_OK != err) {
            wm_clear_pointers();
            return err;
        }
        err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wm_ip_event_handler, NULL, NULL);
        if( ESP_OK != err) {
            wm_clear_pointers();
            return err;
        }

        /* Setup AP mode */
        wm_apply_ap_driver_config();

        /* Set initial WiFi mode */
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if(err != ESP_OK) {
            wm_clear_pointers();
            return err;    
        };

        /* Apply initial AP configuration */
        err = esp_wifi_set_config(WIFI_IF_AP, wm_run_conf->ap.driver_config);
        if(err != ESP_OK) {
            wm_clear_pointers();
            return err;
        };

        /* Apply empty STA configuration */
        err = esp_wifi_set_config(WIFI_IF_STA, wm_run_conf->sta.driver_config);
        if(err != ESP_OK) {
            wm_clear_pointers();
            return err;
        };

        /* Set bandwidth to HT20 */
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
        err = esp_wifi_start();
        if( err != ESP_OK ) {
            wm_clear_pointers();
            return err;
        } else { 
            wm_event_post(WM_EVENT_AP_START, NULL, 0);
            esp_netif_ip_info_t *event_data = (esp_netif_ip_info_t *)malloc(sizeof(wifi_init_config_t));
            esp_netif_get_ip_info( wm_run_conf->ap.iface, event_data);
            wm_event_post(WM_EVENT_GOT_IP, event_data, sizeof(esp_netif_ip_info_t));
            free(event_data);
        };

        wm_run_conf->scanning = 1;
        xTaskCreate(vScanTask, "wscan", 2048, NULL, 15, &wm_run_conf->scanTask_handle);
    } else return ESP_ERR_NO_MEM;

    return ESP_OK;
}

esp_err_t wm_add_known_network( char *ssid, char *pwd) {
    if(!wm_run_conf) return ESP_ERR_NOT_ALLOWED;    /* Safety check */
    if(ESP_OK != wm_check_ssid_pwd(ssid, pwd)) return ESP_ERR_INVALID_ARG;
    wm_wifi_base_config_t *new_network = wm_create_known_network(ssid, pwd);
    if(!new_network) return ESP_ERR_NO_MEM;
    esp_err_t err = wm_add_known_network_node(new_network);
    free(new_network);
    return err;
}

esp_err_t wm_add_known_network_config( wm_net_base_config_t *known_network) {
    if(!wm_run_conf) return ESP_ERR_NOT_ALLOWED;    /* Safety check */
    if(ESP_OK != wm_check_ssid_pwd(known_network->ssid, known_network->password)) return ESP_ERR_INVALID_ARG;
    wm_wifi_base_config_t *new_network = wm_create_known_network(known_network->ssid, known_network->password);
    if(!new_network) return ESP_ERR_NO_MEM;
    new_network->ip_config = known_network->ip_config;
    esp_err_t err = wm_add_known_network_node(new_network);
    free(new_network);
    return err;
}

void wm_set_country(char *cc) {
    if(!wm_run_conf) return;    /* Safety check */
    wifi_country_t *new_country = (wifi_country_t *)calloc(1, sizeof(wifi_country_t));
    *new_country = (wifi_country_t ){
        .cc = "",
        .schan = 1,
        .nchan = ((0 == strcmp(cc, "US")) || (0 == strcmp(cc, "01")) )? 11 : 13,
        .policy=WIFI_COUNTRY_POLICY_AUTO
    };
    new_country->cc[0] = cc[0];
    new_country->cc[1] = cc[1];
    memcpy(&wm_run_conf->country, new_country, sizeof(wifi_country_t));
    free(new_country);
    return;
    wm_event_post((esp_wifi_set_country(&(wm_run_conf->country)) == ESP_OK ) ? WM_EVENT_CC_SET_OK : WM_EVENT_CC_SET_FAIL, NULL, 0);
}

void wm_change_ap_mode_config( wm_net_base_config_t *ap_conf ) {
    if(!wm_run_conf) return;    /* Safety check */
    memcpy(&wm_run_conf->ap_conf, ap_conf, sizeof(wm_net_base_config_t));
    if(ap_conf->ip_config.static_ip.ip.addr != IPADDR_ANY) wm_set_interface_ip(WIFI_IF_AP, &ap_conf->ip_config);
    else wm_set_interface_ip(WIFI_IF_AP, NULL);
    wm_apply_ap_driver_config();
    esp_wifi_set_config(WIFI_IF_AP, wm_run_conf->ap.driver_config);
}

void wm_set_sta_dns_by_id(esp_ip4_addr_t dns_ip, uint32_t known_network_id) {
    if(!wm_run_conf) return;    /* Safety check */
    if(known_network_id) {
        wm_ll_known_network_node_t *work = wm_find_known_net_by_id(known_network_id);
        if(work) work->payload.net_config.ip_config.pri_dns_server = dns_ip;
    }
}

void wm_set_sta_dns_by_ssid(esp_ip4_addr_t dns_ip, char *ssid) {
    if(!wm_run_conf) return;    /* Safety check */
    if(ssid) {
        wm_ll_known_network_node_t *work = wm_find_known_net_by_ssid(ssid);
        if(work) work->payload.net_config.ip_config.pri_dns_server = dns_ip;
    }
}

void wm_set_secondary_dns(esp_ip4_addr_t dns_ip) {
    if(!wm_run_conf) return;    /* Safety check */
    wm_run_conf->sec_dns_server = dns_ip;
}

void wm_del_known_net_by_id( uint32_t known_network_id ) {
    if(!wm_run_conf) return;    /* Safety check */
    if(!known_network_id) {
        wm_event_post(WM_EVENT_KN_DEL_FAIL, NULL, 0);
        return;
    }
    wm_ll_known_network_node_t *work = wm_run_conf->known_networks_head;
    wm_ll_known_network_node_t *prev = NULL;
    bool for_delete = false;
    while( !for_delete && work) {
        for_delete = (known_network_id == work->payload.net_config_id);
        if(for_delete) {
            if(!prev) wm_run_conf->known_networks_head = work->next; //head node going to be deleted
            else prev->next = work->next;
            /* Free SSID and Password and release node */
            wm_event_post(WM_EVENT_KN_DEL_OK, &(work->payload.net_config_id), sizeof(int32_t));
            free(work->payload.net_config.ssid);
            free(work->payload.net_config.password);
            free(work);
            (wm_run_conf->known_net_count)--;
        } else {
            prev = work;
            work = work->next;
        }
    }
    if(!for_delete) wm_event_post(WM_EVENT_KN_DEL_FAIL, NULL, 0);
    return;
}

void wm_del_known_net_by_ssid( char *ssid ) {
    if(!wm_run_conf) return;    /* Safety check */
    wm_del_known_net_by_id(wm_get_kn_config_id(ssid));
    return;
    //return wm_del_known_net_by_id(wm_get_config_id(ssid));
}

wm_known_net_config_t *wm_get_known_networks(size_t *size) {
    *size = 0;
    if(!wm_run_conf) return NULL;     /* Safety check */
    wm_ll_known_network_node_t *work = wm_run_conf->known_networks_head;
    wm_known_net_config_t *known_net = NULL;
    if(work) {
        known_net = (wm_known_net_config_t *)calloc(wm_run_conf->known_net_count, sizeof(wm_known_net_config_t));
        while(work && (*size < wm_run_conf->known_net_count)) {
            known_net[*size].net_config.ip_config = work->payload.net_config.ip_config;
            known_net[*size].net_config_id = work->payload.net_config_id;
            strcpy(known_net[*size].net_config.ssid, work->payload.net_config.ssid);
            strcpy(known_net[*size].net_config.password, work->payload.net_config.password);
            (*size)++;
            work = work->next;
        }
    } else {
        *size = 0;
    }
    return (*size) ? known_net : NULL;
}

void wm_get_ap_config(wm_net_base_config_t *ap_conf) {
    if(!wm_run_conf) return;    /* Safety check */
    memcpy(ap_conf, (char *)&wm_run_conf->ap_conf, sizeof(wm_net_base_config_t));
}

uint32_t wm_get_kn_config_id(char *ssid) {
    if(!wm_run_conf) return 0;    /* Safety check */
    wm_ll_known_network_node_t *work = wm_find_known_net_by_ssid(ssid);
    return (work) ? work->payload.net_config_id : 0;
}

void wm_create_apmode_config( wm_apmode_config_t *full_ap_cfg) {
    if(!full_ap_cfg) return;
    *full_ap_cfg = (wm_apmode_config_t) {
        .base_conf = (wm_net_base_config_t) {
            .ssid = CONFIG_WIFIMGR_AP_SSID,
            .password = CONFIG_WIFIMGR_AP_PWD,
            .ip_config.static_ip = {{ IPADDR_ANY }, { IPADDR_ANY }, { IPADDR_ANY }},
            .ip_config.pri_dns_server = { IPADDR_ANY }
        },
        .country = (wifi_country_t ){ 
            .cc = CONFIG_WIFIMGR_COUNTRY_CODE,
            .schan = 1,
            .nchan = ((0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "US")) || (0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "01")) )? 11 : 13,
            .policy = WIFI_COUNTRY_POLICY_AUTO
        },
        .ap_channel = CONFIG_WIFIMGR_AP_CHANNEL
    };
}

uint8_t wm_netmask_to_cidr(uint32_t nm)
{
    nm = nm - ((nm >> 1) & ((u32_t)0x55555555UL));
    nm = (nm & ((u32_t)0x33333333UL)) + ((nm >> 2) & ((u32_t)0x33333333UL));
    nm = (nm + (nm >> 4)) & ((u32_t)0x0F0F0F0FUL);
    nm = nm + (nm >> 8);
    nm = nm + (nm >> 16);
    return (uint8_t)( nm & ((u32_t)0x0000003FUL));
}

/**
 * Internal event functions
*/

static void wm_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if(event_id == WIFI_EVENT_SCAN_DONE) {
            if(((wifi_event_sta_scan_done_t *)event_data)->status == 0) {
                uint16_t found_ap_count = 0;
                #if (CONFIG_WIFIMGR_AP_CHANNEL == 0)
                wm_airband_rank_t airband;
                #endif
                esp_wifi_scan_get_ap_num(&found_ap_count);
                wifi_ap_record_t *found_ap_info = (wifi_ap_record_t *)calloc(found_ap_count, sizeof(wifi_ap_record_t));
                esp_wifi_scan_get_ap_records(&found_ap_count, found_ap_info);
                if(!wm_run_conf->scanned_channel) {
                    memset(&(wm_run_conf->found_known_ap), 0, sizeof(wifi_ap_record_t));
                #if (CONFIG_WIFIMGR_AP_CHANNEL == 0)
                    memset(&airband.channel, 0, sizeof(airband.channel));
                    memset(&airband.rssi, 0b10011111, sizeof(airband.rssi)); /* set min RSSI */
                #endif
                }
                #if (CONFIG_WIFIMGR_AP_CHANNEL == 0)
                else {
                    airband.channel[wm_run_conf->scanned_channel-1] = 0;
                    airband.rssi[wm_run_conf->scanned_channel-1] = 0b10011111;
                }
                #endif

                wm_run_conf->known_ssid = 0;
                for(int i=0; ( i<found_ap_count ); i++) {
                    if(!wm_run_conf->known_ssid && !wm_run_conf->scanned_channel) {
                        /* If not blacklisted */
                        if(!wm_is_blacklisted(found_ap_info[i].bssid)) {
                            wm_ll_known_network_node_t *found_ssid = wm_find_known_net_by_ssid((char *)found_ap_info[i].ssid);
                            if(found_ssid) {
                                /* AP in list found in known networks */
                                wm_run_conf->found_known_ap = found_ap_info[i];
                                wm_run_conf->known_ssid = 1;
                            }
                        }
                    }
                    #if (CONFIG_WIFIMGR_AP_CHANNEL == 0)
                    if(wm_run_conf->ap_channel == 0) {
                        airband.channel[found_ap_info[i].primary-1]++;
                        if(airband.rssi[found_ap_info[i].primary-1] < found_ap_info[i].rssi) {airband.rssi[found_ap_info[i].primary-1] = found_ap_info[i].rssi;}
                        if(found_ap_info[i].primary-2 > 0) {
                            airband.channel[found_ap_info[i].primary-2]++;
                            if(airband.rssi[found_ap_info[i].primary-2] < found_ap_info[i].rssi) {airband.rssi[found_ap_info[i].primary-2] = found_ap_info[i].rssi;}
                        }
                        if(found_ap_info[i].primary < 13 ) {
                            airband.channel[found_ap_info[i].primary]++;
                            if(airband.rssi[found_ap_info[i].primary] < found_ap_info[i].rssi) {airband.rssi[found_ap_info[i].primary] = found_ap_info[i].rssi;}
                        }
                        if(found_ap_info[i].second != WIFI_SECOND_CHAN_NONE ) {
                            for(int b=1; b<5; b++) {
                                if( (found_ap_info[i].second == WIFI_SECOND_CHAN_ABOVE) ) { 
                                    if((found_ap_info[i].primary+b) < 13 ) {
                                        airband.channel[found_ap_info[i].primary+b]++;
                                        if(airband.rssi[found_ap_info[i].primary+b] < found_ap_info[i].rssi) {airband.rssi[found_ap_info[i].primary+b] = found_ap_info[i].rssi;}
                                    }
                                } else {
                                    if((found_ap_info[i].primary-b-1) > 0 ) {
                                        airband.channel[found_ap_info[i].primary-b-1]++;
                                        if(airband.rssi[found_ap_info[i].primary-b-1] < found_ap_info[i].rssi) {airband.rssi[found_ap_info[i].primary-b-1] = found_ap_info[i].rssi;}
                                    }
                                }
                            }
                        }
                    }
                    #endif
                }
                #if (CONFIG_WIFIMGR_AP_CHANNEL == 0)
                if(wm_run_conf->ap_channel == 0) {
                    int iRatedChannel = 0;
                    float fRatedRSSI = 0.0f, fCalcRSSI = 0.0f;
                    for(int i=0; i<13; i++) {
                        if(i==0) { fCalcRSSI = (float)(airband.channel[i] + airband.rssi[i]*10 + airband.channel[i+1] + airband.rssi[i+1]*10)/2; }
                        else if (i==12) { fCalcRSSI = (float)(airband.channel[i] + airband.rssi[i]*10 + airband.channel[i-1] + airband.rssi[i-1]*10)/2; }
                        else { fCalcRSSI = (float)(airband.channel[i] + airband.rssi[i]*10 + airband.channel[i-1] + airband.rssi[i-1]*10+ airband.channel[i+1] + airband.rssi[i+1]*10)/3;}
                        if( fRatedRSSI>fCalcRSSI ) {
                            fRatedRSSI = fCalcRSSI;
                            iRatedChannel = i+1;
                        }
                    }
                    if( wm_run_conf->ap.driver_config->ap.channel != iRatedChannel ) {
                        wm_run_conf->ap.driver_config->ap.channel = iRatedChannel;
                    };
                }
                #endif
                free(found_ap_info);
                wm_run_conf->scanning = 1;
                if(!(wm_run_conf->sta_connected)) {
                    if(strlen((char *)wm_run_conf->found_known_ap.ssid) > 0 ) {
                        if(!wm_run_conf->sta_connecting) {
                            wm_ll_known_network_node_t *net_conf = wm_find_known_net_by_ssid((char *)wm_run_conf->found_known_ap.ssid);
                            if(net_conf) {
                                strcpy((char *)wm_run_conf->sta.driver_config->sta.ssid, net_conf->payload.net_config.ssid);
                                strcpy((char *)wm_run_conf->sta.driver_config->sta.password, net_conf->payload.net_config.password);
                                wm_run_conf->sta.driver_config->sta.bssid_set = 1;
                                memcpy(wm_run_conf->sta.driver_config->sta.bssid, wm_run_conf->found_known_ap.bssid, 6);
                                wm_run_conf->sta.driver_config->sta.channel = wm_run_conf->found_known_ap.primary;
                                if( ESP_OK == esp_wifi_set_config(WIFI_IF_STA, wm_run_conf->sta.driver_config)) {
                                    wm_run_conf->sta_connecting = 1;
                                    wm_run_conf->sta_connect_retry = 0;
                                    esp_wifi_connect();
                                } else {
                                    /* Notification for failed connect */
                                    wm_event_post(WM_EVENT_STA_MODE_FAIL, NULL, 0);
                                }
                            }
                        }
                    } else {
                        wm_restart_ap();
                    }
                }
            }
            return;
        }

        if ( event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
            return;
        }

        if ( event_id == WIFI_EVENT_STA_CONNECTED ) {
            wm_event_post(WM_EVENT_STA_CONNECT, &wm_run_conf->found_known_ap, sizeof(wifi_ap_record_t));
            /* Delete all blacklisted AP when one is successfuly connected */
            wm_del_blist_bssid(esp_rom_crc32_le(0, (const unsigned char *)wm_run_conf->sta.driver_config->sta.ssid, strlen((const char *)wm_run_conf->sta.driver_config->sta.ssid)));
            wm_set_interface_ip(WIFI_IF_STA, &((wm_find_known_net_by_ssid((char *)wm_run_conf->found_known_ap.ssid))->payload.net_config.ip_config));

            wm_run_conf->blacklist_reason = 0;
            #if (CONFIG_WIFIMGR_RUN_SNTP_WHEN_STA == 1)
            esp_sntp_config_t *sntp_config = (esp_sntp_config_t *)malloc(sizeof(esp_sntp_config_t));
            *sntp_config = (esp_sntp_config_t) { 
                .smooth_sync = 0, 
                .server_from_dhcp = 0, 
                .wait_for_sync = 1, 
                .start = 1, 
                .sync_cb = wm_sntp_sync_cb,     //((void *)0)
                .renew_servers_after_new_IP = 0, 
                .ip_event_to_renew = IP_EVENT_STA_GOT_IP, 
                .index_of_first_server = 0, 
                .num_of_servers = (1), 
                .servers = {"pool.ntp.org"} // From config???
                };
            esp_netif_sntp_init(sntp_config);
            free(sntp_config);
            #endif
            return;
        }

        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            #if (CONFIG_WIFIMGR_RUN_SNTP_WHEN_STA == 1)
                /* Destroy sntp */
                esp_netif_sntp_deinit();
            #endif
            wm_run_conf->blacklist_reason |= ((((wifi_event_sta_disconnected_t *)event_data)->reason) > 201);
            if (wm_run_conf->sta_connect_retry < wm_run_conf->max_sta_connect_retry) {
                esp_wifi_connect();
                wm_run_conf->sta_connect_retry++;
            } else {
                /* Clear connecting and connected bits */
                wm_run_conf->state &= 0xFFFFFFFCUL;
                wm_run_conf->scanning = 1;
                wm_event_post(WM_EVENT_STA_DISCONNECT, NULL, 0);
                if(wm_run_conf->blacklist_reason) {
                    wm_blist_data_t *bbssid = (wm_blist_data_t *)calloc(1, sizeof(wm_blist_data_t));
                    if(bbssid) {
                        memcpy(bbssid->bssid, wm_run_conf->sta.driver_config->sta.bssid, 6);
                        bbssid->net_config_id = esp_rom_crc32_le(0, (const unsigned char *)wm_run_conf->sta.driver_config->sta.ssid, strlen((const char *)wm_run_conf->sta.driver_config->sta.ssid));
                        wm_add_blist_bssid(bbssid);
                        free(bbssid);
                        bbssid = NULL;
                    }
                }
                wm_run_conf->blacklist_reason = 0;
            }
        }
    }
}

static void wm_ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wm_run_conf->sta_connect_retry = 0;
        wm_event_post(WM_EVENT_GOT_IP, (void *)&(((ip_event_got_ip_t *)event_data)->ip_info), sizeof(esp_netif_ip_info_t));
        wm_run_conf->sta_connected = 1;
        wm_run_conf->sta_connecting = 0;
        if(esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
            /* It's a warning state - Station connected, but AP is still running */
            wm_event_post(WM_EVENT_STA_MODE_FAIL, NULL, 0);
        } else { wm_event_post(WM_EVENT_AP_STOP, NULL, 0); }
    }
}

static void wm_event_post(int32_t event_id, const void *event_data, size_t event_data_size) {
    if(wm_run_conf->uevent_loop) esp_event_post_to(wm_run_conf->uevent_loop, WM_EVENT, event_id, event_data, event_data_size, 1);
    else esp_event_post(WM_EVENT, event_id, event_data, event_data_size, 1);
    return;
}

/**
 * Internal Known network functions
*/

static wm_wifi_base_config_t *wm_create_known_network(char *ssid, char *pwd) {
    wm_wifi_base_config_t *new_network = calloc(1, sizeof(wm_wifi_base_config_t));
    if(!new_network) return NULL;
    size_t len = strlen(ssid);
    len = (len>32) ? 32 : len;
    new_network->ssid = calloc(len+1, sizeof(char));
    if(!new_network->ssid) {
        free(new_network);
        return NULL;
    }
    memcpy(new_network->ssid, ssid, len);
    len = strlen(pwd);
    len = (len>64) ? 64 : len;
    new_network->password = calloc(len+1, sizeof(char));
    if(!new_network->password) {
        free(new_network->ssid);
        free(new_network);
        return NULL;
    }
    memcpy(new_network->password, pwd, len);
    return new_network;
}

static wm_ll_known_network_node_t *wm_find_known_net_by_ssid( char *ssid ) {
    wm_ll_known_network_node_t *work = NULL;
    if(ssid) {
        work = wm_run_conf->known_networks_head;
        bool found = false;
        while(!found && work) {
            found = !strcmp(ssid, work->payload.net_config.ssid);
            if(!found) work = work->next;
        }
    }
    return work;
}

static wm_ll_known_network_node_t *wm_find_known_net_by_id( uint32_t known_network_id ) {
    wm_ll_known_network_node_t *work = NULL;
    if(known_network_id) {
        work = wm_run_conf->known_networks_head;
        bool found = false;
        while(!found && work) {
            found = (work->payload.net_config_id == known_network_id);
            if(!found) work = work->next;
        }
    }
    return work;
}

static esp_err_t wm_add_known_network_node( wm_wifi_base_config_t *known_network) {
    if(wm_run_conf->known_net_count >= CONFIG_WIFIMGR_MAX_KNOWN_NETWORKS) {
        wm_event_post(WM_EVENT_KN_ADD_MAX_REACHED, NULL, 0);
        return ESP_ERR_NOT_ALLOWED;
    }
    wm_ll_known_network_node_t *work = calloc(1, sizeof(wm_ll_known_network_node_t));
    if(work) {
        work->payload.net_config = *known_network;
        work->payload.net_config_id = esp_rom_crc32_le(0, (const unsigned char *)known_network->ssid, strlen(known_network->ssid));
        wm_del_blist_bssid(work->payload.net_config_id);
        work->payload.net_config_id = esp_rom_crc32_le(work->payload.net_config_id, (const unsigned char *)known_network->password, strlen(known_network->password));
        if(wm_find_known_net_by_id(work->payload.net_config_id)) wm_del_known_net_by_id(work->payload.net_config_id); /* Prevents false event flood */
        /* Semaphore */
        work->next = wm_run_conf->known_networks_head;
        wm_run_conf->known_networks_head = work;
        (wm_run_conf->known_net_count)++;
        /* release */
        wm_event_post(WM_EVENT_KN_ADD_OK, &(work->payload.net_config_id), sizeof(uint32_t));
        return ESP_OK;
    }
    wm_event_post(WM_EVENT_KN_ADD_NOMEM, NULL, 0);
    return ESP_ERR_NO_MEM;
}

/**
 * Blacklist opperating functions
*/

static void wm_add_blist_bssid(wm_blist_data_t *bssid) {
    if(!bssid) return;
    wm_ll_blacklist_node_t *bnode = wm_is_blacklisted(bssid->bssid);
    if(bnode) {
        bnode->payload.net_config_id = bssid->net_config_id;
    } else {
        wm_ll_blacklist_node_t *node = (wm_ll_blacklist_node_t *)calloc(1, sizeof(wm_ll_blacklist_node_t));
        if(node) {
            memcpy(node->payload.bssid, bssid, sizeof(wm_blist_data_t));
            node->next = wm_run_conf->blacklist_head;
            wm_run_conf->blacklist_head = node;
            wm_event_post(WM_EVENT_BL_ADD_OK, bssid, sizeof(wm_blist_data_t));
        }
    }
}

static void wm_del_blist_bssid(uint32_t net_config_id) {
    wm_ll_blacklist_node_t *work = wm_run_conf->blacklist_head;
    wm_ll_blacklist_node_t *prev = NULL;
    bool found = false;
    while(!found && work) {
        found = (net_config_id == work->payload.net_config_id);
        if(!found) {
            prev = work;
            work = work->next;
        } else {
            if(prev) prev->next = work->next;
            else wm_run_conf->blacklist_head = work->next;
            wm_event_post(WM_EVENT_BL_DEL_OK, NULL, 0);
            free(work);
            work = (prev) ? prev->next : wm_run_conf->blacklist_head;
        }
    }
}

static wm_ll_blacklist_node_t *wm_is_blacklisted(uint8_t *bssid) {
    wm_ll_blacklist_node_t *work = wm_run_conf->blacklist_head;
    bool found = false;
    while(!found && work) {
        found = !memcmp(work->payload.bssid, bssid, 6);
        if(!found) work = work->next;
    }
    return work;
}

/**
 * Other functions
*/

static void wm_apply_ap_driver_config() {
    strcpy((char *)wm_run_conf->ap.driver_config->ap.ssid, wm_run_conf->ap_conf.ssid);
    wm_run_conf->ap.driver_config->ap.channel = (wm_run_conf->ap_channel != 0) ? wm_run_conf->ap_channel : CONFIG_WIFIMGR_DEFAULT_AP_CHANNEL;
    wm_run_conf->ap.driver_config->ap.max_connection = 1;
    wm_run_conf->ap.driver_config->ap.authmode = 
        (strlen(strcpy((char *)wm_run_conf->ap.driver_config->ap.password, wm_run_conf->ap_conf.password)) != 0) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wm_run_conf->ap.driver_config->ap.pairwise_cipher = WIFI_CIPHER_TYPE_TKIP; //Kconfig param...
    wm_run_conf->ap.driver_config->ap.pmf_cfg = (wifi_pmf_config_t) { .required = true };
}

static void wm_apply_netif_dns(esp_netif_t *iface, esp_ip4_addr_t *dns_server_ip, esp_netif_dns_type_t type ) {
    esp_netif_dns_info_t *dns = (esp_netif_dns_info_t *)calloc(1, sizeof(esp_netif_dns_info_t));
    *dns = (esp_netif_dns_info_t) { .ip = (esp_ip_addr_t){.u_addr.ip4 = *dns_server_ip, .type = ESP_IPADDR_TYPE_V4 }};
    esp_err_t err = esp_netif_set_dns_info(iface, type, dns);
    free(dns);
    if(ESP_OK != err) wm_event_post(WM_EVENT_DNS_CHANGE_FAIL, NULL, 0);
    return;
}

void wm_set_interface_ip( wifi_interface_t iface, wm_net_ip_config_t *ip_info)
{
    bool _ok = true;
    esp_netif_dhcp_status_t dhcp_status;
    esp_err_t err;

    if(iface != WIFI_IF_STA && iface != WIFI_IF_AP) return;
    esp_netif_ip_info_t *new_ip_info = (esp_netif_ip_info_t *)calloc(1, sizeof(esp_netif_ip_info_t));
    if( ip_info == NULL ) {
       *new_ip_info = (WIFI_IF_AP == iface) ? (esp_netif_ip_info_t) {
            .ip = { ((u32_t)0x0104A8C0UL) }, 
            .gw = { ((u32_t)0x0104A8C0UL) },
            .netmask = { ((u32_t)0x00FFFFFFUL) }
        } : (esp_netif_ip_info_t) {
            .ip = { ((u32_t)0x00000000UL) }, 
            .gw = { ((u32_t)0x00000000UL) },
            .netmask = { ((u32_t)0x00000000UL) }
        };
    } else { memcpy(new_ip_info, &ip_info->static_ip, sizeof(esp_netif_ip_info_t)); }

    if( iface == WIFI_IF_AP ) {
        esp_netif_dhcps_get_status(wm_run_conf->ap.iface, &dhcp_status);
        if(ESP_NETIF_DHCP_STOPPED != dhcp_status) {
            err = esp_netif_dhcps_stop(wm_run_conf->ap.iface);
            if(ESP_OK != err && ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED != err) _ok = false;
        }
        if(_ok) {
            err = esp_netif_set_ip_info(wm_run_conf->ap.iface, new_ip_info); 
            _ok &= ( ESP_OK == err );
            if( ip_info != NULL ) {
                if(ip_info->pri_dns_server.addr != IPADDR_ANY) {
                    wm_apply_netif_dns(wm_run_conf->ap.iface, &ip_info->pri_dns_server, ESP_NETIF_DNS_MAIN);
                }
            } else {
                wm_apply_netif_dns(wm_run_conf->ap.iface, &new_ip_info->ip, ESP_NETIF_DNS_MAIN);
            }
            _ok &= ( err == ESP_OK );
            err = esp_netif_dhcps_start(wm_run_conf->ap.iface);
            _ok &= (( err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED));
        }
    } else {
        esp_netif_dhcpc_get_status(wm_run_conf->sta.iface, &dhcp_status);
        if(ESP_NETIF_DHCP_STOPPED != dhcp_status) {
            err = esp_netif_dhcpc_stop(wm_run_conf->sta.iface);
            if(ESP_OK != err && ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED != err) {
                _ok = false;
            }
        }
        if( _ok ) {
            err = esp_netif_set_ip_info(wm_run_conf->sta.iface, new_ip_info); 
            _ok &= ( ESP_OK == err );
            if(ip_info != NULL) {
                if(ip_info->pri_dns_server.addr != IPADDR_ANY) {
                    /* Get sta dns address from dhcp */
                    wm_apply_netif_dns(wm_run_conf->sta.iface, &ip_info->pri_dns_server, (new_ip_info->ip.addr == IPADDR_ANY) ? ESP_NETIF_DNS_FALLBACK : ESP_NETIF_DNS_MAIN );
                }
            }
            if(new_ip_info->ip.addr == IPADDR_ANY )
            {
                err = esp_netif_dhcpc_start(wm_run_conf->sta.iface);
                _ok &= (( err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED));
            } else {
                if(wm_run_conf->sec_dns_server.addr != IPADDR_ANY) {
                    wm_apply_netif_dns(wm_run_conf->sta.iface, &wm_run_conf->sec_dns_server, ESP_NETIF_DNS_BACKUP);
                }
            }
        }
    }
    free(new_ip_info);
    wm_event_post(_ok ? WM_EVENT_IP_SET_OK : WM_EVENT_IP_SET_FAIL, NULL, 0);
    return;
}

static void wm_clear_pointers(void) {
    if(wm_run_conf->ap.driver_config) free(wm_run_conf->ap.driver_config);
    if(wm_run_conf->sta.driver_config) free(wm_run_conf->sta.driver_config);
    free(wm_run_conf);
}

static esp_err_t wm_check_ssid_pwd(char *ssid, char *pwd) {
    size_t pwd_length = strlen(pwd);
    return ((strlen(ssid) < 2) || ( pwd_length>0 && pwd_length<8));
}

static void wm_restart_ap(void) {
    wifi_mode_t *wifi_run_mode = (wifi_mode_t *)calloc(1, sizeof(wifi_mode_t));
    esp_wifi_get_mode(wifi_run_mode);
    if(*wifi_run_mode != WIFI_MODE_APSTA) {
        if(esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
            wm_event_post(WM_EVENT_APSTA_MODE_FAIL, NULL, 0);
        } else { 
            esp_wifi_set_channel(wm_run_conf->ap.driver_config->ap.channel, WIFI_SECOND_CHAN_NONE);
            wm_event_post(WM_EVENT_AP_START, NULL, 0);
        }
    }
    free(wifi_run_mode);
    return;
}

/**
 * System functions
*/

#if (CONFIG_WIFIMGR_RUN_SNTP_WHEN_STA == 1)
static void wm_sntp_sync_cb(struct timeval *tv) {
    wm_event_post(WM_EVENT_GOT_TIME, tv, sizeof(struct timeval));
}
#endif

static void vScanTask(void *pvParameters)
{
    wm_event_post(WM_EVENT_SCAN_TASK_START, NULL, 0);
    wifi_mode_t wifi_run_mode = WIFI_MODE_MAX;
    TickType_t xDelayTicks = (2500 / portTICK_PERIOD_MS);
    wifi_scan_config_t cfg = {NULL, NULL, 0, true, WIFI_SCAN_TYPE_ACTIVE, (wifi_scan_time_t){{0, 120}, 320}, 255};
    while(true) {
        if(esp_wifi_get_mode(&wifi_run_mode) == ESP_OK) {
            if((wm_run_conf->sta_connect_retry >= wm_run_conf->max_sta_connect_retry) || (wifi_run_mode == WIFI_MODE_APSTA) || ((wifi_run_mode == WIFI_MODE_STA) && (wm_run_conf->sta_connected))) {    
                if ( !(wm_run_conf->sta_connecting) && (wm_run_conf->scanning) ) {
                    if(wm_run_conf->known_networks_head) {
                        wm_run_conf->scanning = 0;
                        if(!(wm_run_conf->sta_connected)) {
                            cfg.channel = 0;
                            wm_run_conf->scanned_channel = 0;
                            esp_wifi_scan_start(&cfg, false);
                        } 
                        #if (CONFIG_WIFIMGR_AP_CHANNEL == 0)
                        else {
                            cfg.channel++;
                            if(cfg.channel > wm_run_conf->country.nchan) cfg.channel = 1;
                            wm_run_conf->scanned_channel = cfg.channel;
                            esp_wifi_scan_start(&cfg, false);
                        }
                        #endif
                        xDelayTicks = (wm_run_conf->sta_connected) ? (5000 / portTICK_PERIOD_MS) : (2500 / portTICK_PERIOD_MS);
                    } else wm_restart_ap();
                } 
            } else { xDelayTicks = (1000 / portTICK_PERIOD_MS); }
        } else { xDelayTicks = (500 / portTICK_PERIOD_MS); }
        vTaskDelay(xDelayTicks);
    }
}

/*void wm_set_ap_primary_dns(esp_ip4_addr_t dns_ip) {
    wm_run_conf->ap_conf.ip_config.pri_dns_server = dns_ip;
}*/

/**
 * For test Only!
*/

void wifimgr_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    //static char *wm = "wm";
    if(WM_EVENT ==  event_base ){
        if( event_id == WM_EVENT_GOT_IP ) {
            /*ESP_LOGI("main", "WM_EVENT_NETIF_GOT_IP");
            esp_netif_t *test_netif=esp_netif_get_default_netif();
            if(test_netif == NULL) {
                ESP_LOGE("probe:netif", "No active netif");
            } else {
                ESP_LOGE("probe:ifkey", "%s", esp_netif_get_ifkey(test_netif));
            }*/
            ESP_LOGI("WM_EVENT_GOT_IP", IPSTR "/%u " IPSTR, IP2STR(&((esp_netif_ip_info_t *)event_data)->ip), wm_netmask_to_cidr(((esp_netif_ip_info_t *)event_data)->netmask.addr), IP2STR(&((esp_netif_ip_info_t *)event_data)->gw));
        }
        if(event_id == WM_EVENT_STA_CONNECT ) {
            ESP_LOGI("WM_EVENT_STA_CONNECT", "[%s | " MACSTR "] RSSI: %d on %u channel" , ((wifi_ap_record_t *)event_data)->ssid, MAC2STR(((wifi_ap_record_t *)event_data)->bssid), ((wifi_ap_record_t *)event_data)->rssi, ((wifi_ap_record_t *)event_data)->primary);
        }
        if(event_id == WM_EVENT_STA_DISCONNECT ) {
            ESP_LOGI("WM_EVENT_STA_DISCONNECT", "");
        }
        if(event_id == WM_EVENT_AP_START ) {
            //ESP_LOGI(wm, "WM_EVENT_AP_START");
            //wm_net_base_config_t *ap_conf = (wm_net_base_config_t *)malloc(sizeof(wm_net_base_config_t));
            //wm_get_ap_config(ap_conf);
            ESP_LOGI("WM_EVENT_AP_START", "SSID %s / PWD %s", wm_run_conf->ap.driver_config->ap.ssid, wm_run_conf->ap.driver_config->ap.password);
        }
        if(event_id == WM_EVENT_AP_STOP ) {
            ESP_LOGI("WM_EVENT_AP_STOP", "");
        }

        if(event_id == WM_EVENT_GOT_TIME) {
            struct tm *lt_info;
            setenv("TZ","EET-2,M3.5.0/3,M10.5.0/4",1);
            tzset();
            lt_info = localtime(&((struct timeval *)event_data)->tv_sec);
            ESP_LOGI("WM_EVENT_GOT_TIME", "%s", asctime(lt_info));
        }

        if(event_id == WM_EVENT_IP_SET_OK ) {
            ESP_LOGI("WM_EVENT_IP_SET_OK", "");
        }
        if(event_id == WM_EVENT_IP_SET_FAIL ) {
            ESP_LOGI("WM_EVENT_IP_SET_FAIL", "");
        }
        if(event_id == WM_EVENT_CC_SET_OK ) {
            ESP_LOGI("WM_EVENT_CC_SET_OK", "");
        }
        if(event_id == WM_EVENT_CC_SET_FAIL ) {
            ESP_LOGI("WM_EVENT_CC_SET_FAIL", "");
        }
        if(event_id == WM_EVENT_KN_ADD_OK ) {
            ESP_LOGI("WM_EVENT_KN_ADD_OK", "0x%lX", *((uint32_t *)event_data));
        }
        if(event_id == WM_EVENT_KN_ADD_MAX_REACHED ) {
            ESP_LOGI("WM_EVENT_KN_ADD_MAX_REACHED", "");
        }
        if(event_id == WM_EVENT_KN_DEL_OK ) {
            ESP_LOGI("WM_EVENT_KN_DEL_OK", "0x%lX", *((uint32_t *)event_data));
        }
        if(event_id == WM_EVENT_KN_DEL_FAIL ) {
            ESP_LOGI("WM_EVENT_KN_DEL_FAIL", "");
        }
        //WM_EVENT_BL_ADD_OK,             /*!< AP added to blacklist */
        //WM_EVENT_BL_DEL_OK,             /*!< AP removed from blacklist */
        //WM_EVENT_DNS_CHANGE_FAIL,       /*!< DNS address not changed */
    }
}

void wifimgr_dump_ifaces() {
    const char *tag = "dump";
    esp_netif_dns_info_t dns_m, dns_b, dns_f;
    esp_netif_get_dns_info(wm_run_conf->ap.iface, ESP_NETIF_DNS_MAIN, &dns_m);
    esp_netif_get_dns_info(wm_run_conf->ap.iface, ESP_NETIF_DNS_BACKUP, &dns_b);
    esp_netif_get_dns_info(wm_run_conf->ap.iface, ESP_NETIF_DNS_FALLBACK, &dns_f);
    ESP_LOGI(tag, IPSTR " " IPSTR " " IPSTR, IP2STR(&dns_m.ip.u_addr.ip4), IP2STR(&dns_b.ip.u_addr.ip4), IP2STR(&dns_f.ip.u_addr.ip4));
    esp_netif_get_dns_info(wm_run_conf->sta.iface, ESP_NETIF_DNS_MAIN, &dns_m);
    esp_netif_get_dns_info(wm_run_conf->sta.iface, ESP_NETIF_DNS_BACKUP, &dns_b);
    esp_netif_get_dns_info(wm_run_conf->sta.iface, ESP_NETIF_DNS_FALLBACK, &dns_f);
    ESP_LOGI(tag, IPSTR " " IPSTR " " IPSTR, IP2STR(&dns_m.ip.u_addr.ip4), IP2STR(&dns_b.ip.u_addr.ip4), IP2STR(&dns_f.ip.u_addr.ip4));
}

/*
#include <stdio.h>
#include "idf_gpio_driver.h"
#include "idf_wifi_manager.h"
#include "esp_log.h"

#define UNUSED(x) (void)x;

void app_main(void)
{
    esp_event_loop_handle_t *uevent_loop = (esp_event_loop_handle_t *)malloc(sizeof(esp_event_loop_handle_t));
    esp_event_loop_args_t uevent_args = {
        .queue_size = 5,
        .task_name = "uevloop",
        .task_priority = 15,
        .task_stack_size = 3072,
        .task_core_id = tskNO_AFFINITY
    };
    esp_err_t err;
    err = esp_event_loop_create(&uevent_args, uevent_loop);
    (void)err;
    esp_event_handler_instance_register_with(*uevent_loop, WM_EVENT, ESP_EVENT_ANY_ID, wifimgr_event_handler, NULL, NULL);

    wm_init_wifi_manager(NULL, uevent_loop);
    wm_add_known_network("Apt.16 Guest", "1234567890");
}
*/