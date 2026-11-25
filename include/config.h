/*
 * config.h - user-defined values loaded from config.txt at start-up.
 *
 * SHARED CONTRACT.  Run as:  ./olive_press config.txt
 *
 * File format: one KEY=VALUE per line, '#' starts a comment, blank lines are
 * ignored. Ranges use two keys, KEY_MIN and KEY_MAX. Keys ending in _PERCENT
 * are written in percent in the file and stored as fractions (0..1) here.
 * Per-machine keys are prefixed with the machine name, e.g.
 *     MALAXATION_HEATING_RATE=0.40
 *     MALAXATION_MAX_TEMPERATURE=35
 *     MALAXATION_RECOVERY_TEMPERATURE=30
 * See config.txt for the full list with comments.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include "common.h"

typedef struct {
    double min;
    double max;
} Range;

typedef struct {
    double base_time;            /* s to process NOMINAL_BATCH_KG           */
    double heating_rate;         /* H, deg C / s while running              */
    double cooling_factor;       /* C, 1/s, pulls T back towards ambient    */
    double base_pressure;        /* bar while running                       */
    double pressure_gain;        /* extra bar per unit of load (kg/25 kg)   */
    double max_temperature;      /* QC: alarm above this                    */
    double recovery_temperature; /* QC: recovered below this                */
    double max_pressure;         /* QC: alarm above this                    */
    double recovery_pressure;    /* QC: recovered below this                */
    double max_batch_time;       /* QC: timeout for one batch, s            */
} MachineConfig;

typedef struct {
    /* olives and batches (Reception / Cleaning / Washing) */
    double total_olives_kg;
    Range  batch_size_kg;
    Range  impurity;             /* fraction                                */
    Range  oil_potential;        /* fraction                                */
    Range  quality;              /* fraction                                */
    Range  cleaning_loss;        /* fraction of mass removed by Cleaning    */
    Range  washing_loss;         /* fraction of mass removed by Washing     */

    /* factory */
    double ambient_temperature;  /* deg C, fixed                            */
    double nominal_batch_kg;     /* batch that gives load = 1 (25 kg)       */
    double temperature_noise;    /* +- deg C on sensor readings             */
    double pressure_noise;       /* +- bar on sensor readings               */
    double pressure_time_constant; /* s, how fast pressure follows load     */
    int    queue_capacity;       /* batches allowed to wait before a stage  */
    int    tick_ms;              /* simulation tick of every machine        */

    /* faults */
    unsigned int seed;           /* RANDOM_SEED, each process derives its own */
    int    auto_faults_enabled;
    double fault_probability;    /* fraction: chance a batch triggers a failure */
    double heat_spike;           /* deg C added by a manual heat injection  */
    double pressure_spike;       /* bar added by a manual pressure injection*/

    /* malaxation / separation */
    double quality_loss_rate;    /* quality lost per s malaxing above max T */
    double extraction_efficiency;/* fraction of the oil potential recovered */
    double quality_weight;       /* how much quality affects efficiency 0..1*/
    Range  veg_water;            /* fraction of paste -> vegetable water    */

    MachineConfig machine[NUM_MACHINES];
} Config;

/* Fill every field with the team's default values. */
void config_defaults(Config *cfg);

/* Read KEY=VALUE lines from path on top of the current values. The update is
 * transactional: unknown keys, malformed/non-finite values, invalid ranges,
 * and impossible/negative quantities reject the whole file and leave cfg
 * unchanged. Returns 0 on success, -1 on any error. */
int  config_load(Config *cfg, const char *path);

/* Check ranges and impossible values. Returns 0 if valid, otherwise -1 and
 * a message in err. */
int  config_validate(const Config *cfg, char *err, size_t errlen);

/* Print the configuration (log / debugging). */
void config_print(const Config *cfg);

#endif /* CONFIG_H */
