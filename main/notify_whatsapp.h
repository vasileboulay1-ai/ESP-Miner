#ifndef MAIN_NOTIFY_WHATSAPP_H
#define MAIN_NOTIFY_WHATSAPP_H

// v7 : envoie un message WhatsApp via l'API CallMeBot (numero + cle lus depuis la NVS).
// Non bloquant : cree une tache one-shot (les evenements bloc/record sont rares) afin de
// ne jamais bloquer le chemin de minage. Ignore silencieusement si non configure.
void notify_whatsapp_send(const char *msg);

#endif // MAIN_NOTIFY_WHATSAPP_H
