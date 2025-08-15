#include <string.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/etharp.h"
#include "lwip/ip_addr.h"
#include "driver/gpio.h"
#include "PlugControl.h"
#include "config.h"


struct netif* esp_netif_get_netif_impl(esp_netif_t* esp_netif);

#define ESP_MAXIMUM_RETRY 5

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

#define START_IP 1
#define END_IP 254

#define PLUG1 GPIO_NUM_5
#define PLUG2 GPIO_NUM_4


static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

const uint8_t PLUG_MAC_1[] = {0x8C, 0xBF, 0xEA, 0xA4, 0xA0, 0xB0};
const uint8_t PLUG_MAC_2[] = {0x8C, 0xBF, 0xEA, 0xA4, 0x1D, 0xF0};

volatile int triggered_button = -1;

void IRAM_ATTR button_isr_handler(void* arg)
{
    triggered_button = (int)(intptr_t)arg;
}

void init(void);

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
#ifdef DEBUG
            ESP_LOGI("WIFI", "retry to connect to the AP");
#endif
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
#ifdef DEBUG
        ESP_LOGI("WIFI","connect to the AP fail");
#endif
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
#ifdef DEBUG
        ESP_LOGI("WIFI", "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
#endif
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

ip4_addr_t find_plug_ip(const uint8_t* shutter_mac)
{
    ip4_addr_t result_ip;
    IP4_ADDR(&result_ip, 255, 255, 255, 255);

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);

    struct netif* lwip_netif = esp_netif_get_netif_impl(netif);

    for (int ii = START_IP; ii <= END_IP; ++ii)
    {
        ip4_addr_t target_ip;
        IP4_ADDR(&target_ip,
                 ip_info.ip.addr & 0xFF,
                 (ip_info.ip.addr >> 8) & 0xFF,
                 (ip_info.ip.addr >> 16) & 0xFF,
                 ii);

        if (etharp_request(lwip_netif, &target_ip) != ERR_OK)
        {
#ifdef DEBUG
            ESP_LOGI("SHUTTER", "etharp_request failed for IP: " IPSTR, IP2STR(&target_ip));
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    
        if ((ii % 10 == 0) || (ii == END_IP - 1))
        {
            struct eth_addr* mac;
            struct netif* netif_out;
            ip4_addr_t* ip_out;

            for (int jj = 0; jj < 10; ++jj)
            {
                if (etharp_get_entry(jj, &ip_out, &netif_out, &mac))
                {
                    if (memcmp(mac->addr, shutter_mac, 6) == 0)
                    {
#ifdef DEBUG
                    ESP_LOGI("SHUTTER", "Found MAC: " MACSTR " for IP: " IPSTR,
                            mac->addr[0], mac->addr[1], mac->addr[2], mac->addr[3], mac->addr[4], mac->addr[5], IP2STR(ip_out));
#endif
                        return *ip_out;
                        break;
                    }
                }
            }
        }
    }
    return result_ip;
}

void wait_for_wifi_connected()
{
    // Wait until WIFI_CONNECTED_BIT is set
    while (!(xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void ESPSleep(void)
{
#ifdef DEBUG
    ESP_LOGI("SLEEP", "Going to light sleep now");
    esp_log_level_set("*", ESP_LOG_NONE);
#endif
    esp_light_sleep_start();
#ifdef DEBUG
    esp_log_level_set("*", ESP_LOG_INFO);
#endif
}

void app_main() {
    init();

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PLUG1) | (1ULL << PLUG2),
        // .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PLUG1, button_isr_handler, (void*) PLUG1);
    gpio_isr_handler_add(PLUG2, button_isr_handler, (void*) PLUG2);
    gpio_wakeup_enable(PLUG1, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(PLUG2, GPIO_INTR_LOW_LEVEL);

    ip4_addr_t plug1_ip = find_plug_ip(PLUG_MAC_1);
    ip4_addr_t plug2_ip = find_plug_ip(PLUG_MAC_2);
#ifdef DEBUG
    ESP_LOGI("SHUTTER", "Shutter IP: " IPSTR, IP2STR(&plug1_ip));
    ESP_LOGI("SHUTTER", "Shutter IP: " IPSTR, IP2STR(&plug2_ip));
#endif
    
    esp_sleep_enable_gpio_wakeup();

    const TickType_t awake_time = pdMS_TO_TICKS(5000); // e.g. 5 seconds awake
    TickType_t last_wake = xTaskGetTickCount();

    while(1)
    {
        if ((xTaskGetTickCount() - last_wake) > awake_time)
        {
            ESPSleep();
        }
        if (triggered_button != -1)
        {
#ifdef DEBUG
            ESP_LOGI("BUTTON", "Button %d pressed", triggered_button);
#endif
            wait_for_wifi_connected();
            last_wake = xTaskGetTickCount();
        }

        if (gpio_get_level(PLUG1) == 0 && gpio_get_level(PLUG2) == 0)
        {
            ip4_addr_t plug1_ip = find_plug_ip(PLUG_MAC_1);
            ip4_addr_t plug2_ip = find_plug_ip(PLUG_MAC_2);
#ifdef DEBUG
            ESP_LOGI("SHUTTER", "Shutter IP: " IPSTR, IP2STR(&plug1_ip));
            ESP_LOGI("SHUTTER", "Shutter IP: " IPSTR, IP2STR(&plug2_ip));
#endif
        }


        switch(triggered_button)
        {
            case PLUG1:
                TogglePlug(plug1_ip);
                break;
            case PLUG2:
                TogglePlug(plug2_ip);
                break;
            default:
                break;
        }

        triggered_button = -1;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            // .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            // .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

#ifdef DEBUG
    ESP_LOGI("WIFI", "wifi_init_sta finished.");
#endif

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
#ifdef DEBUG
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI("WIFI", "connected to ap SSID:%s",
                 ESP_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI("WIFI", "Failed to connect to SSID:%s",
                 ESP_WIFI_SSID);
    } else {
        ESP_LOGE("WIFI", "UNEXPECTED EVENT");
    }
#endif
}

void init()
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL)
    {
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
    }
#ifdef DEBUG
    ESP_LOGI("WIFI", "ESP_WIFI_MODE_STA");
#endif
    wifi_init_sta();
}