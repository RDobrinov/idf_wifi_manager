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

//void wm_init_wifi_connection_data( wm_wifi_connection_data_t *pWifiConn );
void wm_create_apmode_config( wm_apmode_config_t *full_ap_cfg); //OK
void wm_change_ap_mode_config( wm_net_base_config_t *ap_conf );
void wm_set_ap_primary_dns(esp_ip4_addr_t dns_ip);  //OK
void wm_set_sta_dns_by_id(esp_ip4_addr_t dns_ip, uint32_t known_network_id);
void wm_set_sta_dns_by_ssid(esp_ip4_addr_t dns_ip, char *ssid);
void wm_set_secondary_dns(esp_ip4_addr_t dns_ip);
wm_known_net_config_t *wm_get_known_networks(size_t *size);                     //OK
void wm_get_ap_config(wm_net_base_config_t *ap_conf);   //OK

esp_err_t wm_set_interface_ip( wifi_interface_t iface, wm_net_ip_config_t *ip_info);   // *** Move to static 
esp_err_t wm_init_wifi_manager( wm_apmode_config_t *full_ap_cfg, esp_event_loop_handle_t *p_uevent_loop);       //OK
esp_err_t wm_add_known_network_config( wm_net_base_config_t *known_network);    //OK
esp_err_t wm_add_known_network( char *ssid, char *pwd );    //OK
esp_err_t wm_del_known_net_by_id( uint32_t known_network_id ); //OK
esp_err_t wm_del_known_net_by_ssid( char *ssid );   //OK
esp_err_t wm_set_country(char *cc); //or char (*cc)[3]

uint8_t wm_netmask_to_cidr(uint32_t nm);    //OK


uint32_t wm_get_config_id(char *ssid);      //OK

/**
 * For test Only
*/
void wifimgr_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

void wifimgr_dump_ifaces();

#endif /* _WIFI_MANAGER_H_ */