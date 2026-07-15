#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "nvs_config.h"
#include "nvs.h"
#include "vcore.h"
#include "thermal.h"
#include "power.h"
#include "asic.h"
#include "utils.h"
#include "asic_init.h"
#include "asic_reset.h"
#include "driver/uart.h"

#define POLL_RATE 100
#define MAX_TEMP 90.0
#define THROTTLE_TEMP 75.0
#define SAFE_TEMP 45.0

#define VOLTAGE_START_THROTTLE 4900
#define VOLTAGE_MIN_THROTTLE 3500
#define VOLTAGE_RANGE (VOLTAGE_START_THROTTLE - VOLTAGE_MIN_THROTTLE)

#define TPS546_THROTTLE_TEMP 105.0
#define TPS546_MAX_TEMP 145.0

#define ASIC_REDUCTION 100.0

// ---- Gouverneur thermique de frequence (ajuste la frequence selon la temp ASIC) ----
#define GOV_TEMP_HIGH       64.0f   // au-dessus : on baisse la frequence (v5 : 63->64, zone morte elargie)
#define GOV_TEMP_LOW        60.0f   // en dessous (ET puissance OK) : on remonte (v5 : 61->60, anti-oscillation)
#define GOV_TEMP_EMERGENCY  65.0f   // plafond dur temperature : baisse renforcee
#define GOV_POWER_HIGH      29.3f   // au-dessus : on baisse la frequence (Watts)
#define GOV_POWER_LOW       28.3f   // en dessous (ET temp OK) : on remonte (Watts)
#define GOV_POWER_EMERGENCY 29.9f   // plafond dur puissance : baisse renforcee (Watts)
#define GOV_FREQ_MIN        400.0f  // frequence plancher (MHz)
#define GOV_FREQ_STEP       25.0f   // pas d'ajustement (MHz)
#define GOV_INTERVAL_CYCLES 100     // 100 x POLL_RATE(100ms) = ajuste toutes les ~10s
// ---- v8 : maintien du 5V d'entree (cape la freq pour eviter l'affaissement de l'input voltage) ----
#define GOV_VIN_ENABLE      0        // v8 abandonne : NE PAS caper la freq (perte de perf). On garde juste le log vin.
#define GOV_VIN_MIN         4970.0f  // mV : input sous ce seuil -> baisse la freq ET memorise ce plafond

// ---- v4 : auto-tuner de tension (apprend la tension mini stable par frequence) ----
// Perturbe-et-observe : on grignote la tension vers le bas ; si le taux d'erreur ASIC
// monte, on remonte franchement et on verrouille ce plancher. Auto-calibration sans PC.
#define TUNE_ENABLE         1        // 1 = apprentissage actif ; 0 = table "propre" v3 figee
#define TUNE_BUCKETS        25       // paliers de frequence memorisables (400..1000 MHz par 25)
#define TUNE_ERR_GOOD       1.0f     // taux d'erreur ASIC <= : marge -> on tente de baisser (%)
#define TUNE_ERR_BAD        2.0f     // taux d'erreur ASIC >= : instable -> on remonte + verrouille (%)
#define TUNE_STEP_DOWN      5        // pas de descente prudent (mV)
#define TUNE_STEP_UP        20       // pas de remontee franc = securite (mV)
#define TUNE_MARGIN         10       // marge conservee au-dessus du plancher trouve (mV)
#define TUNE_VOLT_FLOOR     1000     // tension mini absolue, garde-fou dur (mV)
#define TUNE_MAX_UNDERVOLT  70       // on ne descend jamais plus de 70 mV sous la table "propre" (mV)
#define TUNE_SETTLE_CYCLES  300      // 300 x 100ms = ~30s de stabilisation avant chaque decision
// ---- v5 : anti-oscillation + persistance ----
#define TUNE_SEED_MAX_DIST  3        // amorce un nouveau palier depuis un voisin appris a <=3 paliers (75 MHz)
#define TUNE_SAVE_INTERVAL_CYCLES 18000 // 18000 x 100ms = ~30 min : throttle des ecritures NVS (usure flash)
#define GOV_NVS_NAMESPACE   "governor"  // namespace NVS isole (n'interfere pas avec "main")
#define GOV_NVS_KEY         "vtable"     // blob = [learned[25] , floor[25]] en uint16_t
// ---- v6 : Gardien anti-abandon (chien de garde hashrate) ----
#define GUARD_ENABLE        1           // 1 = surveille et relance un ASIC fige
#define GUARD_MIN_HASHRATE  50.0f       // GH/s : en dessous (alors qu'on devrait miner) = fige
#define GUARD_STALL_CYCLES  1800        // 1800 x 100ms = 180s de hashrate ~0 avant d'agir
#define GUARD_GRACE_CYCLES  1200        // 1200 x 100ms = 120s de grace apres (re)demarrage (ramp)
#define GUARD_VOLT_BUMP     20          // mV remontes a la recuperation (anti-lockup par sous-voltage)
#define GUARD_MAX_SOFT      3           // apres 3 relances soft echouees -> redemarrage complet

static const char * TAG = "power_management";

// V3 : tension "propre" (mV) pour une frequence donnee (auto-undervolt par palier).
// Table lineaire calee sur nos mesures : 740 MHz -> 1185 mV, 900 MHz -> ~1205 mV.
// A affiner apres flash en observant le taux d'erreur. Borne dure a 1240 mV.
static float gov_voltage_for_freq(float freq)
{
    float v = 1185.0f + (freq - 740.0f) * 0.125f;
    if (v < 1120.0f) v = 1120.0f;
    if (v > 1240.0f) v = 1240.0f;
    return v;
}

// v4 : convertit une frequence (MHz) en index de palier pour la table apprise.
static int gov_bucket(float freq)
{
    int b = (int)((freq - GOV_FREQ_MIN) / GOV_FREQ_STEP + 0.5f);
    if (b < 0) b = 0;
    if (b >= TUNE_BUCKETS) b = TUNE_BUCKETS - 1;
    return b;
}

// v5 : amorce un nouveau palier a partir du voisin appris le plus proche.
// On reporte le MEME undervolt (baseline - appris) que le voisin, applique a la baseline
// du palier courant. Evite de repartir de la baseline haute -> supprime l'oscillation du
// gouverneur (les deux paliers voisins tournent alors "frais"). Borne comme le tuner.
static uint16_t gov_seed_voltage(int b, uint16_t baseline_b, const uint16_t *learned)
{
    for (int d = 1; d <= TUNE_SEED_MAX_DIST; d++) {
        for (int s = -1; s <= 1; s += 2) {
            int n = b + s * d;
            if (n < 0 || n >= TUNE_BUCKETS) continue;
            if (learned[n] == 0) continue;                 // voisin pas encore appris
            float base_n = gov_voltage_for_freq(GOV_FREQ_MIN + n * GOV_FREQ_STEP);
            int offset = (int) base_n - (int) learned[n];  // undervolt appris par le voisin
            if (offset < 0) offset = 0;
            int seed = (int) baseline_b - offset;
            int soft = (int) baseline_b - TUNE_MAX_UNDERVOLT;
            int lo = (TUNE_VOLT_FLOOR > soft) ? TUNE_VOLT_FLOOR : soft;
            if (seed < lo) seed = lo;
            if (seed > (int) baseline_b) seed = baseline_b;
            return (uint16_t) seed;
        }
    }
    return baseline_b;                                      // aucun voisin appris -> baseline sure
}

// v5/v6 : ecrit la table apprise (learned+floor) dans la NVS. Utilise par la sauvegarde
// throttlee du tuner ET par le Gardien (sauvegarde immediate avant une relance/redemarrage).
static void gov_save_vtable_nvs(const uint16_t *learned, const uint16_t *floor)
{
    uint16_t blob[TUNE_BUCKETS * 2];
    for (int i = 0; i < TUNE_BUCKETS; i++) {
        blob[i] = learned[i];
        blob[TUNE_BUCKETS + i] = floor[i];
    }
    nvs_handle_t nh;
    if (nvs_open(GOV_NVS_NAMESPACE, NVS_READWRITE, &nh) == ESP_OK) {
        if (nvs_set_blob(nh, GOV_NVS_KEY, blob, sizeof(blob)) == ESP_OK) {
            nvs_commit(nh);
            ESP_LOGI(TAG, "[TUNER] table de tension sauvegardee en NVS");
        }
        nvs_close(nh);
    }
}

static void mining_stop(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Stopping mining");

    // Wind frequency down to 50 MHz before cutting power. This also updates
    // the transition tracker so the ramp starts from 50 MHz on next start,
    // rather than the stale pre-reset frequency.
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value = 50;
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate = 0;

    ASIC_set_frequency(GLOBAL_STATE);
    ASIC_set_nonce_space(GLOBAL_STATE);

    // Cut ASIC power and hold in reset
    VCORE_set_voltage(GLOBAL_STATE, 0.0f);
    asic_hold_reset_low();

    // Mark uninitialized immediately so tasks stop issuing UART commands
    GLOBAL_STATE->ASIC_initalized = false;

    // Give tasks time to complete any in-progress UART operation
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Flush any stale data from the UART buffers
    uart_flush(UART_NUM_1);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "Mining stopped");
}

static uint8_t mining_start(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Starting mining");

    // Restore voltage from NVS
    uint16_t voltage = nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
    VCORE_set_voltage(GLOBAL_STATE, (double) voltage / 1000.0);

    // Wait for voltage to stabilize before touching the ASIC
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Clear any accumulated UART garbage before init
    uart_flush(UART_NUM_1);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    POWER_MANAGEMENT_init_frequency(GLOBAL_STATE);
    // Stabilization delay of 2000ms prevents race conditions where tasks are
    // just starting to use the ASIC while power management tries to change frequency
    uint8_t chip_count = asic_initialize(GLOBAL_STATE, ASIC_INIT_RECOVERY, 2000);

    if (chip_count > 0) {
        ESP_LOGI(TAG, "Mining started successfully (%d chip(s))", chip_count);
    } else {
        ESP_LOGE(TAG, "Mining start failed - ASIC not detected");
    }

    return chip_count;
}

static float expected_hashrate(GlobalState * GLOBAL_STATE)
{
    return GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value * GLOBAL_STATE->DEVICE_CONFIG.family.asic.small_core_count * GLOBAL_STATE->DEVICE_CONFIG.family.asic_count / 1000.0;
}

void POWER_MANAGEMENT_init_frequency(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    float frequency = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);

    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value = frequency;
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency = 50.0;
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate = expected_hashrate(GLOBAL_STATE);
    
    char expected_hashrate_str[16] = {0};
    suffixString(GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate * 1e6, expected_hashrate_str, sizeof(expected_hashrate_str), 0);
    ESP_LOGI(TAG, "ASIC Frequency: %g MHz, Expected hashrate: %sH/s", frequency, expected_hashrate_str);
}

void POWER_MANAGEMENT_task(void * pvParameters)
{
    ESP_LOGI(TAG, "Starting");

    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    SystemModule * sys_module = &GLOBAL_STATE->SYSTEM_MODULE;

    POWER_MANAGEMENT_init_frequency(GLOBAL_STATE);
    
    float last_asic_frequency = power_management->frequency_value;

    vTaskDelay(500 / portTICK_PERIOD_MS);
    uint16_t last_core_voltage = 0.0;

    uint16_t last_known_asic_voltage = 0;
    float last_known_asic_frequency = 0.0;
    bool is_paused = false;

    // Gouverneur thermique : le plafond = la frequence NVS voulue par l'utilisateur ;
    // la frequence effective est ajustee selon la temperature ASIC.
    float gov_target_freq = power_management->frequency_value;
    float gov_effective_freq = power_management->frequency_value;
    int gov_counter = 0;
    float gov_vin_ceiling = 100000.0f;  // v8 : plafond de freq impose par l'input voltage (aucun cap au depart)

    // v4 : tension APPRISE par palier de frequence (0 = pas encore appris -> table "propre").
    static uint16_t gov_learned_mv[TUNE_BUCKETS] = {0};
    static uint16_t gov_floor_mv[TUNE_BUCKETS] = {0}; // tension connue comme instable (0 = inconnu)
    int tune_settle = TUNE_SETTLE_CYCLES;             // temps de stabilisation restant
    int last_bucket = -1;
    int tune_save_counter = 0;                        // v5 : compteur pour throttler les ecritures NVS
    bool tune_dirty = false;                          // v5 : table modifiee depuis la derniere sauvegarde ?
    int stall_counter = 0;                            // v6 : cycles avec hashrate ~0 (alors qu'on devrait miner)
    int stall_grace = GUARD_GRACE_CYCLES;             // v6 : grace apres (re)demarrage (le temps du ramp)
    int stall_recoveries = 0;                         // v6 : relances soft consecutives

    // v5 : recharge la table de tension apprise depuis la NVS (persiste entre reboots).
    {
        nvs_handle_t nh;
        if (nvs_open(GOV_NVS_NAMESPACE, NVS_READONLY, &nh) == ESP_OK) {
            uint16_t blob[TUNE_BUCKETS * 2];
            size_t sz = sizeof(blob);
            if (nvs_get_blob(nh, GOV_NVS_KEY, blob, &sz) == ESP_OK && sz == sizeof(blob)) {
                for (int i = 0; i < TUNE_BUCKETS; i++) {
                    gov_learned_mv[i] = blob[i];
                    gov_floor_mv[i]   = blob[TUNE_BUCKETS + i];
                }
                ESP_LOGI(TAG, "[TUNER] table de tension rechargee depuis la NVS");
            }
            nvs_close(nh);
        }
    }

    while (1) {
        if (GLOBAL_STATE->SELF_TEST_MODULE.is_finished) {
            ESP_LOGI(TAG, "Stopped");
            vTaskDelete(NULL);
            return;
        }

        power_management->voltage = Power_get_input_voltage(GLOBAL_STATE);
        Power_get_output(GLOBAL_STATE, &power_management->power, &power_management->current);
        power_management->core_voltage = VCORE_get_voltage_mv(GLOBAL_STATE);

        power_management->chip_temp_avg = Thermal_get_chip_temp(GLOBAL_STATE);
        power_management->chip_temp2_avg = Thermal_get_chip_temp2(GLOBAL_STATE);

        power_management->vr_temp = Power_get_vreg_temp(GLOBAL_STATE);
        // User pause, hardware fault, or all pools unreachable
        bool wants_stop = sys_module->mining_paused || sys_module->hardware_fault || sys_module->pools_unavailable;
        if (wants_stop && !is_paused) {
            mining_stop(GLOBAL_STATE);
            is_paused = true;
        } else if (!wants_stop && is_paused) {
            mining_start(GLOBAL_STATE);
            is_paused = false;
        }

        // If we've paused or have a hardware fault, skip doing anything else
        if (is_paused || sys_module->hardware_fault) {
            vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
            continue;
        }

        bool asic_overheat =
            power_management->chip_temp_avg > THROTTLE_TEMP
            || power_management->chip_temp2_avg > THROTTLE_TEMP;

        if ((power_management->vr_temp > TPS546_THROTTLE_TEMP || asic_overheat) && (power_management->frequency_value > 50 || power_management->voltage > 1000)) {
            if (power_management->chip_temp2_avg > 0) {
                ESP_LOGE(TAG, "OVERHEAT! VR: %fC ASIC1: %fC ASIC2: %fC", power_management->vr_temp, power_management->chip_temp_avg, power_management->chip_temp2_avg);
            } else {
                ESP_LOGE(TAG, "OVERHEAT! VR: %fC ASIC: %fC", power_management->vr_temp, power_management->chip_temp_avg);
            }

            last_known_asic_voltage = nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
            last_known_asic_frequency = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);
            nvs_config_set_bool(NVS_CONFIG_AUTO_FAN_SPEED, false);
            nvs_config_set_u16(NVS_CONFIG_MANUAL_FAN_SPEED, 100);
            nvs_config_set_bool(NVS_CONFIG_OVERHEAT_MODE, true);
            ESP_LOGW(TAG, "Entering safe mode due to overheat condition. System operation halted.");
            mining_stop(GLOBAL_STATE);
            
            // Note: ASIC temperature readings are invalid when ASIC is powered down (returns -1)
            // For 600-series boards that use ASIC thermal diode, we rely on VR temp and fixed cooling time
            // For boards with EMC internal temp sensor, readings remain valid
            bool asic_temp_valid = GLOBAL_STATE->DEVICE_CONFIG.emc_internal_temp;
            int cooling_cycles = 0;
            const int MIN_COOLING_CYCLES = 6; // Minimum 30 seconds cooling
            
            while (cooling_cycles < MIN_COOLING_CYCLES || power_management->vr_temp > TPS546_THROTTLE_TEMP - 10) {
                vTaskDelay(5000 / portTICK_PERIOD_MS); // Wait 5 seconds
                cooling_cycles++;
                
                power_management->vr_temp = Power_get_vreg_temp(GLOBAL_STATE);
                
                // Only check ASIC temps if they're valid (not using ASIC thermal diode)
                if (asic_temp_valid) {
                    power_management->chip_temp_avg = Thermal_get_chip_temp(GLOBAL_STATE);
                    power_management->chip_temp2_avg = Thermal_get_chip_temp2(GLOBAL_STATE);
                    ESP_LOGW(TAG, "Safe mode active (cycle %d) - VR: %.1f°C ASIC1: %.1f°C ASIC2: %.1f°C",
                             cooling_cycles, power_management->vr_temp, power_management->chip_temp_avg, power_management->chip_temp2_avg);
                    
                    // Continue if ASIC temps still too high
                    if (power_management->chip_temp_avg >  SAFE_TEMP || power_management->chip_temp2_avg > SAFE_TEMP) {
                        cooling_cycles = 0; // Reset cycle count if still hot
                    }
                } else {
                    // For boards using ASIC thermal diode (600 series), rely on VR temp and time
                    ESP_LOGW(TAG, "Safe mode active (cycle %d/%d) - VR: %.1f°C (ASIC temps unavailable while powered down)",
                             cooling_cycles, MIN_COOLING_CYCLES, power_management->vr_temp);
                }
            }
            ESP_LOGI(TAG, "Temperature normalized after %d cooling cycles. Reinitializing ASIC...", cooling_cycles);
            
            uint16_t reduced_voltage = last_known_asic_voltage > ASIC_REDUCTION ? last_known_asic_voltage - ASIC_REDUCTION : 1000;
            float reduced_asic_frequency = last_known_asic_frequency > ASIC_REDUCTION ? last_known_asic_frequency - ASIC_REDUCTION : 400.0;
            
            nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, reduced_voltage);
            nvs_config_set_float(NVS_CONFIG_ASIC_FREQUENCY, reduced_asic_frequency);
            
            ESP_LOGI(TAG, "Restoring at reduced settings: %umV (was %umV), %.0f MHz (was %.0f MHz)",
                     reduced_voltage, last_known_asic_voltage, reduced_asic_frequency, last_known_asic_frequency);

            uint8_t chip_count = mining_start(GLOBAL_STATE);

            if (chip_count > 0) {
                // Frequency reduction will now be applied by normal power management loop
                nvs_config_set_bool(NVS_CONFIG_OVERHEAT_MODE, false);
                ESP_LOGI(TAG, "Resuming normal operation. Reduced frequency (%.0f MHz) will be applied automatically.", reduced_asic_frequency);
            }
        }

        // V3 : la tension n'est plus lue depuis NVS ici -> c'est le gouverneur qui
        // la fixe automatiquement selon la frequence (table "propre"), plus bas.
        float asic_frequency = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);

        // ---- GOUVERNEUR THERMIQUE ----
        // 'asic_frequency' (depuis NVS) = plafond voulu par l'utilisateur.
        // Si l'utilisateur change la frequence via l'API/interface, on resynchronise le plafond.
        if (asic_frequency != gov_target_freq) {
            gov_target_freq = asic_frequency;
            if (gov_effective_freq > gov_target_freq) gov_effective_freq = gov_target_freq;
        }

        // Ajuste la frequence effective selon la temperature, toutes les ~10s (hors self-test).
        if (!GLOBAL_STATE->SELF_TEST_MODULE.is_active && ++gov_counter >= GOV_INTERVAL_CYCLES) {
            gov_counter = 0;
            float t = power_management->chip_temp_avg;
            if (power_management->chip_temp2_avg > t) t = power_management->chip_temp2_avg;
            float p = power_management->power;

            // v8 : l'input voltage s'affaisse quand on tire trop de courant (freq trop haute).
            float vin = power_management->voltage;   // tension d'entree 5V (mV)
            bool vin_low = false;
#if GOV_VIN_ENABLE
            vin_low = (vin > 1000.0f && vin < GOV_VIN_MIN);   // >1000 mV = lecture valide
#endif
            if ((t >= GOV_TEMP_EMERGENCY || p >= GOV_POWER_EMERGENCY) && gov_effective_freq > GOV_FREQ_MIN) {
                gov_effective_freq -= (GOV_FREQ_STEP * 2.0f);   // urgence temp OU puissance : baisse renforcee
            } else if ((t >= GOV_TEMP_HIGH || p >= GOV_POWER_HIGH || vin_low) && gov_effective_freq > GOV_FREQ_MIN) {
                if (vin_low) gov_vin_ceiling = gov_effective_freq - GOV_FREQ_STEP;  // memorise le plafond input
                gov_effective_freq -= GOV_FREQ_STEP;            // trop chaud / trop de watts / input < 5V : on baisse
            } else if (t <= GOV_TEMP_LOW && p <= GOV_POWER_LOW && gov_effective_freq < gov_target_freq && gov_effective_freq < gov_vin_ceiling) {
                gov_effective_freq += GOV_FREQ_STEP;            // frais, marge de watts ET input OK : on remonte
            }

            if (gov_effective_freq < GOV_FREQ_MIN) gov_effective_freq = GOV_FREQ_MIN;
            if (gov_effective_freq > gov_target_freq) gov_effective_freq = gov_target_freq;
        }

        // ---- V4 : AUTO-TUNER DE TENSION ----
        // Pendant le self-test : tension par defaut. Sinon : tension apprise par palier,
        // grignotee vers le bas tant que le taux d'erreur ASIC reste sain.
        uint16_t gov_voltage;
        if (GLOBAL_STATE->SELF_TEST_MODULE.is_active) {
            gov_voltage = GLOBAL_STATE->DEVICE_CONFIG.family.asic.default_voltage_mv;
        } else {
            int b = gov_bucket(gov_effective_freq);
            uint16_t baseline = (uint16_t) gov_voltage_for_freq(gov_effective_freq);

            // Premiere visite de ce palier -> on AMORCE depuis le voisin deja appris (v5)
            // (au lieu de repartir de la baseline haute) pour eviter l'oscillation.
            if (gov_learned_mv[b] == 0) gov_learned_mv[b] = gov_seed_voltage(b, baseline, gov_learned_mv);

            // La frequence a change de palier -> on laisse le hashrate/erreur se stabiliser.
            if (b != last_bucket) {
                last_bucket = b;
                tune_settle = TUNE_SETTLE_CYCLES;
            }

#if TUNE_ENABLE
            // Apprentissage : une decision toutes les ~30s, apres stabilisation.
            if (tune_settle > 0) {
                tune_settle--;
            } else {
                float err = sys_module->error_percentage;
                uint16_t v = gov_learned_mv[b];

                // Bornes de descente : garde-fou dur, limite vs table "propre", plancher verrouille.
                uint16_t min_allowed = TUNE_VOLT_FLOOR;
                uint16_t soft_min = (baseline > TUNE_MAX_UNDERVOLT) ? baseline - TUNE_MAX_UNDERVOLT : TUNE_VOLT_FLOOR;
                if (soft_min > min_allowed) min_allowed = soft_min;
                if (gov_floor_mv[b] > 0 && gov_floor_mv[b] + TUNE_MARGIN > min_allowed) min_allowed = gov_floor_mv[b] + TUNE_MARGIN;

                if (err >= TUNE_ERR_BAD) {
                    // Instable : on verrouille ce niveau comme plancher et on remonte franchement.
                    gov_floor_mv[b] = v;
                    uint16_t up = v + TUNE_STEP_UP;
                    if (up > baseline + 30) up = baseline + 30;   // plafond raisonnable
                    if (up > 1250) up = 1250;
                    gov_learned_mv[b] = up;
                    ESP_LOGW(TAG, "[TUNER] %g MHz : erreurs %.2f%% -> remonte %u -> %u mV (plancher verrouille a %u)", gov_effective_freq, err, v, up, v);
                    tune_settle = TUNE_SETTLE_CYCLES;
                    tune_dirty = true;
                } else if (err <= TUNE_ERR_GOOD && (int)v - TUNE_STEP_DOWN >= (int)min_allowed) {
                    // Sain et marge disponible : on tente de baisser d'un cran.
                    gov_learned_mv[b] = v - TUNE_STEP_DOWN;
                    ESP_LOGI(TAG, "[TUNER] %g MHz : erreurs %.2f%% OK -> essai %u -> %u mV", gov_effective_freq, err, v, (uint16_t)(v - TUNE_STEP_DOWN));
                    tune_settle = TUNE_SETTLE_CYCLES;
                    tune_dirty = true;
                }
                // sinon : zone morte -> point d'equilibre trouve, on garde.
            }
#endif
            gov_voltage = gov_learned_mv[b];
        }

        // Applique la tension AVANT la frequence (stabilite en montee de frequence).
        if (gov_voltage != last_core_voltage) {
            VCORE_set_voltage(GLOBAL_STATE, (double) gov_voltage / 1000.0);
            last_core_voltage = gov_voltage;
        }

        // Applique la frequence effective quand elle change.
        if (gov_effective_freq != last_asic_frequency) {
            ESP_LOGI(TAG, "[GOVERNOR] temp %.1fC pow %.1fW vin %.2fV -> ASIC %g MHz @ %umV (plafond %g MHz)", power_management->chip_temp_avg, power_management->power, power_management->voltage / 1000.0, gov_effective_freq, gov_voltage, gov_target_freq);

            power_management->frequency_value = gov_effective_freq;
            power_management->expected_hashrate = expected_hashrate(GLOBAL_STATE);

            ASIC_set_frequency(GLOBAL_STATE);
            ASIC_set_nonce_space(GLOBAL_STATE);

            last_asic_frequency = gov_effective_freq;
        }

        // Check for changing of overheat mode
        bool new_overheat_mode = nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE);
        
        if (new_overheat_mode != sys_module->overheat_mode) {
            sys_module->overheat_mode = new_overheat_mode;
            ESP_LOGI(TAG, "Overheat mode updated to: %d", sys_module->overheat_mode);
        }

        VCORE_check_fault(GLOBAL_STATE);

#if GUARD_ENABLE
        // ---- V6 : GARDIEN ANTI-ABANDON ----
        // Detecte un ASIC fige (hashrate ~0 alors qu'on devrait miner) et le relance tout
        // seul. Filet de securite SOUS la detection d'erreurs du tuner : un sous-voltage
        // trop agressif peut figer la puce sans meme generer d'erreurs (hashrate a 0).
        if (is_paused || sys_module->overheat_mode || !GLOBAL_STATE->ASIC_initalized
                || GLOBAL_STATE->SELF_TEST_MODULE.is_active) {
            // Pause volontaire / surchauffe / ASIC non initialise / self-test : on ne surveille pas.
            stall_counter = 0;
            stall_grace = GUARD_GRACE_CYCLES;
        } else if (stall_grace > 0) {
            stall_grace--;                 // laisse le hashrate monter apres un (re)demarrage
            stall_counter = 0;
        } else if (sys_module->current_hashrate < GUARD_MIN_HASHRATE) {
            // Hashrate au plancher alors qu'on devrait miner -> on compte.
            if (++stall_counter >= GUARD_STALL_CYCLES) {
                stall_counter = 0;
                stall_recoveries++;

                // Anti-lockup par sous-voltage : on remonte la tension du palier courant
                // et on verrouille ce niveau comme plancher (ne pas y redescendre).
                int gb = gov_bucket(gov_effective_freq);
                uint16_t gbase = (uint16_t) gov_voltage_for_freq(gov_effective_freq);
                uint16_t bumped = gov_learned_mv[gb] + GUARD_VOLT_BUMP;
                if (bumped > gbase + 30) bumped = gbase + 30;
                if (bumped > 1250) bumped = 1250;
                gov_learned_mv[gb] = bumped;
                gov_floor_mv[gb] = bumped;  // verrouille AU niveau remonte : le tuner n'y redescendra pas
                gov_save_vtable_nvs(gov_learned_mv, gov_floor_mv); // persiste AVANT toute relance
                tune_dirty = false;

                if (stall_recoveries > GUARD_MAX_SOFT) {
                    ESP_LOGE(TAG, "[GARDIEN] ASIC toujours fige apres %d relances -> REDEMARRAGE COMPLET", GUARD_MAX_SOFT);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    esp_restart();         // dernier recours (la table apprise est deja en NVS)
                }

                ESP_LOGW(TAG, "[GARDIEN] ASIC fige (hashrate ~0) depuis %ds -> relance #%d, tension %g MHz remontee a %u mV",
                         GUARD_STALL_CYCLES / 10, stall_recoveries, gov_effective_freq, bumped);
                mining_stop(GLOBAL_STATE);
                mining_start(GLOBAL_STATE);
                last_asic_frequency = 0;   // force le gouverneur a re-appliquer la frequence (ramp auto)
                last_core_voltage = 0;     // force la re-application de la tension
                stall_grace = GUARD_GRACE_CYCLES;
            }
        } else {
            // Hashrate correct -> tout va bien, on remet les compteurs a zero.
            stall_counter = 0;
            stall_recoveries = 0;
        }
#endif

        // v5 : sauvegarde NVS throttlee (au plus 1 ecriture / ~30 min, et seulement si change).
        if (++tune_save_counter >= TUNE_SAVE_INTERVAL_CYCLES) {
            tune_save_counter = 0;
            if (tune_dirty) {
                gov_save_vtable_nvs(gov_learned_mv, gov_floor_mv);
                tune_dirty = false;
            }
        }

        // looper:
        vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
    }
}
