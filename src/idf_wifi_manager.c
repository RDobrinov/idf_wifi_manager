#include "idf_wifi_manager.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

typedef struct wm_airband_rank {
    uint8_t channel[13];
    int8_t rssi[13];
} wm_airband_rank_t;

typedef struct wm_wifi_iface {
    esp_netif_t *iface;
    wifi_config_t *driver_config;
} wm_wifi_iface_t;

typedef struct wm_wifi_base_config {
    char *ssid;
    char *password;
    wm_net_ip_config_t ip_config;
} wm_wifi_base_config_t;

typedef struct wm_ll_known_network_node {
    struct {
        wm_wifi_base_config_t net_config;
        uint32_t net_config_id;
    } payload;
    struct wm_ll_known_network_node *next;
} wm_ll_known_network_node_t;

typedef struct wm_wifi_mgr_config {
    wm_ll_known_network_node_t *known_networks_head;
    //wm_ll_known_network_node_t *known_networks_tail;
    wm_net_base_config_t ap_conf;
    wifi_country_t country;
    esp_ip4_addr_t sec_dns_server;
    wm_wifi_iface_t ap;
    wm_wifi_iface_t sta;
    TaskHandle_t apTask_handle;
    TaskHandle_t scanTask_handle;
    esp_event_loop_handle_t uevent_loop;
    union {
        struct {
            uint32_t sta_connected:1;
            uint32_t sta_connecting:1;
            uint32_t scanning:1;
            uint32_t known_net_count:5;
            uint32_t sta_connect_retry:3;
            uint32_t max_sta_connect_retry:3;
            uint32_t known_ssid:1;
            uint32_t ap_channel:4;
            uint32_t reserved_13:13;
        };
        uint32_t state;
    }; 
    wifi_ap_record_t found_known_ap;

    //wifi_event_sta_disconnected_t blacklistedAPs[WIFIMGR_MAX_KNOWN_AP]; //Not used yet
} wm_wifi_mgr_config_t;

static wm_wifi_mgr_config_t *wm_run_conf = NULL;

static void vScanTask(void *pvParameters);
static void vConnectTask(void *pvParameters);
static void wm_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void wm_ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void wm_apply_ap_driver_config();
static void wm_apply_netif_dns(esp_netif_t *iface, esp_ip4_addr_t *dns_server_ip, esp_netif_dns_type_t type );
static esp_err_t wm_event_post(int32_t event_id, const void *event_data, size_t event_data_size);

static esp_err_t wm_add_known_network_node( wm_wifi_base_config_t *known_network);
static wm_wifi_base_config_t *wm_create_known_network(char *ssid, char *pwd);

static wm_ll_known_network_node_t *wm_find_known_net_by_ssid( char *ssid );
static wm_ll_known_network_node_t *wm_find_known_net_by_id( uint32_t known_network_id );

/**
 * Change default ap channel
*/

#if (CONFIG_WIFIMGR_RUN_SNTP_WHEN_STA == 1)
static void wm_sntp_sync_cb(struct timeval *tv);
#endif

ESP_EVENT_DEFINE_BASE(WM_EVENT);

void test() {
    
}

static void vScanTask(void *pvParameters)
{
    wifi_mode_t wifi_run_mode = WIFI_MODE_MAX;
    TickType_t xDelayTicks = (2500 / portTICK_PERIOD_MS);
    while(true) {
        if(esp_wifi_get_mode(&wifi_run_mode) == ESP_OK) {
            //if((wifi_run_mode == WIFI_MODE_APSTA) || ((wifi_run_mode == WIFI_MODE_STA) && (!(xEventGroupGetBits(wm_run_conf->event.group) & WIFIMGR_CONNECTED_BIT)))) {    
            if((wifi_run_mode == WIFI_MODE_APSTA) || ((wifi_run_mode == WIFI_MODE_STA) && (wm_run_conf->sta_connected))) {    
                //if ( !(xEventGroupGetBits(wm_run_conf->event.group) & WIFIMGR_CONNECTING_BIT) && (xEventGroupGetBits(wm_run_conf->event.group) & WIFIMGR_SCAN_BIT) && (wm_run_conf->known_networks_head)) {
                if ( !(wm_run_conf->sta_connecting) && (wm_run_conf->scanning) && (wm_run_conf->known_networks_head)) {
                    //xEventGroupClearBits(wm_run_conf->event.group, WIFIMGR_SCAN_BIT);
                    wm_run_conf->scanning = 1;
                    esp_wifi_scan_start(NULL, false);
                    xDelayTicks = (2500 / portTICK_PERIOD_MS);
                }
            } else { xDelayTicks = (1000 / portTICK_PERIOD_MS); } //Timeout 1 second or maybe greater???
        } else { xDelayTicks = (500 / portTICK_PERIOD_MS); }
        vTaskDelay(xDelayTicks);
    }
}

static void vConnectTask(void *pvParameters) { // Celiqt shiban task e izlishen
    //esp_err_t err;
    while(1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        //if(!(xEventGroupGetBits(wm_config->event.group) & WIFIMGR_CONNECTING_BIT)) {
        if(!wm_run_conf->sta_connecting && strlen((char *)wm_run_conf->found_known_ap.ssid)) {
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
                }
            }
        }
            /*while(!(xEventGroupGetBits(wm_config->event.group) & WIFIMGR_CONNECTING_BIT) && ( i < WIFIMGR_MAX_KNOWN_AP)) { 
                if(strcmp((char *)wm_config->found_known_ap.ssid, wm_config->radio.known_networks[i].wifi_ssid) == 0) {
                    strcpy((char *)wm_config->sta.driver_config->sta.ssid, wm_config->radio.known_networks[i].wifi_ssid);
                    strcpy((char *)wm_config->sta.driver_config->sta.password, wm_config->radio.known_networks[i].wifi_password);
                    wm_config->sta.driver_config->sta.channel = wm_config->found_known_ap.primary;
                    if( ESP_OK = esp_wifi_set_config(WIFI_IF_STA, wm_config->sta.driver_config)) {
                        xEventGroupSetBits(wm_config->event.group, WIFIMGR_CONNECTING_BIT);
                        wm_config->event.sta_connect_retry = 0;
                        esp_wifi_connect();
                    }
                }
                i++;
            }*/
    }
}

static void wm_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {

    const char *ftag = "wifimgr:evt";

    if (event_base == WIFI_EVENT) {
        if(event_id == WIFI_EVENT_SCAN_DONE) {
            //ESP_LOGW(ftag, "WIFI_EVENT_SCAN_DONE");
            if(((wifi_event_sta_scan_done_t *)event_data)->status == 0) {
                uint16_t found_ap_count = 0;
                wm_airband_rank_t airband;
                esp_wifi_scan_get_ap_num(&found_ap_count);
                wifi_ap_record_t *found_ap_info = (wifi_ap_record_t *)calloc(found_ap_count, sizeof(wifi_ap_record_t));
                esp_wifi_scan_get_ap_records(&found_ap_count, found_ap_info);
                memset(&(wm_run_conf->found_known_ap), 0, sizeof(wifi_ap_record_t));
                memset(&airband.channel, 0, sizeof(airband.channel));
                memset(&airband.rssi, 0b10011111, sizeof(airband.rssi)); /* set min RSSI */

                wm_run_conf->known_ssid = 0;
                for(int i=0; ( i<found_ap_count ); i++) {
                    if(!wm_run_conf->known_ssid) {
                        /* If not blacklisted */
                        wm_ll_known_network_node_t *found_ssid = wm_find_known_net_by_ssid((char *)found_ap_info[i].ssid);
                        if(found_ssid) {
                            /* AP in list found in known networks */
                            wm_run_conf->found_known_ap = found_ap_info[i];
                            wm_run_conf->known_ssid = 1;
                        }
                    }
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
                }
                if(wm_run_conf->ap_channel == 0) {
                    int iRatedChannel = 0; //CONFIG_WIFIMGR_DEFAULT_AP_CHANNEL; //?????????
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
                    /* da se proweri dali ne promenq standartnata konfiguraciq */
                    if( wm_run_conf->ap.driver_config->ap.channel != iRatedChannel ) {
                        wm_run_conf->ap.driver_config->ap.channel = iRatedChannel;
                    };
                }
                free(found_ap_info);
                wm_run_conf->scanning = 1;
                if(!(wm_run_conf->sta_connected) && strlen((char *)wm_run_conf->found_known_ap.ssid) > 0 ) {
                    xTaskNotifyGive(wm_run_conf->apTask_handle);
                } else {
                    wifi_mode_t *wifi_run_mode = (wifi_mode_t *)calloc(1, sizeof(wifi_mode_t));
                    esp_wifi_get_mode(wifi_run_mode);
                    if(*wifi_run_mode != WIFI_MODE_APSTA) {
                        if(esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
                            ESP_LOGE(ftag, "APSTA failed");   //Notification
                        } else { 
                            esp_wifi_set_channel(wm_run_conf->ap.driver_config->ap.channel, WIFI_SECOND_CHAN_NONE);
                            wm_event_post(WM_EVENT_AP_START, NULL, 0);
                        }
                    }
                    free(wifi_run_mode);
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
            /*if(wm_config->event.uevent_loop) {

                if(ESP_OK != esp_event_post_to(wm_config->event.uevent_loop, WM_EVENT, WM_EVENT_STA_CONNECT, &wm_config->found_known_ap, sizeof(wifi_ap_record_t), 0)) {
                    ESP_LOGI(ftag,"post event error WM_EVENT_STA_CONNECT");
                }
            }
            */
            wm_set_interface_ip(WIFI_IF_STA, &((wm_find_known_net_by_ssid((char *)wm_run_conf->found_known_ap.ssid))->payload.net_config.ip_config));
            /*int i=0;
            while( (strlen((char *)(wm_config->radio.known_networks[i].wifi_ssid)) > 0) && (i < WIFIMGR_MAX_KNOWN_AP) ) {
                if(strcmp((char *)wm_config->radio.known_networks[i].wifi_ssid, (char *)((wifi_event_sta_connected_t *)(event_data))->ssid) == 0) {
                    wm_set_interface_ip(WIFI_IF_STA, &wm_config->radio.known_networks[i]);
                    i = WIFIMGR_MAX_KNOWN_AP;
                } else { i++; }
            }*/
            #if (CONFIG_WIFIMGR_RUN_SNTP_WHEN_STA == 1)
            esp_sntp_config_t *sntp_config = (esp_sntp_config_t *)malloc(sizeof(esp_sntp_config_t));
            //*sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com", "time.cloudflare.com"));
            //*sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            *sntp_config = (esp_sntp_config_t) { 
                .smooth_sync = 0, 
                .server_from_dhcp = 0, 
                .wait_for_sync = 1, 
                .start = 1, 
                //.sync_cb = ((void *)0), 
                .sync_cb = wm_sntp_sync_cb, 
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
            if (wm_run_conf->sta_connect_retry < wm_run_conf->max_sta_connect_retry) {
                esp_wifi_connect();
                wm_run_conf->sta_connect_retry++;
            } else {
                wm_run_conf->state &= 0xFFFFFFFCUL; /* Clear connecting and connected bits */
                wm_event_post(WM_EVENT_STA_DISCONNECT, NULL, 0);
                #if (CONFIG_WIFIMGR_RUN_SNTP_WHEN_STA == 1)
                /* Destroy sntp */
                esp_netif_sntp_deinit();
                #endif
            }
        }
    }
}

static void wm_ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static const char *ftag = "wifimgr:ipevt";
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wm_run_conf->sta_connect_retry = 0;
        wm_event_post(WM_EVENT_NETIF_GOT_IP, (void *)&(((ip_event_got_ip_t *)event_data)->ip_info), sizeof(esp_netif_ip_info_t));
        wm_run_conf->sta_connected = 1;
        wm_run_conf->sta_connecting = 0;
        if(esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
            /* Event notification instead log message */
            /* It's a warning state - Station connected, but AP is still running */
            ESP_LOGE(ftag, "ip_event_handler STA failed");   
        } else { wm_event_post(WM_EVENT_AP_STOP, NULL, 0); }
    }
}

#if (CONFIG_WIFIMGR_RUN_SNTP_WHEN_STA == 1)
static void wm_sntp_sync_cb(struct timeval *tv) {
    wm_event_post(WM_EVENT_NETIF_GOT_TIME, tv, sizeof(struct timeval));
}
#endif

static void wm_apply_ap_driver_config() {
    strcpy((char *)wm_run_conf->ap.driver_config->ap.ssid, wm_run_conf->ap_conf.ssid);
    wm_run_conf->ap.driver_config->ap.channel = (wm_run_conf->ap_channel != 0) ? wm_run_conf->ap_channel : CONFIG_WIFIMGR_DEFAULT_AP_CHANNEL;
    wm_run_conf->ap.driver_config->ap.max_connection = 1;
    wm_run_conf->ap.driver_config->ap.authmode = 
        (strlen(strcpy((char *)wm_run_conf->ap.driver_config->ap.password, wm_run_conf->ap_conf.password)) != 0) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wm_run_conf->ap.driver_config->ap.pairwise_cipher = WIFI_CIPHER_TYPE_TKIP; //Kconfig param...
    wm_run_conf->ap.driver_config->ap.pmf_cfg = (wifi_pmf_config_t) { .required = true };
    //FTM ???
}

static void wm_apply_netif_dns(esp_netif_t *iface, esp_ip4_addr_t *dns_server_ip, esp_netif_dns_type_t type ) {
    esp_netif_dns_info_t *dns = (esp_netif_dns_info_t *)calloc(1, sizeof(esp_netif_dns_info_t));
    *dns = (esp_netif_dns_info_t) { .ip = (esp_ip_addr_t){.u_addr.ip4 = *dns_server_ip, .type = ESP_IPADDR_TYPE_V4 }};
    esp_err_t err = esp_netif_set_dns_info(iface, type, dns);
    free(dns);
    if(ESP_OK != err) {ESP_LOGE("wm:dns", "wm_apply_netif_dns (%s)", esp_err_to_name(err));}
    return;
}
/**
 * Return pointer to array with current known networks.
*/
wm_known_net_config_t *wm_get_known_networks(size_t *size) {
    wm_ll_known_network_node_t *work = wm_run_conf->known_networks_head;
    wm_known_net_config_t *known_net = NULL;
    if(work) {
        *size = 0;
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
    memcpy(ap_conf, (char *)&wm_run_conf->ap_conf, sizeof(wm_net_base_config_t));    
}

static esp_err_t wm_event_post(int32_t event_id, const void *event_data, size_t event_data_size) {
    return (wm_run_conf->uevent_loop) ? esp_event_post_to(wm_run_conf->uevent_loop, WM_EVENT, event_id, event_data, event_data_size, 1) 
                                          : esp_event_post(WM_EVENT, event_id, event_data, event_data_size, 1);
}

static void wm_clear_pointers(void) {
    if(wm_run_conf->ap.driver_config) free(wm_run_conf->ap.driver_config);
    if(wm_run_conf->sta.driver_config) free(wm_run_conf->sta.driver_config);
    free(wm_run_conf);
}

esp_err_t wm_set_interface_ip( wifi_interface_t iface, wm_net_ip_config_t *ip_info)
{
    bool _ok = true;
    esp_netif_dhcp_status_t dhcp_status;
    esp_err_t err;

    if(iface != WIFI_IF_STA && iface != WIFI_IF_AP) return ESP_FAIL;
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
            //ESP_LOGE("ap"," " IPSTR "/" IPSTR "/" IPSTR "/" IPSTR, IP2STR(&ip_info->static_ip.ip), IP2STR(&ip_info->static_ip.netmask), IP2STR(&ip_info->static_ip.gw), IP2STR(&ip_info->pri_dns_server));
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
    return _ok ? ESP_OK : ESP_FAIL;
}

void wm_create_apmode_config( wm_apmode_config_t *full_ap_cfg) {
    if(!full_ap_cfg) return;
    *full_ap_cfg = (wm_apmode_config_t) {
        .base_conf = (wm_net_base_config_t) {
            .ssid = CONFIG_WIFIMGR_AP_SSID,
            .password = CONFIG_WIFIMGR_AP_PWD,
            .ip_config.static_ip = {{ IPADDR_ANY }, { IPADDR_ANY }, { IPADDR_ANY },},
            .ip_config.pri_dns_server = { IPADDR_ANY }
        },
        .country = (wifi_country_t ){ 
            .cc = CONFIG_WIFIMGR_COUNTRY_CODE,
            .schan = 1,
            .nchan = ((0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "US")) || (0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "01")) )? 11 : 13,
            .policy = WIFI_COUNTRY_POLICY_AUTO
        },
        .ap_channel = (((0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "US")) || (0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "01"))) && 
                            (CONFIG_WIFIMGR_DEFAULT_AP_CHANNEL >11 )) ? 11 : CONFIG_WIFIMGR_DEFAULT_AP_CHANNEL
    };
}

void wm_change_ap_mode_config( wm_net_base_config_t *ap_conf ) {
    memcpy(&wm_run_conf->ap_conf, ap_conf, sizeof(wm_net_base_config_t));      //witch is faster?? ap_conf = *ap_conf
    if(ap_conf->ip_config.static_ip.ip.addr != IPADDR_ANY) wm_set_interface_ip(WIFI_IF_AP, &ap_conf->ip_config);
    else wm_set_interface_ip(WIFI_IF_AP, NULL);
    wm_apply_ap_driver_config();
    esp_wifi_set_config(WIFI_IF_AP, wm_run_conf->ap.driver_config);
}

/* ??? Why? dns in ap mode ???*/
void wm_set_ap_primary_dns(esp_ip4_addr_t dns_ip) {
    wm_run_conf->ap_conf.ip_config.pri_dns_server = dns_ip;
}

void wm_set_sta_dns_by_id(esp_ip4_addr_t dns_ip, uint32_t known_network_id) {
    if(known_network_id) {
        wm_ll_known_network_node_t *work = wm_find_known_net_by_id(known_network_id);
        if(work) work->payload.net_config.ip_config.pri_dns_server = dns_ip;
    }
}

void wm_set_sta_dns_by_ssid(esp_ip4_addr_t dns_ip, char *ssid) {
    if(ssid) {
        wm_ll_known_network_node_t *work = wm_find_known_net_by_ssid(ssid);
        if(work) work->payload.net_config.ip_config.pri_dns_server = dns_ip;
    }
}

void wm_set_secondary_dns(esp_ip4_addr_t dns_ip) {
    wm_run_conf->sec_dns_server = dns_ip;
}

//esp_err_t wm_init_wifi_manager(wm_wifi_connection_data_t *pInitConfig) {
esp_err_t wm_init_wifi_manager( wm_apmode_config_t *full_ap_cfg, esp_event_loop_handle_t *p_uevent_loop) {

    static const char *ftag = "wifimgr:init";

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
            wm_run_conf->ap_channel = (((0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "US")) || (0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "01"))) && 
                                (CONFIG_WIFIMGR_DEFAULT_AP_CHANNEL >11 )) ? 11 : CONFIG_WIFIMGR_DEFAULT_AP_CHANNEL;

            wm_run_conf->max_sta_connect_retry = CONFIG_WIFIMGR_MAX_STA_RETRY;
            wm_run_conf->country = (wifi_country_t ){  //ne se nalaga wyrhu drajwera.
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

        /* Apply static IP to AP if any */
        if( wm_run_conf->ap_conf.ip_config.static_ip.ip.addr != IPADDR_ANY ) {
            if( wm_set_interface_ip(WIFI_IF_AP, &wm_run_conf->ap_conf.ip_config) != ESP_OK ) {
                ESP_LOGE(ftag, "IP Address not changed"); //EVENT
            }
        }

        /* Init default WIFI configuration*/
        wifi_init_config_t *_initconf = (wifi_init_config_t *)calloc(1, sizeof(wifi_init_config_t));
        *_initconf = (wifi_init_config_t)WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(_initconf);
        if( ESP_OK != err) {
            //ESP_LOGE(ftag, "esp_wifi_init (%s)", esp_err_to_name(err)); //Event
            wm_clear_pointers();
            return err;
        }
        free(_initconf);

        /* Storage */
        if( esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ) {
            //ESP_LOGE(ftag, "esp_wifi_set_storage");
            wm_clear_pointers();
            return ESP_FAIL;
        }

        /* Apply country data */
        if( esp_wifi_set_country(&(wm_run_conf->country))) {
            //ESP_LOGE(ftag, "esp_wifi_set_country");
            wm_clear_pointers();
            return ESP_FAIL;  
        }

        /* Event handlers registation */
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wm_wifi_event_handler, NULL, NULL);
        if( ESP_OK != err) {
            //ESP_LOGE(ftag, "wifi_event_handler (%s)", esp_err_to_name(err));
            wm_clear_pointers();
            return err;
        }
        //err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wm_ip_event_handler, NULL, NULL);
        err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wm_ip_event_handler, NULL, NULL);
        if( ESP_OK != err) {
            //ESP_LOGE(ftag, "ip_event_handler (%s)", esp_err_to_name(err));
            wm_clear_pointers();
            return err;
        }

        /* Setup AP mode */
        wm_apply_ap_driver_config();

        /* Set initial WiFi mode */
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if(err != ESP_OK) {
            wm_clear_pointers();
            //ESP_LOGE(ftag, "esp_wifi_set_mode (%s)", esp_err_to_name(err));
            return err;    
        };

        /* Apply initial AP configuration */
        err = esp_wifi_set_config(WIFI_IF_AP, wm_run_conf->ap.driver_config);
        if(err != ESP_OK) {
            wm_clear_pointers();
            //ESP_LOGE(ftag, "esp_wifi_set_config(WIFI_IF_AP) (%s)", esp_err_to_name(err));
            return err;
        };

        /* Apply empty STA configuration */
        err = esp_wifi_set_config(WIFI_IF_STA, wm_run_conf->sta.driver_config);
        if(err != ESP_OK) {
            wm_clear_pointers();
            //ESP_LOGE(ftag, "esp_wifi_set_config(WIFI_IF_STA) (%s)", esp_err_to_name(err));
            return err;
        };

        /* Set bandwidth to HT20 */
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
        err = esp_wifi_start();
        if( err != ESP_OK ) {
            wm_clear_pointers();
            //ESP_LOGE(ftag, "esp_wifi_start (%s)", esp_err_to_name(err));
            return err;
        } else { 
            wm_event_post(WM_EVENT_AP_START, NULL, 0);
            esp_netif_ip_info_t *event_data = (esp_netif_ip_info_t *)malloc(sizeof(wifi_init_config_t));
            esp_netif_get_ip_info( wm_run_conf->ap.iface, event_data);
            wm_event_post(WM_EVENT_NETIF_GOT_IP, event_data, sizeof(esp_netif_ip_info_t));
            free(event_data);
        };

        wm_run_conf->scanning = 1;
        xTaskCreate(vConnectTask, "wconn", 2048, NULL, 16, &wm_run_conf->apTask_handle);
        xTaskCreate(vScanTask, "wscan", 2048, NULL, 15, &wm_run_conf->scanTask_handle); // Still not needed
    } else return ESP_ERR_NO_MEM;

    return ESP_OK;
    //if( wm_config->event.uevent_loop) {
    //
    //}  

    
    /* TODO: Remove in release
    ESP_LOGI(ftag,"iface %s / %s created", wm_config->ap.iface->if_desc, wm_config->ap.iface->if_key);
    ESP_LOGI(ftag,"iface %s / %s created", wm_config->ap.iface->if_desc, wm_config->ap.iface->if_key);
    */
}

static esp_err_t wm_add_known_network_node( wm_wifi_base_config_t *known_network) {
    if(wm_run_conf->known_net_count >= CONFIG_WIFIMGR_MAX_KNOWN_NETWORKS) return ESP_ERR_NOT_ALLOWED; //event for fail
    wm_ll_known_network_node_t *work = calloc(1, sizeof(wm_ll_known_network_node_t));
    if(work) {
        work->payload.net_config = *known_network;
        work->payload.net_config_id = esp_rom_crc32_le(0, (const unsigned char *)known_network->ssid, strlen(known_network->ssid));
        work->payload.net_config_id = esp_rom_crc32_le(work->payload.net_config_id, (const unsigned char *)known_network->password, strlen(known_network->password));
        work->next = wm_run_conf->known_networks_head;
        wm_run_conf->known_networks_head = work;
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
    //post event with ID or add failed
    //return ( i == WIFIMGR_MAX_KNOWN_AP ) ? ESP_FAIL : ESP_OK;
}
// ******* Password check *******
static esp_err_t wm_check_ssid_pwd(char *ssid, char *pwd) {
    size_t pwd_length = strlen(pwd);
    return ((strlen(ssid) < 2) || ( pwd_length>0 && pwd_length<8));
}

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

esp_err_t wm_add_known_network_config( wm_net_base_config_t *known_network) {
    if(ESP_OK != wm_check_ssid_pwd(known_network->ssid, known_network->password)) return ESP_ERR_INVALID_ARG;
    wm_wifi_base_config_t *new_network = wm_create_known_network(known_network->ssid, known_network->password);
    if(!new_network) return ESP_FAIL;
    new_network->ip_config = known_network->ip_config;
    esp_err_t err = wm_add_known_network_node(new_network);
    free(new_network);
    return err;
}

esp_err_t wm_add_known_network( char *ssid, char *pwd) {
    if(ESP_OK != wm_check_ssid_pwd(ssid, pwd)) return ESP_ERR_INVALID_ARG;
    wm_wifi_base_config_t *new_network = wm_create_known_network(ssid, pwd);
    if(!new_network) return ESP_FAIL;
    esp_err_t err = wm_add_known_network_node(new_network);
    free(new_network);
    return err;
}

esp_err_t wm_del_known_net_by_id( uint32_t known_network_id ) {
    if(!known_network_id) return ESP_ERR_INVALID_ARG; // Event communication
    wm_ll_known_network_node_t *work = wm_run_conf->known_networks_head;
    wm_ll_known_network_node_t *prev = NULL;
    bool for_delete = false;            //Not good idea

    while( !for_delete && work) {
        for_delete = (known_network_id == work->payload.net_config_id);
        if(for_delete) {
            if(!prev) wm_run_conf->known_networks_head = work->next; //head node going to be deleted
            else prev->next = work->next;
            free(work->payload.net_config.ssid);       // free both ssid/pass
            free(work->payload.net_config.password);
            free(work); //release node
        } else {
            prev = work;
            work = work->next;
        }
    }
    return (for_delete) ? ESP_OK : ESP_ERR_NOT_FOUND; // Event communication
}

esp_err_t wm_del_known_net_by_ssid( char *ssid ) {
    return wm_del_known_net_by_id(wm_get_config_id(ssid));
}

esp_err_t wm_set_country(char *cc) {
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
    return (esp_wifi_set_country(&(wm_run_conf->country)) == ESP_OK ) ? ESP_OK : ESP_FAIL;
}

uint32_t wm_get_config_id(char *ssid) {
    wm_ll_known_network_node_t *work = wm_find_known_net_by_ssid(ssid);
    return (work) ? work->payload.net_config_id : 0;
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