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
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "PlugControl.h"
#include "config.h"

// #define DEBUG
struct netif *esp_netif_get_netif_impl(esp_netif_t *esp_netif);

#define ESP_MAXIMUM_RETRY 5

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

#define START_IP 1
#define END_IP 254

#define PLUG1 GPIO_NUM_5
#define PLUG2 GPIO_NUM_2

#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

const uint8_t PLUG_MAC_1[] = {0x8C, 0xBF, 0xEA, 0xA4, 0xA0, 0xB0};
const uint8_t PLUG_MAC_2[] = {0x8C, 0xBF, 0xEA, 0xA4, 0x1D, 0xF0};

void init(void);

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
#ifdef DEBUG
            ESP_LOGI("WIFI", "retry to connect to the AP");
#endif
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
#ifdef DEBUG
        ESP_LOGI("WIFI", "connect to the AP fail");
#endif
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
#ifdef DEBUG
        ESP_LOGI("WIFI", "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
#endif
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

ip4_addr_t RequestIp(const uint8_t *shutter_mac)
{
    ip4_addr_t result_ip;
    IP4_ADDR(&result_ip, 255, 255, 255, 255);

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);

    struct netif *lwip_netif = esp_netif_get_netif_impl(netif);

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
            struct eth_addr *mac;
            struct netif *netif_out;
            ip4_addr_t *ip_out;

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

void DisableUnusedPins(void)
{
    gpio_config_t io_conf_0 = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
    };
    gpio_config(&io_conf_0);
    gpio_set_level(GPIO_NUM_0, 0);

    gpio_config_t io_conf_rest = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << GPIO_NUM_1) |
                        (1ULL << GPIO_NUM_2) |
                        (1ULL << GPIO_NUM_3) |
                        (1ULL << GPIO_NUM_6) |
                        (1ULL << GPIO_NUM_7) |
                        (1ULL << GPIO_NUM_8) |
                        (1ULL << GPIO_NUM_9) |
                        (1ULL << GPIO_NUM_10) |
                        (1ULL << GPIO_NUM_20) |
                        (1ULL << GPIO_NUM_21),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf_rest);
}

void ESPSleep(void)
{
#ifdef DEBUG
    ESP_LOGI("SLEEP", "Going to sleep now");
#endif
    DisableUnusedPins();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_deep_sleep_start();
}

void setup_wakeup()
{
    const uint64_t wakeup_pins = 1 << PLUG1 | 1 << PLUG2;
    esp_deep_sleep_enable_gpio_wakeup(wakeup_pins, ESP_GPIO_WAKEUP_GPIO_LOW);
}

void GetIps(ip4_addr_t *pPlug1_ip, ip4_addr_t *pPlug2_ip)
{
    nvs_handle_t handle;
    nvs_open("storage", NVS_READWRITE, &handle);
    esp_err_t err1 = nvs_get_u32(handle, "ip1", &pPlug1_ip->addr);
    esp_err_t err2 = nvs_get_u32(handle, "ip2", &pPlug2_ip->addr);
    if (err1 == ESP_ERR_NVS_NOT_FOUND)
    {
        *pPlug1_ip = RequestIp(PLUG_MAC_1);
        nvs_set_u32(handle, "ip1", pPlug1_ip->addr);
#ifdef DEBUG
        ESP_LOGI("NVM", "Saved plug1 IP");
#endif
    }
    if (err2 == ESP_ERR_NVS_NOT_FOUND)
    {
        *pPlug2_ip = RequestIp(PLUG_MAC_2);
        nvs_set_u32(handle, "ip2", pPlug2_ip->addr);
#ifdef DEBUG
        ESP_LOGI("NVM", "Saved plug2 IP");
#endif
    }
    if (err1 == ESP_ERR_NVS_NOT_FOUND || err2 == ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_commit(handle);
    }
    nvs_close(handle);

#ifdef DEBUG
    ESP_LOGI("PLUG", "Plug1 IP: " IPSTR, IP2STR(pPlug1_ip));
    ESP_LOGI("PLUG", "Plug2 IP: " IPSTR, IP2STR(pPlug2_ip));
#endif
}

void SetIps(ip4_addr_t plug1_ip, ip4_addr_t plug2_ip)
{
    nvs_handle_t handle;
    nvs_open("storage", NVS_READWRITE, &handle);
    nvs_set_u32(handle, "ip1", plug1_ip.addr);
    nvs_set_u32(handle, "ip2", plug2_ip.addr);
    nvs_commit(handle);
    nvs_close(handle);

#ifdef DEBUG
    ESP_LOGI("PLUG", "Plug1 IP: " IPSTR, IP2STR(&plug1_ip));
    ESP_LOGI("PLUG", "Plug2 IP: " IPSTR, IP2STR(&plug2_ip));
#endif
}

void app_main()
{
    int gpioStates = REG_READ(GPIO_IN_REG);
    int plug1State = !((gpioStates >> PLUG1) & 1);
    int plug2State = !((gpioStates >> PLUG2) & 1);
    init();

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PLUG1) | (1ULL << PLUG2),
    };
    gpio_config(&io_conf);
    setup_wakeup();

    ip4_addr_t plug1_ip;
    ip4_addr_t plug2_ip;
    GetIps(&plug1_ip, &plug2_ip);

    esp_sleep_enable_gpio_wakeup();

    const TickType_t awake_time = pdMS_TO_TICKS(5000); // e.g. 5 seconds awake
    TickType_t last_wake = xTaskGetTickCount();

    while (1)
    {
        if (!plug1State && !plug2State)
        {
            gpioStates = REG_READ(GPIO_IN_REG);
            plug1State = !((gpioStates >> PLUG1) & 1);
            plug2State = !((gpioStates >> PLUG2) & 1);
        }
        if (plug1State || plug2State)
        {
            last_wake = xTaskGetTickCount();
        }
        if ((xTaskGetTickCount() - last_wake) > awake_time)
        {
            ESPSleep();
        }

        if (plug1State && plug2State)
        {
            plug1_ip = RequestIp(PLUG_MAC_1);
            plug2_ip = RequestIp(PLUG_MAC_2);
            SetIps(plug1_ip, plug2_ip);
#ifdef DEBUG
            ESP_LOGI("SHUTTER", "P IP: " IPSTR, IP2STR(&plug1_ip));
            ESP_LOGI("SHUTTER", "Shutter IP: " IPSTR, IP2STR(&plug2_ip));
#endif
        }

        if (plug1State)
        {
            TogglePlug(plug1_ip);
            plug1State = 0;
        }
        if (plug2State)
        {
            TogglePlug(plug2_ip);
            plug2State = 0;
        }

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
            .password = WIFI_PASS},
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

#ifdef DEBUG
    ESP_LOGI("WIFI", "wifi_init_sta finished.");
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI("WIFI", "connected to ap SSID:%s",
                 WIFI_SSID);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI("WIFI", "Failed to connect to SSID:%s",
                 WIFI_SSID);
    }
    else
    {
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

#ifdef DEBUG
    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL)
    {
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
    }
    ESP_LOGI("WIFI", "ESP_WIFI_MODE_STA");
#endif
    wifi_init_sta();
}