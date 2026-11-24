#include "config.h"

#include <stdio.h>

void config_set_defaults(Config *config)
{
    config->total_olive_mass_kg = 500.0;
    config->batch_mass_min_kg = 80.0;
    config->batch_mass_max_kg = 120.0;
    config->impurity_fraction_min = 0.05;
    config->impurity_fraction_max = 0.10;
    config->cleaning_efficiency_min = 0.75;
    config->cleaning_efficiency_max = 0.90;
    config->oil_potential_min = 0.15;
    config->oil_potential_max = 0.22;
    config->reception_interval_ms = 100U;
    config->cleaning_duration_ms = 150U;
    config->seed = 4330U;
}

static int fraction_range_is_valid(double minimum, double maximum)
{
    return minimum >= 0.0 && maximum <= 1.0 && minimum <= maximum;
}

int config_validate(const Config *config, char *error, size_t error_size)
{
    if (config == NULL) {
        snprintf(error, error_size, "configuration pointer is null");
        return -1;
    }
    if (config->total_olive_mass_kg <= 0.0) {
        snprintf(error, error_size, "total olive mass must be positive");
        return -1;
    }
    if (config->batch_mass_min_kg <= 0.0 ||
        config->batch_mass_min_kg > config->batch_mass_max_kg) {
        snprintf(error, error_size, "invalid batch mass range");
        return -1;
    }
    if (!fraction_range_is_valid(config->impurity_fraction_min,
                                 config->impurity_fraction_max)) {
        snprintf(error, error_size, "invalid impurity fraction range");
        return -1;
    }
    if (!fraction_range_is_valid(config->cleaning_efficiency_min,
                                 config->cleaning_efficiency_max)) {
        snprintf(error, error_size, "invalid cleaning efficiency range");
        return -1;
    }
    if (!fraction_range_is_valid(config->oil_potential_min,
                                 config->oil_potential_max)) {
        snprintf(error, error_size, "invalid oil potential range");
        return -1;
    }
    return 0;
}
