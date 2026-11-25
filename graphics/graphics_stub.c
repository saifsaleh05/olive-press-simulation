#include "graphics.h"

#include <stdio.h>

int graphics_run(int *argc, char **argv, const Config *cfg, int autostart)
{
    (void)argc;
    (void)argv;
    (void)cfg;
    (void)autostart;
    fprintf(stderr, "OpenGL GUI is unavailable in this build; use --headless\n");
    return 1;
}
