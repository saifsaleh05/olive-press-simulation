#ifndef OLIVE_COMMON_H
#define OLIVE_COMMON_H

#include <sys/types.h>

typedef enum {
    STAGE_RECEPTION = 0,
    STAGE_CLEANING,
    STAGE_WASHING,
    STAGE_CRUSHING,
    STAGE_MALAXATION,
    STAGE_SEPARATION
} Stage;

typedef enum {
    ROLE_RECEPTION = 0,
    ROLE_CLEANING,
    ROLE_WASHING,
    ROLE_CRUSHING,
    ROLE_MALAXATION,
    ROLE_SEPARATION,
    ROLE_QUALITY_CONTROL,
    ROLE_COUNT
} ProcessRole;

typedef struct {
    int id;
    double original_mass_kg;
    double current_mass_kg;
    double olive_mass_kg;
    double impurity_mass_kg;
    double removed_impurity_kg;
    double oil_potential_fraction;
    double quality;
    Stage stage;
} Batch;

typedef struct {
    ProcessRole role;
    pid_t pid;
    int wait_status;
    int has_exited;
} ChildRecord;

const char *stage_name(Stage stage);
const char *process_role_name(ProcessRole role);

#endif
