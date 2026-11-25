/* config.c - defaults, KEY=VALUE loader and validation. See config.h. */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(Config *cfg)
{
    /* base_time heat  cool  base_p gain  maxT recT  maxP recP maxBatch */
    static const MachineConfig def[NUM_MACHINES] = {
        [M_RECEPTION]  = { 1.0, 0.05, 0.08, 0.5, 0.3, 40.0, 35.0, 3.0, 2.0,  5.0 },
        [M_CLEANING]   = { 2.0, 0.15, 0.07, 1.0, 0.3, 40.0, 35.0, 3.0, 2.0,  8.0 },
        [M_WASHING]    = { 3.0, 0.10, 0.09, 1.2, 0.4, 40.0, 35.0, 4.0, 3.0, 12.0 },
        [M_CRUSHING]   = { 4.0, 0.80, 0.05, 3.0, 1.0, 35.0, 30.0, 6.0, 5.0, 15.0 },
        [M_MALAXATION] = { 6.0, 0.40, 0.04, 1.0, 0.5, 35.0, 30.0, 3.0, 2.0, 20.0 },
        [M_SEPARATION] = { 3.0, 0.65, 0.05, 4.0, 1.0, 40.0, 35.0, 6.0, 5.0, 12.0 },
    };

    memset(cfg, 0, sizeof *cfg);
    cfg->total_olives_kg = 500.0;
    cfg->batch_size_kg   = (Range){ 20.0, 30.0 };
    cfg->impurity        = (Range){ 0.01, 0.05 };
    cfg->oil_potential   = (Range){ 0.15, 0.25 };
    cfg->quality         = (Range){ 0.60, 1.00 };
    cfg->cleaning_loss   = (Range){ 0.01, 0.03 };
    cfg->washing_loss    = (Range){ 0.005, 0.015 };

    cfg->ambient_temperature    = 24.0;
    cfg->nominal_batch_kg       = 25.0;
    cfg->temperature_noise      = 0.3;
    cfg->pressure_noise         = 0.1;
    cfg->pressure_time_constant = 1.0;
    cfg->queue_capacity         = 2;
    cfg->tick_ms                = 100;

    cfg->seed                = 12345;
    cfg->auto_faults_enabled = 1;
    cfg->fault_probability   = 0.03;
    cfg->heat_spike          = 10.0;
    cfg->pressure_spike      = 3.0;

    cfg->quality_loss_rate     = 0.02;
    cfg->extraction_efficiency = 0.90;
    cfg->quality_weight        = 0.5;
    cfg->veg_water             = (Range){ 0.30, 0.40 };

    memcpy(cfg->machine, def, sizeof def);
}

static const char *const MACHINE_PREFIX[NUM_MACHINES] = {
    "RECEPTION", "CLEANING", "WASHING", "CRUSHING", "MALAXATION", "SEPARATION"
};

static char *trim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int parse_double(const char *v, double *out)
{
    char *end;
    double d;

    errno = 0;
    d = strtod(v, &end);
    if (end == v || errno == ERANGE || !isfinite(d))
        return -1;
    while (isspace((unsigned char)*end))
        end++;
    if (*end != '\0')
        return -1;
    *out = d;
    return 0;
}

typedef struct {
    const char *key;
    size_t      off;
    double      scale;      /* 0.01 for *_PERCENT keys */
} Key;

#define G(k, f, s)  { k, offsetof(Config, f), s }
static const Key GLOBAL_KEYS[] = {
    G("TOTAL_OLIVES_KG",            total_olives_kg, 1),
    G("BATCH_SIZE_MIN",             batch_size_kg.min, 1),
    G("BATCH_SIZE_MAX",             batch_size_kg.max, 1),
    G("IMPURITY_PERCENT_MIN",       impurity.min, 0.01),
    G("IMPURITY_PERCENT_MAX",       impurity.max, 0.01),
    G("OIL_POTENTIAL_PERCENT_MIN",  oil_potential.min, 0.01),
    G("OIL_POTENTIAL_PERCENT_MAX",  oil_potential.max, 0.01),
    G("QUALITY_PERCENT_MIN",        quality.min, 0.01),
    G("QUALITY_PERCENT_MAX",        quality.max, 0.01),
    G("CLEANING_LOSS_PERCENT_MIN",  cleaning_loss.min, 0.01),
    G("CLEANING_LOSS_PERCENT_MAX",  cleaning_loss.max, 0.01),
    G("WASHING_LOSS_PERCENT_MIN",   washing_loss.min, 0.01),
    G("WASHING_LOSS_PERCENT_MAX",   washing_loss.max, 0.01),
    G("AMBIENT_TEMPERATURE",        ambient_temperature, 1),
    G("NOMINAL_BATCH_KG",           nominal_batch_kg, 1),
    G("TEMPERATURE_NOISE",          temperature_noise, 1),
    G("PRESSURE_NOISE",             pressure_noise, 1),
    G("PRESSURE_TIME_CONSTANT",     pressure_time_constant, 1),
    G("FAULT_PROBABILITY_PERCENT",  fault_probability, 0.01),
    G("HEAT_SPIKE",                 heat_spike, 1),
    G("PRESSURE_SPIKE",             pressure_spike, 1),
    G("QUALITY_LOSS_RATE",          quality_loss_rate, 1),
    G("EXTRACTION_EFFICIENCY_PERCENT", extraction_efficiency, 0.01),
    G("QUALITY_WEIGHT",             quality_weight, 1),
    G("VEG_WATER_PERCENT_MIN",      veg_water.min, 0.01),
    G("VEG_WATER_PERCENT_MAX",      veg_water.max, 0.01),
};
#undef G

#define M(k, f)  { k, offsetof(MachineConfig, f), 1 }
static const Key MACHINE_KEYS[] = {
    M("BASE_TIME",            base_time),
    M("HEATING_RATE",         heating_rate),
    M("COOLING_FACTOR",       cooling_factor),
    M("BASE_PRESSURE",        base_pressure),
    M("PRESSURE_GAIN",        pressure_gain),
    M("MAX_TEMPERATURE",      max_temperature),
    M("RECOVERY_TEMPERATURE", recovery_temperature),
    M("MAX_PRESSURE",         max_pressure),
    M("RECOVERY_PRESSURE",    recovery_pressure),
    M("MAX_BATCH_TIME",       max_batch_time),
};
#undef M

static int set_from_table(void *base, const Key *keys, size_t n,
                          const char *key, double d)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(key, keys[i].key) == 0) {
            *(double *)((char *)base + keys[i].off) = d * keys[i].scale;
            return 0;
        }
    }
    return -1;
}

static int set_key(Config *cfg, const char *key, const char *v)
{
    double d;

    if (parse_double(v, &d) != 0)
        return -1;

    /* Integer keys must be exact integers; do not silently truncate 2.7. */
    if (strcmp(key, "RANDOM_SEED") == 0) {
        if (d < 0.0 || d > (double)UINT_MAX || floor(d) != d)
            return -1;
        cfg->seed = (unsigned int)d;
        return 0;
    }
    if (strcmp(key, "QUEUE_CAPACITY") == 0 ||
        strcmp(key, "TICK_MS") == 0 ||
        strcmp(key, "AUTO_FAULTS_ENABLED") == 0) {
        int *dst;

        if (d < (double)INT_MIN || d > (double)INT_MAX || floor(d) != d)
            return -1;
        dst = strcmp(key, "QUEUE_CAPACITY") == 0 ? &cfg->queue_capacity :
              strcmp(key, "TICK_MS") == 0 ? &cfg->tick_ms :
                                             &cfg->auto_faults_enabled;
        *dst = (int)d;
        return 0;
    }

    if (set_from_table(cfg, GLOBAL_KEYS,
                       sizeof GLOBAL_KEYS / sizeof GLOBAL_KEYS[0], key, d) == 0)
        return 0;

    /* <MACHINE>_<FIELD> */
    for (int m = 0; m < NUM_MACHINES; m++) {
        size_t len = strlen(MACHINE_PREFIX[m]);
        if (strncmp(key, MACHINE_PREFIX[m], len) == 0 && key[len] == '_')
            return set_from_table(&cfg->machine[m], MACHINE_KEYS,
                                  sizeof MACHINE_KEYS / sizeof MACHINE_KEYS[0],
                                  key + len + 1, d);
    }
    return -1;
}

int config_load(Config *cfg, const char *path)
{
    char line[256];
    char err[256];
    Config next;
    int lineno = 0, failed = 0;
    FILE *f = fopen(path, "r");

    if (!f) {
        perror(path);
        return -1;
    }
    next = *cfg;
    while (fgets(line, sizeof line, f)) {
        char *hash, *eq, *key, *val;

        lineno++;
        if (!strchr(line, '\n') && !feof(f)) {
            int ch;

            while ((ch = fgetc(f)) != '\n' && ch != EOF)
                ;
            fprintf(stderr, "%s:%d: line too long\n", path, lineno);
            failed = 1;
            continue;
        }
        if ((hash = strchr(line, '#')) != NULL)
            *hash = '\0';
        key = trim(line);
        if (*key == '\0')
            continue;
        if ((eq = strchr(key, '=')) == NULL) {
            fprintf(stderr, "%s:%d: missing '='\n", path, lineno);
            failed = 1;
            continue;
        }
        *eq = '\0';
        key = trim(key);
        val = trim(eq + 1);
        if (set_key(&next, key, val) != 0) {
            fprintf(stderr, "%s:%d: unknown key or bad value '%s=%s'\n",
                    path, lineno, key, val);
            failed = 1;
        }
    }
    if (ferror(f)) {
        perror(path);
        failed = 1;
    }
    if (fclose(f) != 0) {
        perror(path);
        failed = 1;
    }
    if (!failed && config_validate(&next, err, sizeof err) != 0) {
        fprintf(stderr, "%s: invalid configuration: %s\n", path, err);
        failed = 1;
    }
    if (failed)
        return -1;
    *cfg = next;
    return 0;
}

static int bad_range(Range r, double lo, double hi)
{
    return !isfinite(r.min) || !isfinite(r.max) ||
           r.min > r.max || r.min < lo || r.max > hi;
}

int config_validate(const Config *cfg, char *err, size_t n)
{
#define FAIL(...) do { snprintf(err, n, __VA_ARGS__); return -1; } while (0)
    if (!cfg || !err || n == 0)
        return -1;
    if (bad_range(cfg->batch_size_kg, 0.001, 1e9)) FAIL("BATCH_SIZE range invalid");
    if (bad_range(cfg->impurity, 0, 1))      FAIL("IMPURITY_PERCENT range invalid");
    if (bad_range(cfg->oil_potential, 0, 1)) FAIL("OIL_POTENTIAL_PERCENT range invalid");
    if (bad_range(cfg->quality, 0, 1))       FAIL("QUALITY_PERCENT range invalid");
    if (bad_range(cfg->cleaning_loss, 0, 1)) FAIL("CLEANING_LOSS_PERCENT range invalid");
    if (bad_range(cfg->washing_loss, 0, 1))  FAIL("WASHING_LOSS_PERCENT range invalid");
    if (bad_range(cfg->veg_water, 0, 1))     FAIL("VEG_WATER_PERCENT range invalid");
    if (cfg->oil_potential.max + cfg->veg_water.max > 1.0)
        FAIL("OIL_POTENTIAL_PERCENT_MAX + VEG_WATER_PERCENT_MAX exceed 100%%");
    if (!isfinite(cfg->extraction_efficiency) ||
        cfg->extraction_efficiency < 0 || cfg->extraction_efficiency > 1)
        FAIL("EXTRACTION_EFFICIENCY_PERCENT must be 0..100");
    if (!isfinite(cfg->quality_weight) ||
        cfg->quality_weight < 0 || cfg->quality_weight > 1)
        FAIL("QUALITY_WEIGHT must be 0..1");
    if (!isfinite(cfg->fault_probability) ||
        cfg->fault_probability < 0 || cfg->fault_probability > 1)
        FAIL("FAULT_PROBABILITY_PERCENT must be 0..100");
    if (!isfinite(cfg->nominal_batch_kg) || cfg->nominal_batch_kg <= 0)
        FAIL("NOMINAL_BATCH_KG must be > 0");
    if (cfg->tick_ms < 1)           FAIL("TICK_MS must be >= 1");
    if (cfg->queue_capacity < 1)    FAIL("QUEUE_CAPACITY must be >= 1");
    if (!isfinite(cfg->pressure_time_constant) || cfg->pressure_time_constant <= 0)
        FAIL("PRESSURE_TIME_CONSTANT must be > 0");
    if (!isfinite(cfg->total_olives_kg) || cfg->total_olives_kg < 0)
        FAIL("TOTAL_OLIVES_KG must be >= 0");
    if (!isfinite(cfg->ambient_temperature)) FAIL("AMBIENT_TEMPERATURE must be finite");
    if (!isfinite(cfg->temperature_noise) || cfg->temperature_noise < 0)
        FAIL("TEMPERATURE_NOISE must be >= 0");
    if (!isfinite(cfg->pressure_noise) || cfg->pressure_noise < 0)
        FAIL("PRESSURE_NOISE must be >= 0");
    if (!isfinite(cfg->heat_spike) || cfg->heat_spike < 0)
        FAIL("HEAT_SPIKE must be >= 0");
    if (!isfinite(cfg->pressure_spike) || cfg->pressure_spike < 0)
        FAIL("PRESSURE_SPIKE must be >= 0");
    if (!isfinite(cfg->quality_loss_rate) || cfg->quality_loss_rate < 0)
        FAIL("QUALITY_LOSS_RATE must be >= 0");
    if (cfg->auto_faults_enabled != 0 && cfg->auto_faults_enabled != 1)
        FAIL("AUTO_FAULTS_ENABLED must be 0 or 1");

    for (int m = 0; m < NUM_MACHINES; m++) {
        const MachineConfig *mc = &cfg->machine[m];
        const char *p = MACHINE_PREFIX[m];

        if (!isfinite(mc->base_time) || !isfinite(mc->heating_rate) ||
            !isfinite(mc->cooling_factor) || !isfinite(mc->base_pressure) ||
            !isfinite(mc->pressure_gain) || !isfinite(mc->max_temperature) ||
            !isfinite(mc->recovery_temperature) ||
            !isfinite(mc->max_pressure) || !isfinite(mc->recovery_pressure) ||
            !isfinite(mc->max_batch_time))
            FAIL("%s machine values must be finite", p);
        if (mc->base_time <= 0)      FAIL("%s_BASE_TIME must be > 0", p);
        if (mc->heating_rate < 0)    FAIL("%s_HEATING_RATE must be >= 0", p);
        if (mc->cooling_factor <= 0 || mc->cooling_factor * cfg->tick_ms / 1000.0 >= 1.0)
            FAIL("%s_COOLING_FACTOR must be > 0 and < 1/tick", p);
        if (mc->recovery_temperature > mc->max_temperature)
            FAIL("%s_RECOVERY_TEMPERATURE must be <= MAX_TEMPERATURE", p);
        if (mc->base_pressure < 0)   FAIL("%s_BASE_PRESSURE must be >= 0", p);
        if (mc->pressure_gain < 0)   FAIL("%s_PRESSURE_GAIN must be >= 0", p);
        if (mc->max_pressure < 0 || mc->recovery_pressure < 0)
            FAIL("%s pressure limits must be >= 0", p);
        if (mc->recovery_pressure > mc->max_pressure)
            FAIL("%s_RECOVERY_PRESSURE must be <= MAX_PRESSURE", p);
        if (mc->max_batch_time < 0)  FAIL("%s_MAX_BATCH_TIME must be >= 0", p);
    }
    return 0;
#undef FAIL
}

void config_print(const Config *cfg)
{
    printf("config: olives=%.0fkg batch=[%.1f,%.1f]kg impurity=[%.1f,%.1f]%% "
           "oil=[%.1f,%.1f]%% quality=[%.0f,%.0f]%% seed=%u\n",
           cfg->total_olives_kg, cfg->batch_size_kg.min, cfg->batch_size_kg.max,
           cfg->impurity.min * 100, cfg->impurity.max * 100,
           cfg->oil_potential.min * 100, cfg->oil_potential.max * 100,
           cfg->quality.min * 100, cfg->quality.max * 100, cfg->seed);
    printf("config: ambient=%.1fC nominal=%.0fkg tick=%dms queue=%d "
           "efficiency=%.0f%% veg_water=[%.0f,%.0f]%% auto_faults=%d (%.1f%%)\n",
           cfg->ambient_temperature, cfg->nominal_batch_kg, cfg->tick_ms,
           cfg->queue_capacity, cfg->extraction_efficiency * 100,
           cfg->veg_water.min * 100, cfg->veg_water.max * 100,
           cfg->auto_faults_enabled, cfg->fault_probability * 100);
    for (int m = 0; m < NUM_MACHINES; m++) {
        const MachineConfig *mc = &cfg->machine[m];
        printf("config: %-10s t=%.1fs H=%.2f C=%.3f P=%.1f+%.1f*load "
               "T<%.0f(rec %.0f) P<%.1f(rec %.1f) batch<%.0fs\n",
               machine_name(m), mc->base_time, mc->heating_rate,
               mc->cooling_factor, mc->base_pressure, mc->pressure_gain,
               mc->max_temperature, mc->recovery_temperature,
               mc->max_pressure, mc->recovery_pressure, mc->max_batch_time);
    }
}
