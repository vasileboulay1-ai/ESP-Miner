#ifndef RENTAL_PROOF_H_
#define RENTAL_PROOF_H_

#include <stdbool.h>
#include <stdint.h>
#include "mining.h"

// Systeme de PREUVE de bloc en location (MiningRigRentals).
//
// Ce module NE PRELEVE RIEN et ne modifie AUCUN job : il OBSERVE.
// Etude de faisabilite : le prelevement automatique des 2 % est IMPOSSIBLE depuis
// le Bitaxe (la coinbase - donc l'adresse de paiement - est construite par le pool
// du locataire, et la recompense arrive sur SON compte). On construit donc
// uniquement ce qui est reel : la PREUVE qu'un bloc a ete produit par cette
// machine pendant une location, + le calcul THEORIQUE de la commission.
//
// Contraintes respectees :
//  - jamais appele avant mining.submit (aucun retard de soumission)
//  - aucune allocation dynamique
//  - aucune securite thermique/electrique touchee
#define RENTAL_PROOF_ENABLE 1

typedef enum {
    BLOCK_STATE_NONE = 0,
    BLOCK_STATE_CANDIDATE,   // hash <= network target, detecte localement
    BLOCK_STATE_SUBMITTED,   // envoye au pool
    BLOCK_STATE_ACCEPTED,    // le pool a accepte la soumission
    BLOCK_STATE_CONFIRMED,   // bloc retrouve sur la blockchain (verif externe)
    BLOCK_STATE_REJECTED
} rental_block_state_t;

typedef struct {
    // --- mode de minage ---
    bool     rental_active;          // true = job venant d'une location MRR
    int64_t  rental_start_s;         // unix, debut de la session de location
    char     pool[80];
    char     worker[80];

    // --- session ---
    uint32_t shares_submitted;
    double   best_share_diff;
    double   network_diff;

    // --- block candidate ---
    rental_block_state_t block_state;
    uint32_t candidates;             // nombre de block candidates vus
    char     block_hash[65];         // hash du bloc (hex, big-endian d'affichage)
    char     cand_jobid[24];
    char     cand_extranonce2[24];
    uint32_t cand_nonce;
    uint32_t cand_ntime;
    uint32_t cand_version;           // version roulee effective
    double   cand_share_diff;
    double   cand_network_diff;
    int64_t  cand_time_s;
    int64_t  cand_rental_elapsed_s;  // temps ecoule depuis le debut de la location
    bool     cand_during_rental;

    // --- commission (theorique uniquement) ---
    float    fee_percent;            // defaut 2.00
} rental_proof_t;

extern rental_proof_t rental_proof;

// Met a jour le mode (normal / location MRR) depuis la config stratum courante.
void rental_proof_update_mode(const char *url, const char *user);

// Appele APRES mining.submit, pour chaque resultat ASIC valide.
// Detecte le block candidate et calcule le hash de bloc reel (preuve).
void rental_proof_on_result(const bm_job *job, uint32_t nonce, uint32_t rolled_version,
                            double share_diff, bool submitted);

// Etat du bloc en texte (pour l'API / l'UI).
const char *rental_proof_state_str(void);

// Part proprietaire THEORIQUE pour une recompense donnee (aucun paiement effectue).
double rental_proof_owner_share(double reward_btc);

#endif // RENTAL_PROOF_H_
