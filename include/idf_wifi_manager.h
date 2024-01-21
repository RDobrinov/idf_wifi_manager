#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "esp_netif_types.h"
#include "../lwip/esp_netif_lwip_internal.h"

/* WiFi */
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
/**/

#define MACSTR "%X:%X:%X:%X:%X:%X"
#define MAC2STR(macaddr)    ((uint8_t)macaddr[0]), ((uint8_t)macaddr[1]), ((uint8_t)macaddr[2]), \
                            ((uint8_t)macaddr[3]), ((uint8_t)macaddr[4]), ((uint8_t)macaddr[5]) 

typedef enum {
    WM_EVENT_AP_START,
    WM_EVENT_AP_STOP,
    WM_EVENT_STA_CONNECT,
    WM_EVENT_STA_DISCONNECT,
    WM_EVENT_NETIF_GOT_IP,
    WM_EVENT_NETIF_GOT_TIME
} wm_event_t;

ESP_EVENT_DECLARE_BASE(WM_EVENT);

typedef struct wm_net_ip_config {
    esp_netif_ip_info_t static_ip;
    esp_ip4_addr_t pri_dns_server;
} wm_net_ip_config_t;

typedef struct wm_net_base_config {
    char ssid[33];
    char password[64];
    wm_net_ip_config_t ip_config;
} wm_net_base_config_t;

typedef struct wm_apmode_config {
    wm_net_base_config_t base_conf;
    wifi_country_t country;
    uint32_t ap_channel;
} wm_apmode_config_t;

typedef struct wm_known_net_config {
    wm_net_base_config_t net_config;
    uint32_t net_config_id;
} wm_known_net_config_t;

//void wm_init_wifi_connection_data( wm_wifi_connection_data_t *pWifiConn );
void wm_create_apmode_config( wm_apmode_config_t *full_ap_cfg);
void wm_change_ap_mode_config( wm_net_base_config_t *ap_conf );
void wm_set_ap_primary_dns(esp_ip4_addr_t dns_ip);
void wm_set_sta_dns_by_id(esp_ip4_addr_t dns_ip, uint32_t known_network_id);
void wm_set_sta_dns_by_ssid(esp_ip4_addr_t dns_ip, char *ssid);
void wm_set_secondary_dns(esp_ip4_addr_t dns_ip);
wm_known_net_config_t *wm_get_known_networks(size_t *size);
void wm_get_ap_config(wm_net_base_config_t *ap_conf);

esp_err_t wm_set_interface_ip( wifi_interface_t iface, wm_net_ip_config_t *ip_info);   // *** Move to static 
esp_err_t wm_init_wifi_manager( wm_apmode_config_t *full_ap_cfg, esp_event_loop_handle_t *p_uevent_loop);
esp_err_t wm_add_known_network_config( wm_net_base_config_t *known_network);
esp_err_t wm_add_known_network( char *ssid, char *pwd );
esp_err_t wm_del_known_net_by_id( uint32_t known_network_id );
esp_err_t wm_del_known_net_by_ssid( char *ssid );
esp_err_t wm_set_country(char *cc); //or char (*cc)[3]

uint8_t wm_netmask_to_cidr(uint32_t nm);


uint32_t wm_get_config_id(char *ssid);

/**
 * For test Only
*/
void wifimgr_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);


#endif /* _WIFI_MANAGER_H_ */