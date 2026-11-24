#include "common.h"

const char *stage_name(Stage stage)
{
    switch (stage) {
    case STAGE_RECEPTION: return "Reception";
    case STAGE_CLEANING: return "Cleaning";
    case STAGE_WASHING: return "Washing";
    case STAGE_CRUSHING: return "Crushing";
    case STAGE_MALAXATION: return "Malaxation";
    case STAGE_SEPARATION: return "Separation";
    }
    return "Unknown stage";
}

const char *process_role_name(ProcessRole role)
{
    switch (role) {
    case ROLE_RECEPTION: return "Reception";
    case ROLE_CLEANING: return "Cleaning";
    case ROLE_WASHING: return "Washing";
    case ROLE_CRUSHING: return "Crushing";
    case ROLE_MALAXATION: return "Malaxation";
    case ROLE_SEPARATION: return "Separation";
    case ROLE_QUALITY_CONTROL: return "Quality Control";
    case ROLE_COUNT: break;
    }
    return "Unknown role";
}
