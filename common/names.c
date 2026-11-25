/* common.c - name helpers for logs and the GUI. */
#include "common.h"

const char *machine_name(int m)
{
    static const char *names[NUM_MACHINES] = {
        "reception", "cleaning", "washing", "crushing", "malaxation",
        "separation"
    };
    return (m >= 0 && m < NUM_MACHINES) ? names[m] : "unknown";
}

const char *state_name(int s)
{
    static const char *names[] = {
        "IDLE", "OPERATING", "PAUSED", "ERROR", "COMPLETED"
    };
    return (s >= ST_IDLE && s <= ST_COMPLETED) ? names[s] : "?";
}

const char *fault_name(int f)
{
    static const char *names[NUM_FAULTS] = {
        "none", "overheat", "overpressure", "timeout", "machine-error"
    };
    return (f >= 0 && f < NUM_FAULTS) ? names[f] : "?";
}
