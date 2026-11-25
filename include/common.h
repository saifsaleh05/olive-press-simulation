/*
 * common.h - shared data types for the olive-press simulation.
 *
 * SHARED CONTRACT: every process (Supervisor + 7 children) depends on these
 * structures. Do not change a field, an enum value or a struct size without
 * telling the whole team: all messages are sent through pipes as raw
 * fixed-size structs, so both ends must be compiled from the same header.
 */
#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* Machines                                                           */
/* ------------------------------------------------------------------ */

/* Production machines, in pipeline order. Quality Control is a process,
 * not a machine on the line, so it has no MachineId and sends no telemetry. */
typedef enum {
    M_RECEPTION = 0,
    M_CLEANING,
    M_WASHING,
    M_CRUSHING,
    M_MALAXATION,
    M_SEPARATION,
    NUM_MACHINES
} MachineId;

/* Actual state of a machine. The GUI colours: IDLE=gray, OPERATING=green,
 * PAUSED=yellow, ERROR=red, COMPLETED=blue. */
typedef enum {
    ST_IDLE = 0,
    ST_OPERATING,
    ST_PAUSED,
    ST_ERROR,
    ST_COMPLETED,
    NUM_STATES
} MachineState;

/* Seven children are forked: six machines plus the Quality Control process. */
#define M_QC NUM_MACHINES
#define NUM_PROCS (NUM_MACHINES + 1)
#define NUM_BATCH_PIPES (NUM_MACHINES - 1)

/* ------------------------------------------------------------------ */
/* Batch: travels through the five stage-to-stage pipes               */
/* ------------------------------------------------------------------ */

typedef struct {
    int    id;                 /* unique, assigned by Reception          */
    double original_mass;      /* kg of olives when the batch was created */
    double current_mass;       /* kg after the losses of earlier stages   */
    double impurity_fraction;  /* 0..1, removed by Cleaning/Washing       */
    double oil_potential;      /* 0..1, fraction of paste mass that is oil*/
    double quality;            /* 0..1, degraded by overheating etc.      */
    int    stage;              /* MachineId of the last stage that handled it */
} Batch;

/* ------------------------------------------------------------------ */
/* Telemetry: every machine -> Quality Control                        */
/* ------------------------------------------------------------------ */

typedef enum {
    EV_HEARTBEAT = 0,   /* periodic reading, nothing special happened   */
    EV_STATE_CHANGE,    /* state field differs from the previous event  */
    EV_BATCH_START,     /* machine started processing batch_id          */
    EV_BATCH_DONE,      /* machine forwarded batch_id to the next stage */
    EV_SHUTDOWN         /* last event before the process exits          */
} EventType;

typedef struct {
    int          machine;      /* MachineId                                  */
    pid_t        pid;
    int          batch_id;     /* -1 when no batch is being processed        */
    int          state;        /* MachineState                               */
    double       temperature;  /* deg C (sensor reading)                     */
    double       pressure;     /* bar  (sensor reading)                      */
    double       active_time;  /* s spent OPERATING since start-up           */
    double       batch_time;   /* s spent OPERATING on the current batch
                                  (paused / failed time is NOT counted)      */
    double       progress;     /* 0..1 of the current batch                  */
    int          event;        /* EventType                                  */
    unsigned int batches_done; /* batches forwarded so far                   */
} MachineEvent;

/* ------------------------------------------------------------------ */
/* Alarms: Quality Control -> Supervisor                              */
/* ------------------------------------------------------------------ */

typedef enum {
    FAULT_NONE = 0,
    FAULT_OVERHEAT,        /* temperature > max_temperature                */
    FAULT_OVERPRESSURE,    /* pressure    > max_pressure                   */
    FAULT_TIMEOUT,         /* batch_time  > max_batch_time (slow machine)  */
    FAULT_MACHINE_ERROR,   /* machine reports ST_ERROR (failure)           */
    FAULT_MANUAL,          /* operator injected failure                    */
    FAULT_CHILD_DIED,      /* Supervisor observed an abnormal child exit   */
    NUM_FAULTS
} FaultCode;

typedef enum {
    ALARM_RAISED = 0,      /* fault started -> Supervisor shows red        */
    ALARM_CLEARED          /* fault recovered -> Supervisor may resume     */
} AlarmKind;

typedef struct {
    int    machine;        /* MachineId                                    */
    int    batch_id;
    int    fault;          /* FaultCode                                    */
    int    kind;           /* AlarmKind                                    */
    double measured;       /* value that crossed the limit                 */
    double limit;          /* limit (RAISED) or recovery threshold (CLEARED)*/
} Alarm;

/* ------------------------------------------------------------------ */
/* Production result: Separation -> Supervisor                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int    batch_id;
    double original_mass;  /* kg of olives received                        */
    double input_mass;     /* kg of paste entering Separation              */
    double losses;         /* original_mass - input_mass (debris, dirt...) */
    double oil;            /* kg                                           */
    double water;          /* kg of vegetable water                        */
    double pomace;         /* kg of solid pomace                           */
    double quality;        /* 0..1, copied from the batch                  */
} ProductionResult;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

const char *machine_name(int m);
const char *state_name(int s);
const char *fault_name(int f);

#endif /* COMMON_H */
