#include <GL/freeglut.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "graphics.h"
#include "supervisor.h"

/* Everything is laid out on a fixed logical canvas and scaled to the window. */
#define VIEW_W 1200.0f
#define VIEW_H 800.0f
#define FRAME_MS 33

#define BOX_W 260.0f
#define BOX_H 140.0f
#define ROW1_Y 150.0f
#define ROW2_Y 370.0f
#define COL_X(i) (60.0f + (float)(i) * 410.0f)

typedef struct { float r, g, b; } Rgb;

typedef struct {
    float x, y, w, h;
    const char *label;
    char key;
} Button;

enum { BTN_START, BTN_PAUSE, BTN_RESUME, BTN_STOP, BTN_FAULT, BTN_CLEAR, NUM_BUTTONS };

static const Button buttons[NUM_BUTTONS] = {
    {  30, 78, 110, 34, "Start",       's' },
    { 150, 78, 110, 34, "Pause",       'p' },
    { 270, 78, 110, 34, "Resume",      'r' },
    { 390, 78, 110, 34, "Stop",        'x' },
    { 530, 78, 130, 34, "Inject fault", 'f' },
    { 670, 78, 130, 34, "Clear fault", 'c' },
};

/*
 * Two row layout:  Reception -> Cleaning -> Washing
 *                                              |
 *               Separation <- Malaxation <- Crushing
 */
static const float box_pos[NUM_MACHINES][2] = {
    [M_RECEPTION]  = { COL_X(0), ROW1_Y },
    [M_CLEANING]   = { COL_X(1), ROW1_Y },
    [M_WASHING]    = { COL_X(2), ROW1_Y },
    [M_CRUSHING]   = { COL_X(2), ROW2_Y },
    [M_MALAXATION] = { COL_X(1), ROW2_Y },
    [M_SEPARATION] = { COL_X(0), ROW2_Y },
};

static const Rgb state_color[NUM_STATES] = {
    [ST_IDLE]      = { 0.62f, 0.64f, 0.66f },
    [ST_OPERATING] = { 0.22f, 0.64f, 0.32f },
    [ST_PAUSED]    = { 0.96f, 0.79f, 0.20f },
    [ST_ERROR]     = { 0.84f, 0.21f, 0.21f },
    [ST_COMPLETED] = { 0.22f, 0.45f, 0.85f },
};

static const Rgb bg       = { 0.11f, 0.12f, 0.14f };
static const Rgb panel    = { 0.17f, 0.18f, 0.21f };
static const Rgb text_hi  = { 0.94f, 0.95f, 0.96f };
static const Rgb text_lo  = { 0.64f, 0.67f, 0.71f };
static const Rgb text_dk  = { 0.10f, 0.10f, 0.11f };
static const Rgb olive    = { 0.55f, 0.60f, 0.22f };
static const Rgb arrow_c  = { 0.50f, 0.53f, 0.58f };

static const Config   *cfg;
static FactorySnapshot snap;
static int             selected = M_RECEPTION;
static int             win_w = 1200, win_h = 800;

/* ------------------------------------------------------------ primitives */

static void color(Rgb c)
{
    glColor3f(c.r, c.g, c.b);
}

static Rgb shade(Rgb c, float k)
{
    return (Rgb){ c.r * k, c.g * k, c.b * k };
}

static void rect(float x, float y, float w, float h)
{
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static void rect_outline(float x, float y, float w, float h, float width)
{
    glLineWidth(width);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
    glLineWidth(1.0f);
}

static void circle(float cx, float cy, float r)
{
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= 28; i++) {
        float a = (float)i * 2.0f * (float)M_PI / 28.0f;
        glVertex2f(cx + r * cosf(a), cy + r * sinf(a));
    }
    glEnd();
}

static void text(float x, float y, void *font, const char *s)
{
    glRasterPos2f(x, y);
    glutBitmapString(font, (const unsigned char *)s);
}

static int text_width(void *font, const char *s)
{
    return glutBitmapLength(font, (const unsigned char *)s);
}

static void textf(float x, float y, void *font, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void textf(float x, float y, void *font, const char *fmt, ...)
{
    char    buf[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    text(x, y, font, buf);
}

/* Draw at most max_w pixels of s, cutting from the end. */
static void text_clipped(float x, float y, void *font, const char *s, float max_w)
{
    char buf[LOG_WIDTH + 1];
    size_t n = strlen(s);

    if (n > LOG_WIDTH)
        n = LOG_WIDTH;
    memcpy(buf, s, n);
    buf[n] = '\0';
    while (n > 0 && (float)text_width(font, buf) > max_w)
        buf[--n] = '\0';
    text(x, y, font, buf);
}

/* Arrow from (x0,y0) to (x1,y1) with a filled head at the end. */
static void arrow(float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    float ux = dx / len, uy = dy / len;
    float hx = x1 - ux * 14.0f, hy = y1 - uy * 14.0f;

    color(arrow_c);
    glLineWidth(3.0f);
    glBegin(GL_LINES);
    glVertex2f(x0, y0);
    glVertex2f(hx, hy);
    glEnd();
    glLineWidth(1.0f);

    glBegin(GL_TRIANGLES);
    glVertex2f(x1, y1);
    glVertex2f(hx - uy * 8.0f, hy + ux * 8.0f);
    glVertex2f(hx + uy * 8.0f, hy - ux * 8.0f);
    glEnd();
}

/* --------------------------------------------------------------- pieces */

static Rgb text_on(int st)
{
    return (st == ST_PAUSED || st == ST_IDLE) ? text_dk : text_hi;
}

static void batch_token(float cx, float cy, float r, int id)
{
    char buf[12];

    color(shade(olive, 0.6f));
    circle(cx, cy, r + 2.0f);
    color(olive);
    circle(cx, cy, r);

    snprintf(buf, sizeof buf, "%d", id);
    color(text_hi);
    text(cx - (float)text_width(GLUT_BITMAP_HELVETICA_12, buf) / 2.0f, cy + 4.0f,
         GLUT_BITMAP_HELVETICA_12, buf);
}

static void draw_machine(int id)
{
    const MachineView *m = &snap.m[id];
    const MachineCfg  *mc = &cfg->m[id];
    float x = box_pos[id][0], y = box_pos[id][1];
    int   st = (m->state >= 0 && m->state < NUM_STATES) ? m->state : ST_IDLE;
    Rgb   fg = text_on(st);
    Rgb   fg_soft = st == ST_PAUSED || st == ST_IDLE ? shade(text_dk, 1.8f) : shade(text_hi, 0.85f);

    color(state_color[st]);
    rect(x, y, BOX_W, BOX_H);
    color(shade(state_color[st], 0.6f));
    rect_outline(x, y, BOX_W, BOX_H, 2.0f);
    if (id == selected) {
        color(text_hi);
        rect_outline(x - 5, y - 5, BOX_W + 10, BOX_H + 10, 3.0f);
    }

    color(fg);
    text(x + 12, y + 26, GLUT_BITMAP_HELVETICA_18, machine_name(id));
    text(x + BOX_W - 12 - (float)text_width(GLUT_BITMAP_HELVETICA_12, state_name(st)),
         y + 24, GLUT_BITMAP_HELVETICA_12, state_name(st));

    color(fg_soft);
    if (m->pid > 0)
        textf(x + 12, y + 46, GLUT_BITMAP_HELVETICA_12, "PID %d%s", (int)m->pid,
              m->alive ? "" : "  (exited)");
    else
        text(x + 12, y + 46, GLUT_BITMAP_HELVETICA_12, "not started");

    color(m->temperature > mc->max_temp * 0.92 ? (Rgb){ 0.55f, 0.0f, 0.0f } : fg);
    if (st == ST_ERROR || st == ST_COMPLETED)
        color(fg);
    textf(x + 12, y + 72, GLUT_BITMAP_HELVETICA_12, "T  %5.1f \xb0" "C  / %.0f", m->temperature,
          mc->max_temp);
    color(fg);
    textf(x + 12, y + 92, GLUT_BITMAP_HELVETICA_12, "P  %5.2f bar / %.1f", m->pressure,
          mc->max_pressure);

    if (m->batch_id > 0) {
        batch_token(x + BOX_W - 40, y + 76, 17.0f, m->batch_id);
    } else {
        color(fg_soft);
        text(x + BOX_W - 62, y + 80, GLUT_BITMAP_HELVETICA_12, "empty");
    }

    /* progress on the current batch */
    color(shade(state_color[st], 0.55f));
    rect(x + 12, y + BOX_H - 26, BOX_W - 24, 12);
    color(text_hi);
    rect(x + 12, y + BOX_H - 26, (BOX_W - 24) * (float)fmin(1.0, fmax(0.0, m->progress)), 12);
}

/* Batches written by `from` but not yet read by `to` are sitting in the pipe. */
static void draw_link(int from, int to, float x0, float y0, float x1, float y1)
{
    int   queued = snap.m[from].batches_out - snap.m[to].batches_in;
    int   shown = queued > 3 ? 3 : queued;
    float mx = (x0 + x1) / 2.0f, my = (y0 + y1) / 2.0f;
    int   vertical = fabsf(x1 - x0) < 1.0f;

    arrow(x0, y0, x1, y1);
    for (int i = 0; i < shown; i++) {
        float off = ((float)i - (float)(shown - 1) / 2.0f) * 22.0f;
        color(shade(olive, 0.6f));
        circle(vertical ? mx - 22 : mx + off, vertical ? my + off : my - 20, 9.0f);
        color(olive);
        circle(vertical ? mx - 22 : mx + off, vertical ? my + off : my - 20, 7.0f);
    }
    if (queued > 3) {
        color(text_lo);
        textf(vertical ? mx - 60 : mx + 38, vertical ? my + 4 : my - 16,
              GLUT_BITMAP_HELVETICA_12, "+%d", queued - 3);
    }
}

static void draw_flow(void)
{
    float mid1 = ROW1_Y + BOX_H / 2.0f, mid2 = ROW2_Y + BOX_H / 2.0f;
    float wash_cx = COL_X(2) + BOX_W / 2.0f;

    draw_link(M_RECEPTION, M_CLEANING, COL_X(0) + BOX_W + 6, mid1, COL_X(1) - 8, mid1);
    draw_link(M_CLEANING, M_WASHING, COL_X(1) + BOX_W + 6, mid1, COL_X(2) - 8, mid1);
    draw_link(M_WASHING, M_CRUSHING, wash_cx, ROW1_Y + BOX_H + 8, wash_cx, ROW2_Y - 10);
    draw_link(M_CRUSHING, M_MALAXATION, COL_X(2) - 6, mid2, COL_X(1) + BOX_W + 8, mid2);
    draw_link(M_MALAXATION, M_SEPARATION, COL_X(1) - 6, mid2, COL_X(0) + BOX_W + 8, mid2);

    /* oil, water and pomace leave separation into the production panel */
    arrow(COL_X(0) + BOX_W / 2.0f, ROW2_Y + BOX_H + 6, COL_X(0) + BOX_W / 2.0f, 568);

    color(text_lo);
    text(COL_X(0), ROW1_Y - 12, GLUT_BITMAP_HELVETICA_12, "olives in");
}

static const char *line_label(LineState l)
{
    switch (l) {
    case LINE_READY:    return "READY";
    case LINE_RUNNING:  return "RUNNING";
    case LINE_PAUSED:   return "PAUSED";
    case LINE_STOPPING: return "STOPPING";
    case LINE_FINISHED: return "FINISHED";
    }
    return "?";
}

static int button_enabled(int b)
{
    LineState l = snap.line;
    int       sel_faulted = (snap.faulted_mask >> selected) & 1;

    switch (b) {
    case BTN_START:  return l == LINE_READY;
    case BTN_PAUSE:  return l == LINE_RUNNING;
    case BTN_RESUME: return l == LINE_PAUSED;
    case BTN_STOP:   return l == LINE_RUNNING || l == LINE_PAUSED;
    case BTN_FAULT:  return (l == LINE_RUNNING || l == LINE_PAUSED) && !sel_faulted &&
                            snap.m[selected].alive;
    case BTN_CLEAR:  return sel_faulted;
    }
    return 0;
}

static void draw_header(void)
{
    const MachineView *qc = &snap.m[M_QC];
    Rgb  badge = snap.line == LINE_RUNNING ? state_color[ST_OPERATING]
               : snap.line == LINE_PAUSED  ? state_color[ST_PAUSED]
               : snap.line == LINE_FINISHED ? state_color[ST_COMPLETED]
               : snap.line == LINE_STOPPING ? state_color[ST_ERROR] : state_color[ST_IDLE];

    color(panel);
    rect(0, 0, VIEW_W, 62);

    color(text_hi);
    text(30, 38, GLUT_BITMAP_HELVETICA_18, "Olive Press Supervisor");

    color(badge);
    rect(250, 18, 104, 28);
    color(snap.line == LINE_PAUSED || snap.line == LINE_READY ? text_dk : text_hi);
    text(302 - (float)text_width(GLUT_BITMAP_HELVETICA_12, line_label(snap.line)) / 2.0f, 37,
         GLUT_BITMAP_HELVETICA_12, line_label(snap.line));

    color(text_lo);
    textf(372, 37, GLUT_BITMAP_HELVETICA_12, "uptime %6.1f s", snap.uptime);
    textf(492, 37, GLUT_BITMAP_HELVETICA_12, "QC: %s%s", qc->pid > 0 ? state_name(qc->state) : "-",
          qc->pid > 0 && !qc->alive ? " (exited)" : "");
    textf(650, 37, GLUT_BITMAP_HELVETICA_12, "alarms %d", snap.alarms);

    /* legend */
    for (int s = 0; s < NUM_STATES; s++) {
        float lx = 740 + (float)s * 86;
        color(state_color[s]);
        rect(lx, 24, 14, 14);
        color(text_lo);
        text(lx + 20, 36, GLUT_BITMAP_HELVETICA_10, state_name(s));
    }

    for (int b = 0; b < NUM_BUTTONS; b++) {
        const Button *bt = &buttons[b];
        int en = button_enabled(b);

        color(en ? (b == BTN_FAULT ? (Rgb){ 0.55f, 0.20f, 0.20f } : (Rgb){ 0.27f, 0.30f, 0.36f })
                 : (Rgb){ 0.19f, 0.20f, 0.23f });
        rect(bt->x, bt->y, bt->w, bt->h);
        color(en ? text_hi : shade(text_lo, 0.6f));
        textf(bt->x + 12, bt->y + 22, GLUT_BITMAP_HELVETICA_12, "%s  [%c]", bt->label,
              bt->key - 32);
    }

    color(text_lo);
    textf(820, 100, GLUT_BITMAP_HELVETICA_12, "selected: %s   (click a machine or 1-6)",
          machine_name(selected));
}

static void panel_box(float x, float y, float w, float h, const char *title)
{
    color(panel);
    rect(x, y, w, h);
    color(text_hi);
    text(x + 14, y + 24, GLUT_BITMAP_HELVETICA_12, title);
    color(shade(panel, 1.5f));
    glBegin(GL_LINES);
    glVertex2f(x + 14, y + 33);
    glVertex2f(x + w - 14, y + 33);
    glEnd();
}

static void row(float x, float y, const char *k, const char *v)
{
    color(text_lo);
    text(x, y, GLUT_BITMAP_HELVETICA_12, k);
    color(text_hi);
    text(x + 120, y, GLUT_BITMAP_HELVETICA_12, v);
}

static void draw_production(float x, float y, float w, float h)
{
    char v[64];
    double yield = snap.mass_in > 0.0 ? 100.0 * snap.oil / snap.mass_in : 0.0;

    panel_box(x, y, w, h, "Production");
    snprintf(v, sizeof v, "%d / %d", snap.produced, cfg->num_batches);
    row(x + 14, y + 56, "batches done", v);
    snprintf(v, sizeof v, "%.1f kg", snap.mass_in);
    row(x + 14, y + 76, "olives in", v);
    snprintf(v, sizeof v, "%.1f kg  (%.1f %%)", snap.oil, yield);
    row(x + 14, y + 96, "oil", v);
    snprintf(v, sizeof v, "%.1f kg", snap.water);
    row(x + 14, y + 116, "vegetable water", v);
    snprintf(v, sizeof v, "%.1f kg", snap.pomace);
    row(x + 14, y + 136, "pomace", v);
    snprintf(v, sizeof v, "%.1f kg", snap.losses);
    row(x + 14, y + 156, "debris removed", v);
    snprintf(v, sizeof v, "%.2e kg", snap.balance_error);
    row(x + 14, y + 176, "mass balance err", v);
    snprintf(v, sizeof v, "%d", snap.qc_dropped);
    row(x + 14, y + 196, "QC samples lost", v);
}

static void draw_details(float x, float y, float w, float h)
{
    const MachineView *m = &snap.m[selected];
    const MachineCfg  *mc = &cfg->m[selected];
    char v[80], title[48];

    snprintf(title, sizeof title, "Details: %s", machine_name(selected));
    panel_box(x, y, w, h, title);

    if (m->pid > 0)
        snprintf(v, sizeof v, "%d  %s", (int)m->pid, m->alive ? "running" : "exited");
    else
        snprintf(v, sizeof v, "-");
    row(x + 14, y + 56, "pid", v);
    row(x + 14, y + 76, "state", state_name(m->state));
    if (m->batch_id > 0)
        snprintf(v, sizeof v, "#%d  (%.0f %%)", m->batch_id, m->progress * 100.0);
    else
        snprintf(v, sizeof v, "none");
    row(x + 14, y + 96, "batch", v);
    snprintf(v, sizeof v, "%.2f \xb0" "C   limit %.1f", m->temperature, mc->max_temp);
    row(x + 14, y + 116, "temperature", v);
    snprintf(v, sizeof v, "%.2f bar   limit %.1f", m->pressure, mc->max_pressure);
    row(x + 14, y + 136, "pressure", v);
    snprintf(v, sizeof v, "%.1f s", m->active_time);
    row(x + 14, y + 156, "active time", v);
    snprintf(v, sizeof v, "%d in / %d out", m->batches_in, m->batches_out);
    row(x + 14, y + 176, "batches", v);
    if (m->fault != FAULT_NONE)
        snprintf(v, sizeof v, "%s%s", fault_name(m->fault),
                 (snap.faulted_mask >> selected) & 1 ? "  (active)" : "");
    else
        snprintf(v, sizeof v, "none");
    row(x + 14, y + 196, "last fault", v);
}

static void draw_log(float x, float y, float w, float h)
{
    int fit = (int)((h - 46.0f) / 13.5f);
    int n = snap.log_total < LOG_LINES ? snap.log_total : LOG_LINES;
    int first;

    if (n > fit)
        n = fit;
    first = snap.log_total - n;

    panel_box(x, y, w, h, "Event log");
    for (int i = 0; i < n; i++) {
        const char *line = snap.log[(first + i) % LOG_LINES];
        color(strstr(line, "ALARM") || strstr(line, "fault") ? (Rgb){ 0.95f, 0.55f, 0.50f }
                                                             : text_lo);
        text_clipped(x + 14, y + 52 + (float)i * 13.5f, GLUT_BITMAP_HELVETICA_10, line, w - 28);
    }
}

static void display(void)
{
    glClearColor(bg.r, bg.g, bg.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    draw_header();
    draw_flow();
    for (int id = 0; id < NUM_MACHINES; id++)
        draw_machine(id);

    draw_production(30, 570, 330, 214);
    draw_details(380, 570, 360, 214);
    draw_log(760, 570, 410, 214);

    glutSwapBuffers();
}

/* -------------------------------------------------------------- control */

static void quit(void)
{
    glutLeaveMainLoop();
}

static void do_action(char key)
{
    switch (key) {
    case 's': if (supervisor_start() < 0 && snap.line == LINE_READY)
                  supervisor_log("could not start the line");
              break;
    case 'p': supervisor_pause(); break;
    case 'r': supervisor_resume(); break;
    case 'x': supervisor_stop(); break;
    case 'f': supervisor_inject_fault(selected); break;
    case 'c': supervisor_clear_fault(selected); break;
    case 'q': quit(); break;
    }
    supervisor_snapshot(&snap);
    glutPostRedisplay();
}

static void keyboard(unsigned char key, int x, int y)
{
    (void)x;
    (void)y;

    if (key >= '1' && key <= '6') {
        selected = key - '1';
        glutPostRedisplay();
        return;
    }
    if (key == 27)
        key = 'q';
    if (key >= 'A' && key <= 'Z')
        key = (unsigned char)(key + 32);
    do_action((char)key);
}

static void mouse(int button, int st, int mx, int my)
{
    float x = (float)mx * VIEW_W / (float)win_w;
    float y = (float)my * VIEW_H / (float)win_h;

    if (button != GLUT_LEFT_BUTTON || st != GLUT_DOWN)
        return;

    for (int b = 0; b < NUM_BUTTONS; b++) {
        const Button *bt = &buttons[b];
        if (x >= bt->x && x <= bt->x + bt->w && y >= bt->y && y <= bt->y + bt->h) {
            if (button_enabled(b))
                do_action(bt->key);
            return;
        }
    }
    for (int id = 0; id < NUM_MACHINES; id++) {
        if (x >= box_pos[id][0] && x <= box_pos[id][0] + BOX_W &&
            y >= box_pos[id][1] && y <= box_pos[id][1] + BOX_H) {
            selected = id;
            glutPostRedisplay();
            return;
        }
    }
}

static void reshape(int w, int h)
{
    win_w = w > 0 ? w : 1;
    win_h = h > 0 ? h : 1;
    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, VIEW_W, VIEW_H, 0);   /* y grows downwards like the mouse */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void tick(int unused)
{
    (void)unused;
    supervisor_poll(0);     /* never blocks: poll() with a zero timeout */
    supervisor_snapshot(&snap);

    /* Ctrl-C / SIGTERM: the line has been shut down, close the window too */
    if (supervisor_stop_signalled() && supervisor_finished()) {
        quit();
        return;
    }
    glutPostRedisplay();
    glutTimerFunc(FRAME_MS, tick, 0);
}

int graphics_run(int *argc, char **argv, const Config *config, int autostart)
{
    cfg = config;

    glutInit(argc, argv);
    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE, GLUT_ACTION_GLUTMAINLOOP_RETURNS);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_MULTISAMPLE);
    glutInitWindowSize(win_w, win_h);
    glutCreateWindow("Olive Press Supervisor");

    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    glutMouseFunc(mouse);
    glutTimerFunc(FRAME_MS, tick, 0);

    if (autostart)
        supervisor_start();
    supervisor_snapshot(&snap);

    glutMainLoop();
    return 0;
}
