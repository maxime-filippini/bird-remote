#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bird_remote_client.h"
#include "bsp/esp-bsp.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

#define SETUP_AP_CHANNEL 6
#define SETUP_AP_MAX_CONNECTIONS 2
#define SETUP_MAX_RETRIES 5
#define SETUP_HTTP_BODY_MAX 256
#define SETUP_SCAN_LIMIT 12
#define SETUP_PORTAL_URL "http://192.168.4.1"

static const char *TAG = "wifi_provisioner";

static httpd_handle_t s_http_server;
static lv_obj_t *s_screen;
static lv_obj_t *s_eyebrow_label;
static lv_obj_t *s_title_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_details_label;
static lv_obj_t *s_action_button;
static lv_obj_t *s_action_button_label;
static lv_obj_t *s_pixel_bird;

static bool s_have_credentials;
static bool s_connected;
static bool s_provisioning_active;
static bool s_shutdown_scheduled;
static int s_retry_count;
static char s_station_ssid[33];
static char s_station_ip[16];
static char s_setup_ssid[33];
static char s_setup_password[65];

static void provisioning_task(void *context);

static void ui_update(const char *status, const char *details, const char *button_text, bool button_enabled)
{
    if (s_status_label == NULL || !bsp_display_lock(1000)) {
        return;
    }

    lv_label_set_text(s_status_label, status);
    lv_label_set_text(s_details_label, details);
    lv_label_set_text(s_action_button_label, button_text);

    if (button_enabled) {
        lv_obj_remove_state(s_action_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_action_button, LV_STATE_DISABLED);
    }

    bsp_display_unlock();
}

static lv_obj_t *create_pixel(lv_obj_t *parent, int32_t x, int32_t y,
                              int32_t width, int32_t height, uint32_t color)
{
    lv_obj_t *pixel = lv_obj_create(parent);
    lv_obj_set_pos(pixel, x, y);
    lv_obj_set_size(pixel, width, height);
    lv_obj_set_style_radius(pixel, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(pixel, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pixel, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pixel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(pixel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pixel, LV_OBJ_FLAG_CLICKABLE);
    return pixel;
}

static void style_action_button_for_setup(void)
{
    lv_obj_set_size(s_action_button, 284, 72);
    lv_obj_align(s_action_button, LV_ALIGN_BOTTOM_MID, 0, -64);
    lv_obj_set_style_radius(s_action_button, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_action_button, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_action_button, 0, LV_PART_MAIN);
    lv_obj_set_style_translate_y(s_action_button, 0, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_action_button, lv_color_hex(0x1677FF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_action_button, lv_color_hex(0x125FCC), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(s_pixel_bird, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(s_action_button_label);
}

static void style_action_button_for_remote(void)
{
    /* Percentages preserve a small, even bezel in the mounted orientation. */
    lv_obj_set_size(s_action_button, lv_pct(94), lv_pct(92));
    lv_obj_align(s_action_button, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_action_button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_action_button, lv_color_hex(0xFFD83D), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_action_button, lv_color_hex(0xFF9F1C), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_action_button, 6, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_action_button, lv_color_hex(0x321B45), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_action_button, lv_color_hex(0x321B45), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_action_button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_action_button, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_x(s_action_button, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_y(s_action_button, 4, LV_PART_MAIN);
    lv_obj_set_style_translate_y(s_action_button, 6, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_remove_flag(s_pixel_bird, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_action_button_label, LV_ALIGN_BOTTOM_MID, 0, -54);
}

static void ui_show_remote(const char *ip_address)
{
    if (!bsp_display_lock(1000)) {
        return;
    }

    (void)ip_address;
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x160D2B), LV_PART_MAIN);
    lv_obj_add_flag(s_eyebrow_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_title_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_details_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_action_button_label, "NEW BIRD!");
    lv_obj_set_style_text_color(s_action_button_label, lv_color_hex(0x321B45), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_action_button_label, &lv_font_montserrat_24, LV_PART_MAIN);
    style_action_button_for_remote();
    if (bird_remote_request_in_flight()) {
        lv_label_set_text(s_action_button_label, "FLYING...");
        lv_obj_add_state(s_action_button, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_action_button, LV_STATE_DISABLED);
    }

    bsp_display_unlock();
}

static void start_provisioning(void)
{
    if (s_provisioning_active) {
        return;
    }

    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x05070A), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_eyebrow_label, lv_color_hex(0x36A9FF), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_action_button_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_action_button_label, LV_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_remove_flag(s_eyebrow_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_title_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_details_label, LV_OBJ_FLAG_HIDDEN);
    style_action_button_for_setup();
    lv_obj_add_state(s_action_button, LV_STATE_DISABLED);
    lv_label_set_text(s_action_button_label, "Starting...");

    if (xTaskCreate(provisioning_task, "wifi_provisioning", 6144, NULL, 5, NULL) != pdPASS) {
        lv_obj_remove_state(s_action_button, LV_STATE_DISABLED);
        lv_label_set_text(s_action_button_label, "Try again");
        lv_label_set_text(s_status_label, "Could not start setup");
    }
}

static void remote_request_result(bool success, int status_code)
{
    if (!bsp_display_lock(1000)) {
        return;
    }
    if (s_connected) {
        lv_label_set_text(s_action_button_label, success ? "NEW BIRD!" : "TRY AGAIN");
        lv_obj_set_style_bg_color(s_action_button,
                                  lv_color_hex(success ? 0x72F1B8 : 0xFF6B6B),
                                  LV_PART_MAIN);
        lv_obj_remove_state(s_action_button, LV_STATE_DISABLED);
    }
    bsp_display_unlock();
    if (!success) {
        ESP_LOGW(TAG, "Server command failed with HTTP status %d", status_code);
    }
}

static void action_button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    if (!s_connected) {
        start_provisioning();
        return;
    }

    lv_obj_add_state(s_action_button, LV_STATE_DISABLED);
    lv_label_set_text(s_action_button_label, "FLYING...");
    lv_obj_set_style_bg_color(s_action_button, lv_color_hex(0xFFD83D), LV_PART_MAIN);
    if (!bird_remote_next_async(remote_request_result)) {
        lv_label_set_text(s_action_button_label, "TRY AGAIN");
        lv_obj_set_style_bg_color(s_action_button, lv_color_hex(0xFF6B6B), LV_PART_MAIN);
        lv_obj_remove_state(s_action_button, LV_STATE_DISABLED);
    }
}

static void create_ui(lv_display_t *display)
{
    lv_obj_t *screen = lv_display_get_screen_active(display);
    s_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x05070A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_eyebrow_label = lv_label_create(screen);
    lv_label_set_text(s_eyebrow_label, "NETWORK SETUP");
    lv_obj_set_style_text_color(s_eyebrow_label, lv_color_hex(0x36A9FF), LV_PART_MAIN);
    lv_obj_align(s_eyebrow_label, LV_ALIGN_TOP_MID, 0, 30);

    s_title_label = lv_label_create(screen);
    lv_label_set_text(s_title_label, "Wi-Fi");
    lv_obj_set_width(s_title_label, 330);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 62);

    s_status_label = lv_label_create(screen);
    lv_label_set_text(s_status_label, "Starting...");
    lv_obj_set_width(s_status_label, 320);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 124);

    s_details_label = lv_label_create(screen);
    lv_label_set_text(s_details_label, "Checking saved credentials");
    lv_obj_set_width(s_details_label, 320);
    lv_obj_set_style_text_color(s_details_label, lv_color_hex(0x9AA6B5), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_details_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_details_label, LV_ALIGN_TOP_MID, 0, 164);

    s_action_button = lv_button_create(screen);
    lv_obj_set_size(s_action_button, 284, 72);
    lv_obj_align(s_action_button, LV_ALIGN_BOTTOM_MID, 0, -64);
    lv_obj_set_style_radius(s_action_button, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_action_button, lv_color_hex(0x1677FF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_action_button, lv_color_hex(0x124B85), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_action_button, action_button_event, LV_EVENT_CLICKED, NULL);

    s_action_button_label = lv_label_create(s_action_button);
    lv_label_set_text(s_action_button_label, "Connect Wi-Fi");
    lv_obj_set_style_text_color(s_action_button_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(s_action_button_label);

    s_pixel_bird = lv_obj_create(s_action_button);
    lv_obj_set_size(s_pixel_bird, 192, 148);
    lv_obj_align(s_pixel_bird, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_opa(s_pixel_bird, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pixel_bird, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_pixel_bird, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_pixel_bird, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_pixel_bird, LV_OBJ_FLAG_CLICKABLE);
    create_pixel(s_pixel_bird, 24, 68, 104, 64, 0x36A9FF);  /* body */
    create_pixel(s_pixel_bird, 84, 20, 64, 64, 0x72F1B8);   /* head */
    create_pixel(s_pixel_bird, 116, 36, 16, 16, 0x160D2B);  /* eye */
    create_pixel(s_pixel_bird, 148, 52, 36, 24, 0xFF6B35);  /* beak */
    create_pixel(s_pixel_bird, 40, 84, 56, 36, 0x5941A9);   /* wing */
    create_pixel(s_pixel_bird, 8, 60, 32, 24, 0x36A9FF);    /* tail */
    create_pixel(s_pixel_bird, 84, 132, 16, 16, 0xFF6B35);  /* foot */
    lv_obj_add_flag(s_pixel_bird, LV_OBJ_FLAG_HIDDEN);
}

static void copy_wifi_ssid(char destination[33], const uint8_t source[32])
{
    size_t length = strnlen((const char *)source, 32);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool url_decode(char *destination, size_t destination_size, const char *source)
{
    size_t output = 0;

    while (*source != '\0') {
        if (output + 1 >= destination_size) {
            return false;
        }

        if (*source == '+') {
            destination[output++] = ' ';
            source++;
            continue;
        }

        if (*source == '%') {
            if (!isxdigit((unsigned char)source[1]) || !isxdigit((unsigned char)source[2])) {
                return false;
            }

            char hex[3] = {source[1], source[2], '\0'};
            char decoded = (char)strtoul(hex, NULL, 16);
            if (decoded == '\0') {
                return false;
            }
            destination[output++] = decoded;
            source += 3;
            continue;
        }

        destination[output++] = *source++;
    }

    destination[output] = '\0';
    return true;
}

static void html_escape(char *destination, size_t destination_size, const char *source)
{
    size_t output = 0;

    while (*source != '\0' && output + 1 < destination_size) {
        const char *replacement = NULL;
        switch (*source) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default: break;
        }

        if (replacement != NULL) {
            size_t length = strlen(replacement);
            if (output + length >= destination_size) {
                break;
            }
            memcpy(destination + output, replacement, length);
            output += length;
        } else {
            destination[output++] = *source;
        }
        source++;
    }

    destination[output] = '\0';
}

static const char PORTAL_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>ESP32 Wi-Fi Setup</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#070a0f;color:#f5f7fa;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif}"
    ".wrap{max-width:480px;margin:auto;padding:32px 20px}.tag{color:#49adff;font-size:12px;font-weight:700;letter-spacing:.12em}"
    "h1{font-size:34px;margin:10px 0 8px}.hint{color:#9da9b8;line-height:1.5;margin-bottom:28px}"
    ".card{background:#121722;border:1px solid #252e3d;border-radius:18px;padding:22px;box-shadow:0 18px 60px #0008}"
    "label{display:block;font-size:13px;color:#b7c1cf;margin:16px 0 7px}input{width:100%;min-height:50px;border-radius:11px;"
    "border:1px solid #354157;background:#090d14;color:white;padding:0 14px;font-size:16px}button{width:100%;min-height:52px;margin-top:24px;"
    "border:0;border-radius:12px;background:#1677ff;color:white;font-size:16px;font-weight:700}small{display:block;color:#7f8b9a;margin-top:16px;line-height:1.45}"
    "</style></head><body><main class=wrap><div class=tag>ESP32 PROVISIONING</div><h1>Connect this board</h1>"
    "<p class=hint>Select the Wi-Fi used by your laptop and enter its password. Credentials are stored on the board.</p>"
    "<form class=card method=post action=/connect><label for=ssid>Wi-Fi network</label>"
    "<input id=ssid name=ssid list=networks maxlength=32 required autocomplete=off placeholder=\"Network name\">"
    "<datalist id=networks>";

static const char PORTAL_TAIL[] =
    "</datalist><label for=password>Password</label>"
    "<input id=password name=password type=password maxlength=63 autocomplete=current-password placeholder=\"Wi-Fi password\">"
    "<button type=submit>Connect board</button><small>The setup network closes after a successful connection. Your laptop should then return to its normal Wi-Fi.</small>"
    "</form></main></body></html>";

static esp_err_t portal_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_send_chunk(request, PORTAL_HEAD, HTTPD_RESP_USE_STRLEN);

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    wifi_ap_record_t records[SETUP_SCAN_LIMIT] = {0};
    uint16_t count = SETUP_SCAN_LIMIT;

    esp_err_t result = esp_wifi_scan_start(&scan_config, true);
    if (result == ESP_OK) {
        result = esp_wifi_scan_get_ap_records(&count, records);
    }

    if (result == ESP_OK) {
        for (uint16_t i = 0; i < count; i++) {
            bool duplicate = false;
            for (uint16_t earlier = 0; earlier < i; earlier++) {
                if (strncmp((const char *)records[i].ssid, (const char *)records[earlier].ssid, 32) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate || records[i].ssid[0] == '\0') {
                continue;
            }

            char ssid[33];
            char escaped[160];
            copy_wifi_ssid(ssid, records[i].ssid);
            html_escape(escaped, sizeof(escaped), ssid);

            char option[400];
            snprintf(option, sizeof(option), "<option value=\"%s\">%s (%d dBm)%s</option>",
                     escaped, escaped, records[i].rssi,
                     records[i].authmode == WIFI_AUTH_OPEN ? " - open" : "");
            httpd_resp_send_chunk(request, option, HTTPD_RESP_USE_STRLEN);
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(result));
    }

    httpd_resp_send_chunk(request, PORTAL_TAIL, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t apply_station_credentials(const char *ssid, const char *password)
{
    wifi_config_t configuration = {0};
    strlcpy((char *)configuration.sta.ssid, ssid, sizeof(configuration.sta.ssid));
    strlcpy((char *)configuration.sta.password, password, sizeof(configuration.sta.password));
    configuration.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    configuration.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    configuration.sta.threshold.authmode = WIFI_AUTH_OPEN;
    configuration.sta.pmf_cfg.capable = true;
    configuration.sta.pmf_cfg.required = false;
    configuration.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &configuration);
    if (result != ESP_OK) {
        return result;
    }

    strlcpy(s_station_ssid, ssid, sizeof(s_station_ssid));
    s_have_credentials = true;
    s_retry_count = 0;

    result = esp_wifi_disconnect();
    if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "Disconnect before applying credentials returned %s", esp_err_to_name(result));
    }

    result = esp_wifi_connect();
    if (result == ESP_ERR_WIFI_CONN) {
        return ESP_OK;
    }
    return result;
}

static esp_err_t connect_post_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len >= SETUP_HTTP_BODY_MAX) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form data");
    }

    char body[SETUP_HTTP_BODY_MAX] = {0};
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        int result = httpd_req_recv(request, body + received, request->content_len - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)result;
    }
    body[received] = '\0';

    char encoded_ssid[97] = {0};
    char encoded_password[193] = {0};
    char ssid[33] = {0};
    char password[64] = {0};

    if (httpd_query_key_value(body, "ssid", encoded_ssid, sizeof(encoded_ssid)) != ESP_OK ||
        !url_decode(ssid, sizeof(ssid), encoded_ssid)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi network name");
    }

    if (httpd_query_key_value(body, "password", encoded_password, sizeof(encoded_password)) == ESP_OK &&
        !url_decode(password, sizeof(password), encoded_password)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid password");
    }

    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    if (ssid_length == 0 || ssid_length > 32 ||
        (password_length != 0 && (password_length < 8 || password_length > 63))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "SSID must be present; password must be empty or 8-63 characters");
    }

    esp_err_t result = apply_station_credentials(ssid, password);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not apply station credentials: %s", esp_err_to_name(result));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start connection");
    }

    char details[128];
    snprintf(details, sizeof(details), "Joining %s\nKeep this page open", ssid);
    ui_update("Connecting...", details, "Setup active", false);
    ESP_LOGI(TAG, "Received credentials for SSID '%s'", ssid);

    const char response[] =
        "<!doctype html><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<style>body{background:#070a0f;color:#fff;font-family:-apple-system,sans-serif;text-align:center;padding:64px 24px}"
        "h1{color:#49adff}p{color:#aab4c2;line-height:1.5}</style>"
        "<h1>Connecting...</h1><p>The board is testing those credentials.<br>Check its screen for the result.</p>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_get_handler(httpd_req_t *request)
{
    if (s_provisioning_active) {
        return portal_get_handler(request);
    }
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Provisioning is inactive");
}

static esp_err_t not_found_handler(httpd_req_t *request, httpd_err_code_t error)
{
    (void)error;
    if (s_provisioning_active) {
        httpd_resp_set_status(request, "302 Found");
        httpd_resp_set_hdr(request, "Location", SETUP_PORTAL_URL "/");
        return httpd_resp_send(request, NULL, 0);
    }
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
}

static const httpd_uri_t ROOT_GET = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
};

static const httpd_uri_t CONNECT_POST = {
    .uri = "/connect",
    .method = HTTP_POST,
    .handler = connect_post_handler,
};

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t configuration = HTTPD_DEFAULT_CONFIG();
    configuration.max_uri_handlers = 2;
    configuration.lru_purge_enable = true;
    configuration.recv_wait_timeout = 10;
    configuration.send_wait_timeout = 10;

    esp_err_t result = httpd_start(&s_http_server, &configuration);
    if (result != ESP_OK) {
        return result;
    }

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &ROOT_GET));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &CONNECT_POST));
    ESP_ERROR_CHECK(httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND, not_found_handler));
    return ESP_OK;
}

static void stop_provisioning_task(void *context)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(12000));

    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not stop setup access point: %s", esp_err_to_name(result));
    }
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }

    s_provisioning_active = false;
    s_shutdown_scheduled = false;
    ui_show_remote(s_station_ip);
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *context, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)context;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_have_credentials) {
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (!s_have_credentials) {
            return;
        }

        if (s_retry_count < SETUP_MAX_RETRIES) {
            s_retry_count++;
            esp_wifi_connect();
            char details[128];
            snprintf(details, sizeof(details), "Joining %s\nRetry %d of %d",
                     s_station_ssid, s_retry_count, SETUP_MAX_RETRIES);
            ui_update("Connecting...", details,
                      s_provisioning_active ? "Setup active" : "Reconnecting...",
                      false);
        } else {
            ui_update("Connection failed",
                      s_provisioning_active ? "Check the password in the portal" : "Tap below to reconfigure",
                      s_provisioning_active ? "Setup active" : "Connect Wi-Fi",
                      !s_provisioning_active);
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        char details[192];
        snprintf(details, sizeof(details), "Laptop connected\nOpen %s", SETUP_PORTAL_URL);
        ui_update("Setup portal ready", details, "Setup active", false);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        s_retry_count = 0;
        s_connected = true;
        snprintf(s_station_ip, sizeof(s_station_ip), IPSTR, IP2STR(&event->ip_info.ip));

        char details[160];
        snprintf(details, sizeof(details), "%s\nIP: " IPSTR,
                 s_station_ssid, IP2STR(&event->ip_info.ip));
        if (s_provisioning_active) {
            ui_update("Connected", details, "Finishing...", false);
        } else {
            ui_show_remote(s_station_ip);
        }
        ESP_LOGI(TAG, "Connected to '%s' with IP " IPSTR,
                 s_station_ssid, IP2STR(&event->ip_info.ip));

        if (s_provisioning_active && !s_shutdown_scheduled) {
            s_shutdown_scheduled = true;
            xTaskCreate(stop_provisioning_task, "stop_provisioning", 4096, NULL, 4, NULL);
        }
    }
}

static void initialize_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() != NULL ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_ap() != NULL ? ESP_OK : ESP_FAIL);

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&initialization));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    wifi_config_t saved_configuration = {0};
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &saved_configuration));
    copy_wifi_ssid(s_station_ssid, saved_configuration.sta.ssid);
    s_have_credentials = s_station_ssid[0] != '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (s_have_credentials) {
        char details[96];
        snprintf(details, sizeof(details), "Joining %s", s_station_ssid);
        ui_update("Connecting...", details, "Connecting...", false);
    } else {
        ui_update("Not configured", "Tap below to create a setup network", "Connect Wi-Fi", true);
    }
}

static void provisioning_task(void *context)
{
    (void)context;
    s_provisioning_active = true;
    s_retry_count = 0;

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(s_setup_ssid, sizeof(s_setup_ssid), "ESP32-Setup-%02X%02X", mac[4], mac[5]);
    snprintf(s_setup_password, sizeof(s_setup_password), "setup-%08" PRIX32, esp_random());

    wifi_config_t access_point = {0};
    strlcpy((char *)access_point.ap.ssid, s_setup_ssid, sizeof(access_point.ap.ssid));
    strlcpy((char *)access_point.ap.password, s_setup_password, sizeof(access_point.ap.password));
    access_point.ap.ssid_len = strlen(s_setup_ssid);
    access_point.ap.channel = SETUP_AP_CHANNEL;
    access_point.ap.max_connection = SETUP_AP_MAX_CONNECTIONS;
    access_point.ap.authmode = WIFI_AUTH_WPA2_PSK;
    access_point.ap.pmf_cfg.capable = true;
    access_point.ap.pmf_cfg.required = false;

    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result == ESP_OK) {
        result = esp_wifi_set_config(WIFI_IF_AP, &access_point);
    }
    if (result == ESP_OK) {
        result = start_http_server();
    }

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not start provisioning: %s", esp_err_to_name(result));
        s_provisioning_active = false;
        ui_update("Setup failed", esp_err_to_name(result), "Try again", true);
        vTaskDelete(NULL);
        return;
    }

    char details[256];
    snprintf(details, sizeof(details), "Join: %s\nPassword: %s\nOpen: %s",
             s_setup_ssid, s_setup_password, SETUP_PORTAL_URL);
    ui_update("Setup network active", details, "Setup active", false);
    ESP_LOGI(TAG, "Provisioning AP '%s' started; portal at %s", s_setup_ssid, SETUP_PORTAL_URL);
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "Display initialization failed");
        abort();
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(80));

    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "Could not lock LVGL");
        abort();
    }
    bsp_display_rotate(display, LV_DISPLAY_ROTATION_90);
    create_ui(display);
    lv_obj_invalidate(lv_display_get_screen_active(display));
    lv_refr_now(display);
    bsp_display_unlock();
    ESP_LOGI(TAG, "Bird Remote UI created and refreshed");

    initialize_wifi();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
