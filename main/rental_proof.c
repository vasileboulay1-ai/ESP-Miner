#include <string.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "rental_proof.h"
#include "utils.h"

static const char *TAG = "rental_proof";

rental_proof_t rental_proof = {
    .rental_active = false,
    .block_state = BLOCK_STATE_NONE,
    .fee_percent = 2.00f,
};

static int64_t now_s(void)
{
    time_t t = time(NULL);
    // Avant synchro NTP, time() est proche de 0 : on retombe sur l'horloge monotone.
    if (t < 1600000000) {
        return (int64_t) (esp_timer_get_time() / 1000000);
    }
    return (int64_t) t;
}

// Un hote MRR => la machine travaille pour un locataire.
static bool host_is_mrr(const char *url)
{
    return url && strstr(url, "miningrigrentals") != NULL;
}

void rental_proof_update_mode(const char *url, const char *user)
{
#if !RENTAL_PROOF_ENABLE
    (void) url; (void) user;
    return;
#else
    bool active = host_is_mrr(url);

    if (active && !rental_proof.rental_active) {
        // Entree en location : on ouvre une session.
        rental_proof.rental_active = true;
        rental_proof.rental_start_s = now_s();
        rental_proof.shares_submitted = 0;
        rental_proof.best_share_diff = 0.0;
        ESP_LOGI(TAG, "[MRR] Rental detected - pool %s worker %s", url ? url : "?", user ? user : "?");
        ESP_LOGI(TAG, "[MRR] Rental mining active (success fee theorique %.2f %%)", rental_proof.fee_percent);
    } else if (!active && rental_proof.rental_active) {
        ESP_LOGI(TAG, "[MRR] Rental ended (%lld s, %" PRIu32 " shares)",
                 (long long) (now_s() - rental_proof.rental_start_s), rental_proof.shares_submitted);
        rental_proof.rental_active = false;
    }

    if (url) { strncpy(rental_proof.pool, url, sizeof(rental_proof.pool) - 1); }
    if (user) { strncpy(rental_proof.worker, user, sizeof(rental_proof.worker) - 1); }
#endif
}

// Reconstruit l'en-tete 80 octets EXACTEMENT comme test_nonce_value(), puis
// double-SHA256 => hash de bloc. On l'affiche en big-endian (convention explorateurs).
static void compute_block_hash(const bm_job *job, uint32_t nonce, uint32_t rolled_version, char out_hex[65])
{
    uint8_t header[80];
    memcpy(header, &rolled_version, 4);
    reverse_32bit_words(job->prev_block_hash, header + 4);
    reverse_32bit_words(job->merkle_root, header + 36);
    memcpy(header + 68, &job->ntime, 4);
    memcpy(header + 72, &job->target, 4);
    memcpy(header + 76, &nonce, 4);

    uint8_t hash[32];
    double_sha256_bin(header, 80, hash);

    // little-endian interne -> big-endian d'affichage
    for (int i = 0; i < 32; i++) {
        snprintf(out_hex + (i * 2), 3, "%02x", hash[31 - i]);
    }
    out_hex[64] = '\0';
}

void rental_proof_on_result(const bm_job *job, uint32_t nonce, uint32_t rolled_version,
                            double share_diff, bool submitted)
{
#if !RENTAL_PROOF_ENABLE
    (void) job; (void) nonce; (void) rolled_version; (void) share_diff; (void) submitted;
    return;
#else
    if (job == NULL) {
        return;
    }

    double net_diff = networkDifficulty(job->target);
    rental_proof.network_diff = net_diff;

    if (submitted) {
        rental_proof.shares_submitted++;
    }
    if (share_diff > rental_proof.best_share_diff) {
        rental_proof.best_share_diff = share_diff;
    }

    // BLOCK CANDIDATE : hash <= target RESEAU (et pas seulement target du pool).
    if (net_diff <= 0.0 || share_diff < net_diff) {
        return;
    }

    rental_proof.candidates++;
    rental_proof.cand_nonce = nonce;
    rental_proof.cand_version = rolled_version;
    rental_proof.cand_ntime = job->ntime;
    rental_proof.cand_share_diff = share_diff;
    rental_proof.cand_network_diff = net_diff;
    rental_proof.cand_time_s = now_s();
    rental_proof.cand_during_rental = rental_proof.rental_active;
    rental_proof.cand_rental_elapsed_s = rental_proof.rental_active
            ? (rental_proof.cand_time_s - rental_proof.rental_start_s) : 0;

    if (job->jobid) {
        strncpy(rental_proof.cand_jobid, job->jobid, sizeof(rental_proof.cand_jobid) - 1);
    }
    if (job->extranonce2) {
        strncpy(rental_proof.cand_extranonce2, job->extranonce2, sizeof(rental_proof.cand_extranonce2) - 1);
    }

    compute_block_hash(job, nonce, rolled_version, rental_proof.block_hash);
    rental_proof.block_state = submitted ? BLOCK_STATE_SUBMITTED : BLOCK_STATE_CANDIDATE;

    ESP_LOGI(TAG, "[BLOCK] Network target reached (share %.0f >= network %.0f)", share_diff, net_diff);
    ESP_LOGI(TAG, "[BLOCK] Candidate hash %s", rental_proof.block_hash);
    ESP_LOGI(TAG, "[BLOCK] job=%s extranonce2=%s nonce=%08" PRIX32 " ntime=%" PRIu32 " version=%08" PRIX32,
             rental_proof.cand_jobid, rental_proof.cand_extranonce2,
             nonce, job->ntime, rolled_version);
    if (submitted) {
        ESP_LOGI(TAG, "[BLOCK] Candidate submitted to pool");
    }

    if (rental_proof.cand_during_rental) {
        ESP_LOGI(TAG, "[SUCCESS FEE] Eligible - trouve pendant une location MRR (t+%lld s)",
                 (long long) rental_proof.cand_rental_elapsed_s);
        ESP_LOGI(TAG, "[SUCCESS FEE] Owner share theorique %.2f %% - Settlement: MANUAL",
                 rental_proof.fee_percent);
    } else {
        ESP_LOGI(TAG, "[SUCCESS FEE] Non applicable (hors location, bloc 100 %% proprietaire)");
    }
    ESP_LOGW(TAG, "[BLOCK] Etat = CANDIDAT : confirmation a verifier on-chain (hash ci-dessus)");
#endif
}

const char *rental_proof_state_str(void)
{
    switch (rental_proof.block_state) {
        case BLOCK_STATE_CANDIDATE: return "Candidate";
        case BLOCK_STATE_SUBMITTED: return "Submitted";
        case BLOCK_STATE_ACCEPTED:  return "Accepted";
        case BLOCK_STATE_CONFIRMED: return "Confirmed";
        case BLOCK_STATE_REJECTED:  return "Rejected";
        default:                    return "None";
    }
}

double rental_proof_owner_share(double reward_btc)
{
    return reward_btc * (double) rental_proof.fee_percent / 100.0;
}
