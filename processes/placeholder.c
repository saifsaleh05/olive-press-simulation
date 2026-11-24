#include "placeholder.h"

#include "ipc.h"

#include <stdio.h>
#include <unistd.h>

int run_passthrough_stage(ProcessRole role, Stage stage,
                          int input_fd, int output_fd)
{
    for (;;) {
        Batch batch;
        ssize_t received = read_full(input_fd, &batch, sizeof(batch));
        if (received == 0) {
            fprintf(stdout, "[%s placeholder pid=%ld] EOF; exiting\n",
                    process_role_name(role), (long)getpid());
            return 0;
        }
        if (received == -1) {
            perror("placeholder: read batch");
            return 1;
        }
        batch.stage = stage;
        fprintf(stdout, "[%s placeholder] Batch %d forwarded\n",
                process_role_name(role), batch.id);
        if (write_full(output_fd, &batch, sizeof(batch)) == -1) {
            perror("placeholder: write batch");
            return 1;
        }
    }
}

int run_separation_placeholder(int input_fd)
{
    int count = 0;
    for (;;) {
        Batch batch;
        ssize_t received = read_full(input_fd, &batch, sizeof(batch));
        if (received == 0) {
            fprintf(stdout,
                    "[Separation placeholder pid=%ld] EOF after %d batches; exiting\n",
                    (long)getpid(), count);
            return 0;
        }
        if (received == -1) {
            perror("Separation placeholder: read batch");
            return 1;
        }
        ++count;
        fprintf(stdout, "[Separation placeholder] Batch %d received\n", batch.id);
    }
}

int run_quality_control_placeholder(void)
{
    fprintf(stdout,
            "[Quality Control placeholder pid=%ld] Telemetry channel pending integration\n",
            (long)getpid());
    return 0;
}
