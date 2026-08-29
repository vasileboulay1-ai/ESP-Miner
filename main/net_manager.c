#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "net_manager.h"
#include "nvs_config.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "iot_usbh_ecm.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_cdc.h"
#include "usb/usb_host.h"
#define NET_ETH_SUPPORTED 1
#else
#define NET_ETH_SUPPORTED 0
#endif

static const char *TAG = "net";

// Table d'identification (affichage seulement) : le firmware accepte N'IMPORTE QUEL
// adaptateur et lit son VID/PID reel. Cette table sert uniquement a nommer le chipset.
typedef struct { uint16_t vid; uint16_t pid; const char *name; } ecm_known_t;
static const ecm_known_t s_known[] = {
    { 0x1A86, 0x5397, "CH397A"   },
    { 0x0BDA, 0x8152, "RTL8152B" },
    { 0x0BDA, 0x8153, "RTL8153"  },
    { 0x0B95, 0x7720, "AX88772"  },
    { 0x0B95, 0x1790, "AX88179"  },
    { 0x2C7C, 0x0125, "NX7202D"  },
};
#define ECM_KNOWN_COUNT (sizeof(s_known) / sizeof(s_known[0]))

static bool s_enabled   = false;
static bool s_link_up   = false;
static bool s_has_ip    = false;
static char s_ip[16]    = "";
static char s_gw[16]    = "";
static char s_dns[16]   = "";
static char s_mac[18]   = "";
static char s_chipset[24] = "";

#if NET_ETH_SUPPORTED
static iot_eth_driver_t *s_eth_driver = NULL;
static esp_netif_t      *s_eth_netif  = NULL;


// Lit le VID/PID REEL de l'adaptateur branche (aucune supposition de chipset).
static void log_adapter_identity(void)
{
    usbh_cdc_port_handle_t port = usb_ecm_get_cdc_port_handle(s_eth_driver);
    if (port == NULL) return;
    usb_device_handle_t dev = NULL;
    if (usbh_cdc_get_dev_handle(port, &dev) != ESP_OK || dev == NULL) return;
    usb_device_info_t info;
    if (usb_host_device_info(dev, &info) != ESP_OK) return;
    const usb_device_desc_t *d = NULL;
    if (usb_host_get_device_descriptor(dev, &d) != ESP_OK || d == NULL) return;

    const char *name = "inconnu";
    for (size_t i = 0; i < sizeof(s_known) / sizeof(s_known[0]); i++) {
        if (s_known[i].vid == d->idVendor && s_known[i].pid == d->idProduct) {
            name = s_known[i].name;
            break;
        }
    }
    snprintf(s_chipset, sizeof(s_chipset), "%s", name);
    ESP_LOGI(TAG, "[USB] Ethernet adapter detected");
    ESP_LOGI(TAG, "[USB] VID:%04X PID:%04X class:%02X (%s)",
             d->idVendor, d->idProduct, d->bDeviceClass, name);
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
        case IOT_ETH_EVENT_START:
            ESP_LOGI(TAG, "[ETH] USB Ethernet adapter detected");
            break;
        case IOT_ETH_EVENT_CONNECTED:
            s_link_up = true;
            log_adapter_identity();
            ESP_LOGI(TAG, "[ETH] Link UP");
            break;
        case IOT_ETH_EVENT_DISCONNECTED:
            s_link_up = false;
            s_has_ip  = false;
            s_ip[0] = '\0';
            ESP_LOGW(TAG, "[ETH] Link DOWN");
            ESP_LOGI(TAG, "[NET] Falling back to Wi-Fi");
            break;
        case IOT_ETH_EVENT_STOP:
            s_link_up = false;
            s_has_ip  = false;
            ESP_LOGW(TAG, "[ETH] Adapter removed");
            break;
        default:
            break;
    }
}

static void on_eth_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *) data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    snprintf(s_gw, sizeof(s_gw), IPSTR, IP2STR(&e->ip_info.gw));
    s_has_ip = true;

    uint8_t mac[6] = {0};
    if (esp_netif_get_mac(s_eth_netif, mac) == ESP_OK) {
        snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        snprintf(s_dns, sizeof(s_dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
    ESP_LOGI(TAG, "[ETH] IP: %s (gw %s, dns %s)", s_ip, s_gw, s_dns);
    ESP_LOGI(TAG, "[NET] Active interface: Ethernet");
}
#endif // NET_ETH_SUPPORTED

void net_manager_init(void)
{
    s_enabled = nvs_config_get_bool(NVS_CONFIG_ETH_ENABLE);
    if (!s_enabled) {
        // Comportement historique strictement inchange : USB-Serial/JTAG conserve.
        return;
    }

#if !NET_ETH_SUPPORTED
    ESP_LOGW(TAG, "[ETH] USB Ethernet non supporte sur cette cible");
#else
    ESP_LOGI(TAG, "[ETH] Initialisation de l'Ethernet USB (mode host)");

    // Detection GENERIQUE : on accepte tout peripherique USB et on lit son VID/PID.
    // C'est le driver ECM qui verifiera ensuite s'il expose bien une interface CDC-ECM.
    iot_usbh_ecm_config_t ecm_cfg = { .match_id_list = ESP_USB_DEVICE_MATCH_ID_ANY };
    if (iot_eth_new_usb_ecm(&ecm_cfg, &s_eth_driver) != ESP_OK || s_eth_driver == NULL) {
        ESP_LOGE(TAG, "[ETH] Creation du driver ECM impossible");
        return;
    }

    iot_eth_handle_t eth_handle = NULL;
    iot_eth_config_t eth_cfg = { .driver = s_eth_driver, .stack_input = NULL };
    if (iot_eth_install(&eth_cfg, &eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "[ETH] Installation du driver impossible");
        return;
    }

    // Priorite de route : c'est esp_netif qui choisit tout seul l'interface active
    // (la plus prioritaire qui a une IP). Wi-Fi STA vaut 100 par defaut.
    net_priority_t prio = (net_priority_t) nvs_config_get_u16(NVS_CONFIG_NET_PRIORITY);
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.if_key     = "ETH_USB";
    base_cfg.if_desc    = "usb-eth";
    base_cfg.route_prio = (prio == NET_PRIORITY_WIFI) ? 50 : 200;

    esp_netif_config_t netif_cfg = {
        .base   = &base_cfg,
        .driver = NULL,
        .stack  = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (s_eth_netif == NULL) {
        ESP_LOGE(TAG, "[ETH] Creation de l'interface reseau impossible");
        return;
    }

    iot_eth_netif_glue_handle_t glue = iot_eth_new_netif_glue(eth_handle);
    if (glue == NULL) {
        ESP_LOGE(TAG, "[ETH] netif glue impossible");
        return;
    }
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, glue));

    ESP_ERROR_CHECK(esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_eth_got_ip, NULL));

    if (iot_eth_start(eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "[ETH] Demarrage impossible");
        return;
    }
    ESP_LOGI(TAG, "[ETH] En attente d'un adaptateur CDC-ECM (priorite route %d)", base_cfg.route_prio);
#endif
}

bool net_manager_eth_enabled(void) { return s_enabled; }
bool net_manager_eth_link_up(void) { return s_link_up; }
bool net_manager_eth_has_ip(void)  { return s_has_ip; }
const char *net_manager_eth_ip(void)   { return s_ip; }
const char *net_manager_eth_gw(void)   { return s_gw; }
const char *net_manager_eth_dns(void)  { return s_dns; }
const char *net_manager_eth_mac(void)  { return s_mac; }
const char *net_manager_chipset(void)  { return s_chipset; }

const char *net_manager_active(void)
{
    if (s_enabled && s_has_ip) return "Ethernet";
    return "Wi-Fi";
}
