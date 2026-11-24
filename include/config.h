#ifndef OLIVE_CONFIG_H
#define OLIVE_CONFIG_H

#include <stddef.h>

typedef struct {
    double total_olive_mass_kg;
    double batch_mass_min_kg;
    double batch_mass_max_kg;
    double impurity_fraction_min;
    double impurity_fraction_max;
    double cleaning_efficiency_min;
    double cleaning_efficiency_max;
    double oil_potential_min;
    double oil_potential_max;
    unsigned int reception_interval_ms;
    unsigned int cleaning_duration_ms;
    unsigned int seed;
} Config;

void config_set_defaults(Config *config);
int config_validate(const Config *config, char *error, size_t error_size);

#endif
