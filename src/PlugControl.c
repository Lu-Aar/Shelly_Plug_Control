#include "PlugControl.h"
#include "lwip/ip4_addr.h"
#include "esp_http_client.h"
#include "esp_log.h"

esp_http_client_handle_t CreateClient(ip4_addr_t shutter_ip, char* method)
{
    char ip_str[16];
    char url[64];
    snprintf(ip_str, sizeof(ip_str), "%s", ip4addr_ntoa(&shutter_ip));
    snprintf(url, sizeof(url), "http://%s/rpc/Switch.%s", ip_str, method);
    
    esp_http_client_config_t config = {
        .url = url,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    return client;
}


void SendPostRequest(esp_http_client_handle_t client)
{
    esp_http_client_set_method(client, HTTP_METHOD_POST);

    char post_data[] = "{\"id\":0}";

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
#ifdef DEBUG
    if (err == ESP_OK) {
        ESP_LOGI("HTTP", "Status = %d, content_length = %lld\n",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        
        ESP_LOGE("HTTP", "HTTP request failed: %s\n", esp_err_to_name(err));
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(200));
}

void TogglePlug(ip4_addr_t shutter_ip)
{
    esp_http_client_handle_t client = CreateClient(shutter_ip, "Toggle");
    SendPostRequest(client);
}