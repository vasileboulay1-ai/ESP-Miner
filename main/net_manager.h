#ifndef NET_MANAGER_H_
#define NET_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

// Gestionnaire d'interface reseau : Ethernet USB (adaptateur CDC-ECM sur le port
// USB-C, mode host) + Wi-Fi, avec priorite automatique et bascule sans redemarrage.
//
// IMPORTANT - materiel requis :
//   1) un adaptateur USB->RJ45 de classe CDC-ECM (CH397A, RTL8152B, NX7202D testes
//      par Espressif). Les puces proprietaires (RTL8153, AX88179) NE fonctionnent PAS.
//   2) un hub USB alimente : le port USB-C du Bitaxe ne fournit pas le 5 V.
//   3) le Bitaxe doit etre alimente par son jack d'alimentation separe.
//
// L'Ethernet est DESACTIVE par defaut (NVS "ethEnable" = false) : tant qu'il n'est
// pas active, le comportement est strictement identique a avant et le port USB-C
// reste en USB-Serial/JTAG (flashage USB conserve). Une fois active, le mode host
// prend GPIO19/20 -> les mises a jour se font en OTA.
//
// La bascule s'appuie sur les priorites de route natives d'esp_netif : l'interface
// active la plus prioritaire ET disposant d'une IP est choisie automatiquement par
// la pile TCP/IP. Aucune machine a etats maison, aucune socket geree a la main.

typedef enum {
    NET_PRIORITY_AUTO = 0,   // Ethernet prioritaire, Wi-Fi en secours (defaut)
    NET_PRIORITY_ETH  = 1,   // Ethernet prioritaire
    NET_PRIORITY_WIFI = 2,   // Wi-Fi prioritaire
} net_priority_t;

// A appeler une fois au demarrage, APRES esp_netif_init() et la boucle d'evenements.
// Ne fait rien (retourne ESP_OK) si l'Ethernet USB n'est pas active en NVS.
void net_manager_init(void);

// Etat pour l'API / l'interface AxeOS.
bool        net_manager_eth_enabled(void);   // fonction activee en NVS
bool        net_manager_eth_link_up(void);   // cable branche + lien etabli
bool        net_manager_eth_has_ip(void);    // DHCP obtenu
const char *net_manager_active(void);        // "Ethernet" | "Wi-Fi" | "None"
const char *net_manager_eth_ip(void);
const char *net_manager_eth_gw(void);
const char *net_manager_eth_mac(void);
const char *net_manager_chipset(void);       // chipset detecte (ou "")

#endif // NET_MANAGER_H_
