#include "idf_wifi_manager.h"
#include "esp_wifi.h"       //remove
#include "esp_netif_sntp.h"
#include "esp_log.h"

#define WIFIMGR_CONNECTED_BIT  BIT0
#define WIFIMGR_CONNECTING_BIT BIT1
#define WIFIMGR_SCAN_BIT       BIT2

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
            uint32_t sta_connect_retry:4;
            uint32_t ap_channel:4;
            uint32_t reserved_16:16;
        };
        uint32_t state;
    }; 
    uint8_t sta_connect_retry;
    wifi_ap_record_t found_known_ap;

    //wifi_event_sta_disconnected_t blacklistedAPs[WIFIMGR_MAX_KNOWN_AP]; //Not used yet
} wm_wifi_mgr_config_t;

static wm_wifi_mgr_config_t *wm_run_conf;

static void vScanTask(void *pvParameters);
static void vConnectTask(void *pvParameters);
static void wm_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void wm_ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void wm_apply_ap_driver_config();
static void wm_apply_netif_dns(esp_netif_t *iface, esp_ip4_addr_t *dns_server_ip, esp_netif_dns_type_t type );
static esp_err_t wm_event_post(int32_t event_id, const void *event_data, size_t event_data_size);

static wm_ll_known_network_node_t *wm_find_known_net_by_ssid( char *ssid );
static wm_ll_known_network_node_t *wm_find_known_net_by_id( uint32_t known_network_id );

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
        if(!wm_run_conf->sta_connecting && strlen(wm_run_conf->found_known_ap.ssid)) {
            /* Take care. sta.driver_config must be filled in event handler */
            if( ESP_OK == esp_wifi_set_config(WIFI_IF_STA, wm_run_conf->sta.driver_config)) {
                //xEventGroupSetBits(wm_config->event.group, WIFIMGR_CONNECTING_BIT);
                wm_run_conf->sta_connecting = 1;
                wm_run_conf->sta_connect_retry = 0;
                esp_wifi_connect();
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

                bool bAPNotFound = true;
                for(int i=0; ( i<found_ap_count ); i++) {
                    if(bAPNotFound) {
                        /* If not blacklisted */
                        wm_ll_known_network_node_t *found_ssid = wm_find_known_net_by_ssid((char *)found_ap_info[i].ssid);
                        if(found_ssid) {
                            /* AP in list found in known networks */
                        }
                        /*int iKnownIndex = 0;
                        while( iKnownIndex < WIFIMGR_MAX_KNOWN_AP) {
                            if(strlen(wm_config->radio.known_networks[iKnownIndex].wifi_ssid) > 0) {
                                if(strcmp(wm_config->radio.known_networks[iKnownIndex].wifi_ssid, (char *)found_ap_info[i].ssid) == 0) {
                                    int iBLIndex = 0;
                                    bool bAPNotBL = true;
                                    while( (iBLIndex < WIFIMGR_MAX_KNOWN_AP) && bAPNotBL ) {
                                        if( (strcmp((char *)found_ap_info[i].ssid, (char *)wm_config->blacklistedAPs[iBLIndex].ssid) == 0) && 
                                                (memcmp((char *)found_ap_info[i].bssid, (char *)wm_config->blacklistedAPs[iBLIndex].bssid, 6 * sizeof(uint8_t)) == 0) ) {
                                            bAPNotBL = false;
                                        }
                                        iBLIndex++;
                                    }
                                    if( bAPNotBL ) { 
                                        wm_config->found_known_ap = found_ap_info[i];
                                        bAPNotFound = false;
                                        iKnownIndex = WIFIMGR_MAX_KNOWN_AP;
                                    }
                                }
                                iKnownIndex++;
                            } else { iKnownIndex = WIFIMGR_MAX_KNOWN_AP; }
                        }*/
                    }
                    if(wm_config->radio.wifi_ap_channel == 0) {
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
                if(wm_config->radio.wifi_ap_channel == 0) {
                    int iRatedChannel = CONFIG_WIFIMGR_DEFAULT_AP_CHANNEL;
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
                    if( wm_config->ap.driver_config->ap.channel != iRatedChannel ) {
                        wm_config->ap.driver_config->ap.channel = iRatedChannel;
                    };
                }
                free(found_ap_info);
                xEventGroupSetBits(wm_config->event.group, WIFIMGR_SCAN_BIT);
                if(!(xEventGroupGetBits(wm_config->event.group) & WIFIMGR_CONNECTED_BIT) && strlen((char *)wm_config->found_known_ap.ssid) > 0 ) {
                    xTaskNotifyGive(wm_config->event.apTask_handle);
                } else {
                    wifi_mode_t *wifi_run_mode = (wifi_mode_t *)calloc(1, sizeof(wifi_mode_t));
                    esp_wifi_get_mode(wifi_run_mode);
                    if(*wifi_run_mode != WIFI_MODE_APSTA) {
                        if(esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
                            ESP_LOGE(ftag, "APSTA failed");   
                        } else { 
                            esp_wifi_set_channel(wm_config->ap.driver_config->ap.channel, WIFI_SECOND_CHAN_NONE);
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
            wm_set_interface_ip(WIFI_IF_STA, &((wm_find_known_net_by_ssid(wm_run_conf->found_known_ap.ssid))->payload.net_config.ip_config));
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
            if (wm_run_conf->sta_connect_retry < CONFIG_WIFIMGR_MAX_STA_RETRY) {
                esp_wifi_connect();
                wm_run_conf->sta_connect_retry;
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
    wm_known_net_config_t *known_net;
    if(work) {
        *size = 0;
        known_net = (wm_known_net_config_t *)calloc(wm_run_conf->known_net_count, sizeof(wm_known_net_config_t));
        while(work && (*size < wm_run_conf->known_net_count)) {
            known_net[*size].net_config.ip_config = work->payload.net_config.ip_config;
            known_net[*size].net_config_id = work->payload.net_config_id;
            strcpy(known_net[*size].net_config.ssid, work->payload.net_config.ssid);
            strcpy(known_net[*size].net_config.password, work->payload.net_config.password);
            *size++;
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

void wm_init_wifi_connection_data( wm_wifi_connection_data_t *pWifiConn) { //going to init
    *pWifiConn = (wm_wifi_connection_data_t) {
        .ap_mode = (wm_wifi_base_config_t) {
            .wifi_ssid = CONFIG_WIFIMGR_AP_SSID,
            .wifi_password = CONFIG_WIFIMGR_AP_PWD,
            .static_ip.ip = { IPADDR_ANY },
            .static_ip.netmask = { IPADDR_ANY }, 
            .static_ip.gw = { IPADDR_ANY },
            .pri_dns_server = { IPADDR_ANY }
        },
        .wifi_ap_channel = (((0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "US")) || (0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "01"))) && 
                            (CONFIG_WIFIMGR_DEFAULT_AP_CHANNEL >11 )) ? 11 : CONFIG_WIFIMGR_DEFAULT_AP_CHANNEL,
        .wifi_max_sta_retry = CONFIG_WIFIMGR_MAX_STA_RETRY,
        .country = (wifi_country_t ){ 
            .cc = CONFIG_WIFIMGR_COUNTRY_CODE,
            .schan = 1,
            .nchan = ((0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "US")) || (0 == strcmp(CONFIG_WIFIMGR_COUNTRY_CODE, "01")) )? 11 : 13,
            .policy=WIFI_COUNTRY_POLICY_AUTO
        }
    };

    for( uint8_t i=0; i<WIFIMGR_MAX_KNOWN_AP; i++) { 
        pWifiConn->known_networks[i] = (wm_wifi_base_config_t){
            .wifi_ssid = "", .wifi_password = "",
            .static_ip.ip = { IPADDR_ANY },
            .static_ip.netmask = { IPADDR_ANY }, 
            .static_ip.gw = { IPADDR_ANY },
            .pri_dns_server = { IPADDR_ANY }
        };
    }
    return;
}

void wm_init_base_config( wm_wifi_base_config_t *base_conf) {
    *base_conf = (wm_wifi_base_config_t) {
        .ssid = NULL,
        .password = NULL,
        .static_ip.ip = { IPADDR_ANY },
        .static_ip.netmask = { IPADDR_ANY }, 
        .static_ip.gw = { IPADDR_ANY },
        .pri_dns_server = { IPADDR_ANY }
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
esp_err_t wm_init_wifi_manager( wm_wifi_connection_data_t *pInitConfig, esp_event_loop_handle_t *p_uevent_loop) {

    static const char *ftag = "wifimgr:init";

    static bool wifi_init_done = false;
    if(wifi_init_done) { return ESP_OK; }

    wm_config = (wm_wifi_mgr_config_t *)calloc(1, sizeof(wm_wifi_mgr_config_t));
    if(pInitConfig == NULL) { wm_init_wifi_connection_data(&wm_config->radio); } 
    else {memcpy(&wm_config->radio, pInitConfig, sizeof(wm_wifi_connection_data_t));}
    /* Disable wifi log info*/
    esp_log_level_set("wifi", ESP_LOG_WARN);
    /* Init NVS*/
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if( err != ESP_OK ) { 
            ESP_LOGE(ftag, "nvs_flash_erase (%s)", esp_err_to_name(err));
            return err; 
        }
        err = nvs_flash_init();
    }
    if( err != ESP_OK ) { 
        ESP_LOGE(ftag, "nvs_flash_init (%s)", esp_err_to_name(err));
        return err; 
    }

    /* Init netif */
    if( esp_netif_init() != ESP_OK ) {
        ESP_LOGE(ftag, "esp_netif_init failed");
        return ESP_FAIL;
    };

    //if(p_uevent_loop != NULL) {wm_config->event.uevent_loop = *p_uevent_loop;}
    wm_config->event.uevent_loop = (p_uevent_loop) ? *p_uevent_loop : NULL;

    /* Create default event loop */
    err = esp_event_loop_create_default();
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        ESP_LOGE(ftag, "esp_event_loop_create_default (%s)", esp_err_to_name(err));
        return err; 
    }
    wm_config->event.group = xEventGroupCreate();
    xEventGroupClearBits(wm_config->event.group, WIFIMGR_CONNECTING_BIT | WIFIMGR_CONNECTED_BIT);

    /* Init wifi interfaces */
    wm_config->ap.iface = esp_netif_create_default_wifi_ap();
	wm_config->sta.iface = esp_netif_create_default_wifi_sta();

    //ESP_LOGW("ap"," " IPSTR "/" IPSTR "/" IPSTR, IP2STR(&ap_ip.ip), IP2STR(&ap_ip.netmask), IP2STR(&ap_ip.gw));

    /* Setup initial driver configuration */
    wm_config->ap.driver_config = (wifi_config_t *)calloc(1, sizeof(wifi_config_t));
    wm_config->sta.driver_config = (wifi_config_t *)calloc(1, sizeof(wifi_config_t));
    if( wm_config->radio.ap_mode.static_ip.ip.addr != IPADDR_ANY ) {
        //if( wm_set_interface_ip(WIFI_IF_AP, &wm_config->radio.ap_mode.static_ip) != ESP_OK ) {
        if( wm_set_interface_ip(WIFI_IF_AP, &wm_config->radio.ap_mode) != ESP_OK ) {
            ESP_LOGE(ftag, "IP Address not changed");
        }
    }
    //if( wm_config->event.uevent_loop) {
    esp_netif_ip_info_t *event_data = (esp_netif_ip_info_t *)malloc(sizeof(wifi_init_config_t));
    esp_netif_get_ip_info( wm_config->ap.iface, event_data);
    wm_event_post(WM_EVENT_NETIF_GOT_IP, event_data, sizeof(esp_netif_ip_info_t));
    //}
    
    /* Init default WIFI configuration*/
    wifi_init_config_t *wifi_initconf = (wifi_init_config_t *)calloc(1, sizeof(wifi_init_config_t));
    *wifi_initconf = (wifi_init_config_t)WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(wifi_initconf);
    if( ESP_OK != err) {
        ESP_LOGE(ftag, "esp_wifi_init (%s)", esp_err_to_name(err));
        return err;
    }
    free(wifi_initconf);

    if( esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ) {
        ESP_LOGE(ftag, "esp_wifi_set_storage");
        return ESP_FAIL;
    }

    if( esp_wifi_set_country(&(wm_config->radio.country))) {
        ESP_LOGE(ftag, "esp_wifi_set_country");
        return ESP_FAIL;  
    }

    /* Event handlers registation */ /* TODO Do not use default event loop */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wm_wifi_event_handler, NULL, NULL);
    if( ESP_OK != err) {
        ESP_LOGE(ftag, "wifi_event_handler (%s)", esp_err_to_name(err));
        return err;
    }
    //err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wm_ip_event_handler, NULL, NULL);
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wm_ip_event_handler, NULL, NULL);
    if( ESP_OK != err) {
        ESP_LOGE(ftag, "ip_event_handler (%s)", esp_err_to_name(err));
        return err;
    }

    /* Setup AP mode configuration */
    wm_apply_ap_driver_config();    

    /* Apply initial configuration */
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if(err != ESP_OK) {
        ESP_LOGE(ftag, "esp_wifi_set_mode (%s)", esp_err_to_name(err));
        return err;    
    };
    err = esp_wifi_set_config(WIFI_IF_AP, wm_config->ap.driver_config);
    if(err != ESP_OK) {
        ESP_LOGE(ftag, "esp_wifi_set_config(WIFI_IF_AP) (%s)", esp_err_to_name(err));
        return err;
    };
    err = esp_wifi_set_config(WIFI_IF_STA, wm_config->sta.driver_config);
    if(err != ESP_OK) {
        ESP_LOGE(ftag, "esp_wifi_set_config(WIFI_IF_STA) (%s)", esp_err_to_name(err));
        return err;
    };
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    err = esp_wifi_start();
    if( err != ESP_OK ) {
        ESP_LOGE(ftag, "esp_wifi_start (%s)", esp_err_to_name(err));
        return err;
    } else { wm_event_post(WM_EVENT_AP_START, NULL, 0); };

    /* TODO: Remove in release
    ESP_LOGI(ftag,"iface %s / %s created", wm_config->ap.iface->if_desc, wm_config->ap.iface->if_key);
    ESP_LOGI(ftag,"iface %s / %s created", wm_config->ap.iface->if_desc, wm_config->ap.iface->if_key);
    */
    memset(wm_config->blacklistedAPs, 0, sizeof(wm_config->blacklistedAPs));
    xEventGroupSetBits(wm_config->event.group, WIFIMGR_SCAN_BIT );
    xTaskCreate(vConnectTask, "wconn", 2048, NULL, 16, &wm_config->event.apTask_handle);
    xTaskCreate(vScanTask, "wscan", 2048, NULL, 15, &wm_config->event.scanTask_handle);
    wifi_init_done = true;

    return ESP_OK;
}

esp_err_t wm_add_known_network_config( wm_wifi_base_config_t *known_network) {
    if(wm_run_conf->known_net_count >= GONFIG_WIFIMGR_MAX_KNOWN_NETWORKS) return ESP_ERR_NOT_ALLOWED; //event for fail
    wm_ll_known_network_node_t *work = calloc(1, sizeof(wm_ll_known_network_node_t));
    if(work) {
        work->payload.net_config = *known_network;
        work->payload.net_config_id = esp_rom_crc32_le(0, known_network->ssid, strlen(known_network->ssid));
        work->payload.net_config_id = esp_rom_crc32_le(work->payload.net_config_id, known_network->password, strlen(known_network->password));
        work->next = wm_run_conf->known_networks_head;
        wm_run_conf->known_networks_head = work;
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
    //post event with ID or add failed
    //return ( i == WIFIMGR_MAX_KNOWN_AP ) ? ESP_FAIL : ESP_OK;
}

esp_err_t wm_add_known_network( char *ssid, char *pwd) {
    wm_wifi_base_config_t *new_network = calloc(1, sizeof(wm_wifi_base_config_t));
    wm_init_base_config(new_network);
    size_t len = strlen(ssid);
    len = (len>32) ? 32 : len;
    new_network->ssid = calloc(len+1, sizeof(char));
    memcpy(new_network->ssid, ssid, len);
    len = strlen(pwd);
    len = (len>64) ? 64 : len;
    new_network->password = calloc(len+1, sizeof(char));
    memcpy(new_network->password, pwd, len);
    esp_err_t err = wm_add_known_network_config(new_network);
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
    return wm_del_known_net_by_(wm_get_config_id(*ssid));
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