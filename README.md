# IDF WiFi manager

Easy IDF WiFi connection manager component for ESP IDF. It has been tested with the ESP32, ESP32-S3, and ESP32-C6 boards.

![Static Badge](https://img.shields.io/badge/any_text-you_like-blue?logo=espressif) ![Dynamic YAML Badge](https://img.shields.io/badge/dynamic/yaml?url=https://raw.githubusercontent.com/RDobrinov/idf_wifi_manager/dev-events/idf_component.yml&query=$.verison&label=Ver)

---

## Features

* Automatically switching between AP and STA modes
* Easy AP and STA modes configuration with static or dynamic IP
* Custom DNS Servers
* Event notification via __default__ or __user created__ event loop 
* SNTP Time Synchronization in System Time
* Up to 30 known networks for STA mode
* Automatically blacklist APs with the wrong password configured
* Channels rating capability to auto-select the best channel in AP mode


## Installation

1. Create *idf_component.yml*
```
idf.py create-manifest
```
2. Edit ***idf_component.yml*** to add dependency
```
dependencies:
  ...
  idf_wifi_manager:
    version: "main"
    git: git@github.com:RDobrinov/idf_wifi_manager.git
  ...
```
3. Reconfigure project

or 

4. Download and unzip component in project ***components*** folder

### Example
```
#include <stdio.h>
#include "idf_wifi_manager.h"

void wifimgr_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    /* Do nothing */
    return;
}

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
    esp_event_loop_create(&uevent_args, uevent_loop);
    esp_event_handler_instance_register_with(*uevent_loop, WM_EVENT, ESP_EVENT_ANY_ID, wifimgr_event_handler, NULL, NULL);

    wm_init_wifi_manager(NULL, uevent_loop);
    wm_add_known_network("Test0", "1234567890");
    wm_add_known_network("Test1", "1234567890");
    wm_add_known_network("Test2", "1234567890");
    wm_add_known_network("Test3", "1234567890");
    wm_add_known_network("Test4", "1234567890");
    wm_add_known_network("Test5", "1234567890");
    wm_del_known_net_by_ssid("Test3");
    wm_del_known_net_by_ssid("Test2");
    wm_add_known_network("Test4", "1234567890");
}
```