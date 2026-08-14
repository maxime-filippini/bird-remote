#include "bird_remote_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define REQUEST_TIMEOUT_MS 4000
#define RESPONSE_DRAIN_SIZE 128

static const char *TAG = "bird_remote_client";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_in_flight;

typedef struct {
    bird_remote_result_callback_t callback;
} request_context_t;

bool bird_remote_request_in_flight(void)
{
    bool result;
    taskENTER_CRITICAL(&s_lock);
    result = s_in_flight;
    taskEXIT_CRITICAL(&s_lock);
    return result;
}

static void request_task(void *argument)
{
    request_context_t context = *(request_context_t *)argument;
    free(argument);

    char url[sizeof(CONFIG_BIRD_REMOTE_SERVER_URL) + 24];
    snprintf(url, sizeof(url), "%s/api/next", CONFIG_BIRD_REMOTE_SERVER_URL);
    esp_http_client_config_t configuration = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = REQUEST_TIMEOUT_MS,
        .buffer_size = RESPONSE_DRAIN_SIZE,
        .buffer_size_tx = 256,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&configuration);
    esp_err_t result = ESP_ERR_NO_MEM;
    int status = 0;
    if (client != NULL) {
        esp_http_client_set_header(client, "Authorization", "Bearer " CONFIG_BIRD_REMOTE_TOKEN);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, "", 0);
        result = esp_http_client_perform(client);
        if (result == ESP_OK) {
            status = esp_http_client_get_status_code(client);
        }
        esp_http_client_cleanup(client);
    }

    bool success = result == ESP_OK && status >= 200 && status < 300;
    if (success) {
        ESP_LOGI(TAG, "Server advanced bird (HTTP %d)", status);
    } else {
        ESP_LOGW(TAG, "Next-bird request failed: %s, HTTP %d", esp_err_to_name(result), status);
    }

    taskENTER_CRITICAL(&s_lock);
    s_in_flight = false;
    taskEXIT_CRITICAL(&s_lock);
    if (context.callback != NULL) {
        context.callback(success, status);
    }
    vTaskDelete(NULL);
}

bool bird_remote_next_async(bird_remote_result_callback_t callback)
{
    if (CONFIG_BIRD_REMOTE_SERVER_URL[0] == '\0' || CONFIG_BIRD_REMOTE_TOKEN[0] == '\0') {
        ESP_LOGE(TAG, "Bird server URL or token is empty; run the Mac installer before building");
        return false;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_in_flight) {
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    s_in_flight = true;
    taskEXIT_CRITICAL(&s_lock);

    request_context_t *context = malloc(sizeof(*context));
    if (context == NULL) {
        taskENTER_CRITICAL(&s_lock);
        s_in_flight = false;
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    context->callback = callback;
    if (xTaskCreate(request_task, "bird_remote_post", 4096, context, 5, NULL) != pdPASS) {
        free(context);
        taskENTER_CRITICAL(&s_lock);
        s_in_flight = false;
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    return true;
}
