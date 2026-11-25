/*
 * Sequential vs OpenMP timing for the lab side of the press.
 *
 * Every sample is one olive lot sent for analysis. For each lot we integrate
 * a malaxation model with RK4: oil droplets coalesce at an Arrhenius rate
 * k(T) = A exp(-Ea / R T) toward the lot's oil potential, while phenolic
 * quality decays faster the warmer the paste gets. The temperature follows
 * the machine's own heating/cooling curve, so the work per sample is a long
 * chain of exp() calls with no shared state: an embarrassingly parallel,
 * CPU bound loop, which is exactly what OpenMP is good at.
 *
 * Each sample's inputs come from a hash of (seed, index), not from a shared
 * random stream, so the parallel loop produces bit-identical per-sample
 * results whatever the thread count or schedule. The totals are reductions,
 * so they may differ in the last bits; those are compared with a tolerance.
 */
#include <math.h>
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R_GAS   8.314
#define EA      42000.0     /* J/mol, activation energy of coalescence */
#define A_RATE  2.6e6       /* 1/min */
#define KQ      0.0009      /* quality loss per minute per degree over 27 C */

typedef struct {
    double yield;           /* kg oil per kg paste */
    double quality;         /* 0..100 */
} SampleResult;

typedef struct {
    int      samples;
    int      steps;
    int      threads;
    int      repeats;
    unsigned seed;
    int      sweep;
} Options;

static uint64_t splitmix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static double unit(uint64_t *s)
{
    *s = splitmix(*s);
    return (double)(*s >> 11) * (1.0 / 9007199254740992.0);
}

static inline double rate(double temp_c)
{
    return A_RATE * exp(-EA / (R_GAS * (temp_c + 273.15)));
}

/* dY/dt = k(T)(Ymax - Y), dQ/dt = -KQ * Q * max(0, T - 27) */
static inline void deriv(double t, const double y[2], double out[2],
                         double ymax, double t_amb, double t_rise, double tau)
{
    double temp = t_amb + t_rise * (1.0 - exp(-t / tau));
    double over = temp > 27.0 ? temp - 27.0 : 0.0;

    out[0] = rate(temp) * (ymax - y[0]);
    out[1] = -KQ * y[1] * over;
}

__attribute__((noinline))
static SampleResult analyse_sample(unsigned seed, int index, int steps)
{
    uint64_t s = splitmix(((uint64_t)seed << 32) ^ (uint64_t)index);
    double   ymax   = 0.16 + 0.06 * unit(&s);     /* oil potential */
    double   t_amb  = 18.0 + 8.0 * unit(&s);      /* hall temperature */
    double   t_rise = 3.0 + 9.0 * unit(&s);       /* machine heating */
    double   tau    = 8.0 + 10.0 * unit(&s);      /* minutes */
    double   minutes = 40.0 + 20.0 * unit(&s);
    double   h = minutes / steps;
    double   y[2] = { 0.0, 100.0 };

    for (int i = 0; i < steps; i++) {
        double t = i * h, k1[2], k2[2], k3[2], k4[2], tmp[2];

        deriv(t, y, k1, ymax, t_amb, t_rise, tau);
        tmp[0] = y[0] + 0.5 * h * k1[0];  tmp[1] = y[1] + 0.5 * h * k1[1];
        deriv(t + 0.5 * h, tmp, k2, ymax, t_amb, t_rise, tau);
        tmp[0] = y[0] + 0.5 * h * k2[0];  tmp[1] = y[1] + 0.5 * h * k2[1];
        deriv(t + 0.5 * h, tmp, k3, ymax, t_amb, t_rise, tau);
        tmp[0] = y[0] + h * k3[0];        tmp[1] = y[1] + h * k3[1];
        deriv(t + h, tmp, k4, ymax, t_amb, t_rise, tau);

        y[0] += h / 6.0 * (k1[0] + 2.0 * k2[0] + 2.0 * k3[0] + k4[0]);
        y[1] += h / 6.0 * (k1[1] + 2.0 * k2[1] + 2.0 * k3[1] + k4[1]);
    }
    return (SampleResult){ y[0], y[1] };
}

static double run_sequential(const Options *o, SampleResult *out, double *total)
{
    double t0 = omp_get_wtime(), sum = 0.0;

    for (int i = 0; i < o->samples; i++) {
        out[i] = analyse_sample(o->seed, i, o->steps);
        sum += out[i].yield;
    }
    *total = sum;
    return omp_get_wtime() - t0;
}

static double run_parallel(const Options *o, int threads, SampleResult *out, double *total)
{
    double t0 = omp_get_wtime(), sum = 0.0;

    /* static: every sample costs the same, so equal chunks balance well */
    #pragma omp parallel for num_threads(threads) schedule(static) reduction(+:sum)
    for (int i = 0; i < o->samples; i++) {
        out[i] = analyse_sample(o->seed, i, o->steps);
        sum += out[i].yield;
    }
    *total = sum;
    return omp_get_wtime() - t0;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_double);
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-n samples] [-s steps] [-t threads] [-r repeats] [-S seed] [--sweep]\n"
            "  defaults: -n 4000 -s 4000 -t <all cores> -r 5 -S 2026\n"
            "  --sweep   time 1, 2, 4 ... up to -t threads\n", prog);
}

static int parse_args(int argc, char **argv, Options *o)
{
    o->samples = 4000;
    o->steps   = 4000;
    o->threads = omp_get_max_threads();
    o->repeats = 5;
    o->seed    = 2026;
    o->sweep   = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (!strcmp(a, "--sweep")) { o->sweep = 1; continue; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); exit(0); }
        if (!v) { usage(argv[0]); return -1; }

        if      (!strcmp(a, "-n")) o->samples = atoi(v);
        else if (!strcmp(a, "-s")) o->steps   = atoi(v);
        else if (!strcmp(a, "-t")) o->threads = atoi(v);
        else if (!strcmp(a, "-r")) o->repeats = atoi(v);
        else if (!strcmp(a, "-S")) o->seed    = (unsigned)strtoul(v, NULL, 10);
        else { usage(argv[0]); return -1; }
        i++;
    }
    if (o->samples < 1 || o->steps < 1 || o->threads < 1 || o->repeats < 1) {
        fprintf(stderr, "all sizes must be positive\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    Options       o;
    SampleResult *seq, *par;
    double       *times, seq_total = 0.0, seq_time;
    int           ok_all = 1;

    if (parse_args(argc, argv, &o) < 0)
        return 1;

    seq   = malloc(sizeof *seq * (size_t)o.samples);
    par   = malloc(sizeof *par * (size_t)o.samples);
    times = malloc(sizeof *times * (size_t)o.repeats);
    if (!seq || !par || !times) {
        perror("malloc");
        return 1;
    }

    printf("OpenMP benchmark: malaxation RK4 analysis per olive sample\n");
    printf("  samples        %d\n", o.samples);
    printf("  RK4 steps      %d per sample  (%.1f M exp() calls per run)\n",
           o.steps, 4.0 * 2.0 * o.samples * (double)o.steps / 1e6);
    printf("  repeats        %d (median reported)\n", o.repeats);
    printf("  cores visible  %d\n", omp_get_num_procs());
    printf("  seed           %u\n\n", o.seed);

    run_sequential(&o, seq, &seq_total);    /* warm up caches and the CPU clock */
    for (int r = 0; r < o.repeats; r++)
        times[r] = run_sequential(&o, seq, &seq_total);
    seq_time = median(times, o.repeats);

    printf("%8s %12s %12s %9s %11s  %s\n",
           "threads", "seq [s]", "omp [s]", "speedup", "efficiency", "check");

    for (int th = o.sweep ? 1 : o.threads; th <= o.threads; th = o.sweep ? th * 2 : th + 1) {
        double par_total = 0.0, par_time, rel;
        int    same;

        run_parallel(&o, th, par, &par_total);
        for (int r = 0; r < o.repeats; r++)
            times[r] = run_parallel(&o, th, par, &par_total);
        par_time = median(times, o.repeats);

        same = memcmp(seq, par, sizeof *seq * (size_t)o.samples) == 0;
        rel = fabs(par_total - seq_total) / fabs(seq_total);
        same = same && rel < 1e-12;
        ok_all &= same;

        printf("%8d %12.6f %12.6f %8.2fx %10.0f%%  %s\n", th, seq_time, par_time,
               seq_time / par_time, 100.0 * seq_time / par_time / th,
               same ? "match" : "MISMATCH");

        if (o.sweep && th < o.threads && th * 2 > o.threads)
            th = o.threads / 2;     /* make sure the last row is exactly -t */
    }

    printf("\nmean yield %.4f kg oil / kg paste, checksum %.12f\n",
           seq_total / o.samples, seq_total);
    printf("results %s\n", ok_all ? "identical between sequential and OpenMP runs"
                                  : "DIFFER between sequential and OpenMP runs");

    free(seq);
    free(par);
    free(times);
    return ok_all ? 0 : 1;
}
