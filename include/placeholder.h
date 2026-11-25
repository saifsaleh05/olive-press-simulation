#ifndef OLIVE_PLACEHOLDER_H
#define OLIVE_PLACEHOLDER_H

#include "common.h"

int run_passthrough_stage(ProcessRole role, Stage stage,
                          int input_fd, int output_fd);
int run_separation_placeholder(int input_fd);
int run_quality_control_placeholder(void);

#endif
