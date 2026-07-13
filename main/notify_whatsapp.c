#include "notify_whatsapp.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "nvs_config.h"

static const char *TAG = "notify_wa";

// Encodage URL (pourcentage) de 'src' vers 'dst' (taille dst incluant le '\0').
static void url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;
    for (size_t i = 0; src[i] != '\0' && di + 4 < dst_size; i++) {
        unsigned char c = (unsigned char) src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char) c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[(c >> 4) & 0xF];
            dst[di++] = hex[c & 0xF];
        }
    }
    dst[di] = '\0';
}

// Tache one-shot : lit la config NVS, envoie le message via CallMeBot, se supprime.
static void notify_task(void *pvParameters)
{
    char *msg = (char *) pvParameters;

    char *phone = nvs_config_get_string(NVS_CONFIG_WA_PHONE);
    char *apikey = nvs_config_get_string(NVS_CONFIG_WA_APIKEY);

    if (phone && apikey && strlen(phone) > 0 && strlen(apikey) > 0) {
        char enc_phone[64];
        char enc_key[96];
        char enc_text[512];
        url_encode(phone, enc_phone, sizeof(enc_phone));
        url_encode(apikey, enc_key, sizeof(enc_key));
        url_encode(msg, enc_text, sizeof(enc_text));

        char url[768];
        snprintf(url, sizeof(url),
                 "https://api.callmebot.com/whatsapp.php?phone=%s&text=%s&apikey=%s",
                 enc_phone, enc_text, enc_key);

        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = 12000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client != NULL) {
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Notification WhatsApp envoyee (HTTP %d)",
                         esp_http_client_get_status_code(client));
            } else {
                ESP_LOGW(TAG, "Echec envoi WhatsApp: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
        } else {
            ESP_LOGW(TAG, "Impossible d'initialiser le client HTTP");
        }
    } else {
        ESP_LOGW(TAG, "WhatsApp non configure (numero/cle vides) - notification ignoree");
    }

    if (phone) free(phone);
    if (apikey) free(apikey);
    free(msg);
    vTaskDelete(NULL);
}

void notify_whatsapp_send(const char *msg)
{
    if (msg == NULL) {
        return;
    }
    char *copy = strdup(msg);
    if (copy == NULL) {
        return;
    }
    // Evenements rares (bloc/record) -> une tache dediee (la pile TLS est gourmande).
    if (xTaskCreate(notify_task, "wa_notify", 12288, copy, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Impossible de creer la tache de notification WhatsApp");
        free(copy);
    }
}
