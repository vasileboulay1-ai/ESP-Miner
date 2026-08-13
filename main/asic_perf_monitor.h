#ifndef MAIN_ASIC_PERF_MONITOR_H
#define MAIN_ASIC_PERF_MONITOR_H

#include <stdint.h>
#include <stdbool.h>

// Instrumentation performance du chemin de mining (Phase 1 - mesure).
// Feature flag : 1 = mesures actives (surcout ~microsecondes, non bloquant, AUCUNE securite touchee) ;
//                0 = comportement AxeOS standard (les hooks deviennent des no-op).
#define ASIC_PERF_ENABLE 1

typedef struct {
    // Reactivite "nouveau bloc" : mining.notify (clean_jobs) recue -> 1er job envoye a l'ASIC (ms)
    float    newblock_react_ms;       // moyenne glissante (EWMA)
    float    newblock_react_max_ms;
    uint32_t newblock_count;

    // Cout CPU de preparation d'un job (generate_work + envoi UART) (ms) - detecte un ESP32 goulot
    float    job_build_ms;            // EWMA
    float    job_build_max_ms;
    uint32_t jobs_sent;

    // Travail re-balaye : doublons filtres avant le pool = proxy du gaspillage de balayage
    uint64_t dup_filtered;
} asic_perf_stats_t;

extern asic_perf_stats_t asic_perf_stats;

void asic_perf_notify(bool clean_jobs);   // stratum : une mining.notify vient d'arriver (T0)
void asic_perf_build_start(void);          // create_jobs : debut de preparation d'un job
void asic_perf_work_sent(void);            // create_jobs : job envoye a l'ASIC (T4 / fin de build)
void asic_perf_dup_filtered(void);         // asic_result : un doublon vient d'etre filtre

#endif // MAIN_ASIC_PERF_MONITOR_H
