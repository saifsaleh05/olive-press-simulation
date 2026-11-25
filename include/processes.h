/*
 * processes.h - entry points of the child processes.
 *
 * Each *_process() function is called in a child right after fork(), once
 * the child has closed every pipe end it does not use. It returns the exit
 * status for the child (0 = success) and closes the fds it was given.
 */
#ifndef PROCESSES_H
#define PROCESSES_H

#include "common.h"
#include "config.h"

/* ---------------- Washing (owner: Yanal) -------------------------- */
/* Pure batch transformation used by the process and unit tests.
 * loss_u is clamped to 0..1 and selects a configured washing-loss value. */
double washing_apply_loss(Batch *batch, const Config *cfg, double loss_u);

/* in_fd:    Batch from Cleaning
 * out_fd:   washed Batch to Crushing
 * telem_fd: MachineEvent to Quality Control (-1 to disable) */
int washing_process(const Config *cfg, int in_fd, int out_fd, int telem_fd);

/* ---------------- Crushing (owner: Yanal) ------------------------- */
/* Crushing conserves mass; stage=M_CRUSHING identifies the Batch payload as
 * paste when it is forwarded to Malaxation. Processing duration is computed
 * by the shared load model. */
int crushing_process(const Config *cfg, int in_fd, int out_fd, int telem_fd);

/* ---------------- Malaxation (owner: Ibrahim) ---------------------- */
/* in_fd:    Batch from Crushing
 * out_fd:   Batch to Separation
 * telem_fd: MachineEvent to Quality Control (-1 to disable) */
int malaxation_process(const Config *cfg, int in_fd, int out_fd, int telem_fd);

/* ---------------- Separation (owner: Ibrahim) ---------------------- */
/* Three-phase mass balance, used by the process and by the tests.
 *   oil    = paste * oil_potential * efficiency
 *            efficiency = EXTRACTION_EFFICIENCY * (1 - w + w * quality)
 *   water  = paste * veg_water, veg_water = VEG_WATER range at water_u (0..1)
 *   pomace = paste - oil - water   (unrecovered oil stays in the pomace)
 * so paste == oil + water + pomace and
 *    original_mass == losses + oil + water + pomace. */
void separation_compute(const Batch *b, const Config *cfg, double water_u,
                        ProductionResult *r);

/* in_fd:     Batch from Malaxation
 * result_fd: ProductionResult to Supervisor (-1 to disable)
 * telem_fd:  MachineEvent to Quality Control (-1 to disable) */
int separation_process(const Config *cfg, int in_fd, int result_fd,
                       int telem_fd);

/* ---------------- Quality Control (owner: Ibrahim) ----------------- */
typedef struct {
    unsigned char active[NUM_MACHINES][NUM_FAULTS]; /* 1 = alarm raised     */
    int           timeout_batch[NUM_MACHINES];      /* batch that timed out */
    unsigned long raised_count;
    unsigned long cleared_count;
    unsigned long events_seen;
} QcState;

void qc_init(QcState *qc);

/* Evaluate one telemetry event against the per-machine limits in cfg.
 * Writes the alarm transitions it causes into out[] and returns how many.
 * An alarm is reported once when the fault starts (ALARM_RAISED) and once
 * when it recovers (ALARM_CLEARED), never repeated while it persists. */
int qc_evaluate(QcState *qc, const Config *cfg, const MachineEvent *ev,
                Alarm out[NUM_FAULTS]);

/* telem_fd:   MachineEvent from all six machines (shared pipe)
 * alarm_fd:   Alarm to Supervisor
 * forward_fd: if >= 0, every MachineEvent is copied there so the Supervisor
 *             gets the status feed for the GUI; otherwise -1.
 * Runs until EOF on telem_fd (all machines exited) or SIGTERM. */
int quality_control_process(const Config *cfg, int telem_fd, int alarm_fd,
                            int forward_fd);

#endif /* PROCESSES_H */
