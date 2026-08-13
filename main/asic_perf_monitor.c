#include "asic_perf_monitor.h"
#include "esp_timer.h"

asic_perf_stats_t asic_perf_stats = {0};

#if ASIC_PERF_ENABLE

// Etat interne (lock-free : ces mesures sont diagnostiques, une micro-course est benigne
// et lissee par l'EWMA ; on evite tout mutex sur le chemin critique du mining).
static int64_t s_notify_us = 0;          // T0 du dernier clean_jobs (nouveau bloc)
static bool    s_newblock_pending = false;
static int64_t s_build_start_us = 0;

void asic_perf_notify(bool clean_jobs)
{
    if (clean_jobs) {
        s_notify_us = esp_timer_get_time();
        s_newblock_pending = true;
    }
}

void asic_perf_build_start(void)
{
    s_build_start_us = esp_timer_get_time();
}

void asic_perf_work_sent(void)
{
    int64_t now = esp_timer_get_time();

    // Duree de preparation du job (CPU ESP32 : coinbase/merkle/midstate + envoi)
    if (s_build_start_us != 0) {
        float ms = (float)(now - s_build_start_us) / 1000.0f;
        if (ms >= 0.0f && ms < 1000.0f) {
            asic_perf_stats.job_build_ms = asic_perf_stats.job_build_ms * 0.9f + ms * 0.1f;
            if (ms > asic_perf_stats.job_build_max_ms) {
                asic_perf_stats.job_build_max_ms = ms;
            }
        }
        s_build_start_us = 0;
    }
    asic_perf_stats.jobs_sent++;

    // Reactivite nouveau bloc : temps entre la reception du clean_jobs et l'envoi du 1er job
    if (s_newblock_pending) {
        s_newblock_pending = false;
        float ms = (float)(now - s_notify_us) / 1000.0f;
        if (ms >= 0.0f && ms < 5000.0f) {
            asic_perf_stats.newblock_react_ms = asic_perf_stats.newblock_react_ms * 0.7f + ms * 0.3f;
            if (ms > asic_perf_stats.newblock_react_max_ms) {
                asic_perf_stats.newblock_react_max_ms = ms;
            }
            asic_perf_stats.newblock_count++;
        }
    }
}

void asic_perf_dup_filtered(void)
{
    asic_perf_stats.dup_filtered++;
}

#else  // ASIC_PERF_ENABLE == 0 : no-op complets (comportement standard)

void asic_perf_notify(bool clean_jobs) { (void) clean_jobs; }
void asic_perf_build_start(void) {}
void asic_perf_work_sent(void) {}
void asic_perf_dup_filtered(void) {}

#endif
