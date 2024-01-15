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

#define WIFIMGR_MAX_KNOWN_AP 5

typedef enum {
    WM_EVENT_AP_START,
    WM_EVENT_AP_STOP,
    WM_EVENT_STA_CONNECT,
    WM_EVENT_STA_DISCONNECT,
    WM_EVENT_NETIF_GOT_IP,
    WM_EVENT_NETIF_GOT_TIME
} wm_event_t;

ESP_EVENT_DECLARE_BASE(WM_EVENT);

typedef struct wm_wifi_base_config {
    char *ssid;
    char *password;
    esp_netif_ip_info_t static_ip;
    esp_ip4_addr_t pri_dns_server;
} wm_wifi_base_config_t;

//void wm_init_wifi_connection_data( wm_wifi_connection_data_t *pWifiConn );
void wm_init_base_config( wm_wifi_base_config_t *base_conf );
void wm_change_ap_mode_config( wm_wifi_base_config_t *pWifiConn );
void wm_set_ap_primary_dns(esp_ip4_addr_t dns_ip);
void wm_set_sta_primary_dns(esp_ip4_addr_t dns_ip, char *ssid);
void wm_set_secondary_dns(esp_ip4_addr_t dns_ip);
void wm_get_known_networks(wm_wifi_base_config_t *net_list);
void wm_get_ap_config(wm_wifi_base_config_t *net_list);

esp_err_t wm_set_interface_ip( wifi_interface_t iface, wm_wifi_base_config_t *ip_info);
//esp_err_t wm_init_wifi_manager( wm_wifi_connection_data_t *pInitConfig, esp_event_loop_handle_t *p_uevent_loop);
esp_err_t wm_add_known_network_config( wm_wifi_base_config_t *known_network );
esp_err_t wm_add_known_network( char *ssid, char *pwd );
esp_err_t wm_delete_known_network( char *ssid );
esp_err_t wm_set_country(char *cc); //or char (*cc)[3]



#endif /* _WIFI_MANAGER_H_ */