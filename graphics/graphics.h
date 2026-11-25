#ifndef OLIVE_GRAPHICS_H
#define OLIVE_GRAPHICS_H

#include "config.h"

/*
 * Runs the GLUT main loop in the supervisor process. All data comes from
 * supervisor_snapshot(); the only I/O done from GLUT callbacks is the
 * non blocking supervisor_poll(0) in the timer.
 *
 * autostart: fork the line immediately instead of waiting for 'S'.
 * Returns when the window is closed or the operator quits.
 */
int graphics_run(int *argc, char **argv, const Config *cfg, int autostart);

#endif
