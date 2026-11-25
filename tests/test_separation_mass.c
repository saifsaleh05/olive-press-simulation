/*
 * test_separation_mass.c - mass balance of Separation (owner: Ibrahim).
 *
 * Checks, for many generated batches that already lost mass in Cleaning and
 * Washing, that
 *     paste (input of Separation) == oil + vegetable water + pomace
 *     original olives             == losses + oil + water + pomace
 * within floating-point tolerance, that no output is negative, plus edge
 * cases. The bulk check runs once sequentially and once with OpenMP and the
 * two timings are compared.
 *
 * Usage: ./build/test_separation_mass [config.txt] [num_batches]
 */
#include "config.h"
#include "processes.h"

#include <math.h>
#ifdef _OPENMP
#include <omp.h>
#else
#include <time.h>
static double omp_get_wtime(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}
static int omp_get_max_threads(void) { return 1; }
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

static double uniform(unsigned int *seed, Range r)
{
    return r.min + (r.max - r.min) * ((double)rand_r(seed) / RAND_MAX);
}

/* A batch as it reaches Separation: olives minus Cleaning/Washing losses. */
static Batch make_batch(const Config *cfg, unsigned int *seed, int id)
{
    Batch b;

    memset(&b, 0, sizeof b);
    b.id                = id;
    b.original_mass     = uniform(seed, cfg->batch_size_kg);
    b.impurity_fraction = uniform(seed, cfg->impurity);
    b.oil_potential     = uniform(seed, cfg->oil_potential);
    b.quality           = uniform(seed, cfg->quality);
    b.current_mass      = b.original_mass;
    b.current_mass     *= 1.0 - uniform(seed, cfg->cleaning_loss);
    b.current_mass     *= 1.0 - uniform(seed, cfg->washing_loss);
    b.stage             = M_MALAXATION;
    return b;
}

/* Relative balance error of one result (0 when perfect). */
static double balance_error(const ProductionResult *r)
{
    double out   = r->oil + r->water + r->pomace;
    double e1    = fabs(r->input_mass - out);
    double e2    = fabs(r->original_mass - (r->losses + out));
    double scale = r->original_mass > 1.0 ? r->original_mass : 1.0;

    return (e1 > e2 ? e1 : e2) / scale;
}

static int negative(const ProductionResult *r)
{
    return r->oil < 0 || r->water < 0 || r->pomace < 0 || r->losses < 0;
}

static void edge_cases(const Config *base)
{
    Config cfg = *base;
    ProductionResult r;
    Batch b;

    printf("edge cases\n");

    memset(&b, 0, sizeof b);                      /* empty batch */
    separation_compute(&b, &cfg, 0.5, &r);
    CHECK(r.oil == 0 && r.water == 0 && r.pomace == 0, "empty batch");

    b = (Batch){ .id = 2, .original_mass = 25, .current_mass = 24,
                 .oil_potential = 0.0, .quality = 1.0 };
    separation_compute(&b, &cfg, 0.5, &r);        /* no oil at all */
    CHECK(r.oil == 0 && balance_error(&r) < 1e-12 && !negative(&r),
          "zero oil potential");

    b.oil_potential = 1.0;                        /* absurd potential */
    separation_compute(&b, &cfg, 1.0, &r);
    CHECK(!negative(&r) && balance_error(&r) < 1e-12,
          "oil_potential 1.0 -> pomace %.3f", r.pomace);

    b.oil_potential = 0.2;
    b.quality = 0.0;
    separation_compute(&b, &cfg, 0.0, &r);
    {
        double low = r.oil;
        b.quality = 1.0;
        separation_compute(&b, &cfg, 0.0, &r);
        CHECK(r.oil > low, "better quality must give more oil");
        CHECK(fabs(r.oil - 24 * 0.2 * cfg.extraction_efficiency) < 1e-9,
              "quality 1 -> full efficiency (%.4f)", r.oil);
    }

    cfg.extraction_efficiency = 0.0;              /* broken decanter */
    separation_compute(&b, &cfg, 0.5, &r);
    CHECK(r.oil == 0 && balance_error(&r) < 1e-12,
          "efficiency 0 -> all oil stays in pomace");
    cfg = *base;

    b = (Batch){ .id = 3, .original_mass = 20, .current_mass = 22,
                 .oil_potential = 0.2, .quality = 0.8 };
    separation_compute(&b, &cfg, 0.5, &r);        /* corrupt: grew mass */
    CHECK(r.losses == 0 && !negative(&r) && balance_error(&r) < 1e-12,
          "current > original must not give negative losses");

    b = (Batch){ .id = 4, .original_mass = 25, .current_mass = -3,
                 .oil_potential = 0.2, .quality = 0.8 };
    separation_compute(&b, &cfg, 0.5, &r);        /* corrupt: negative */
    CHECK(r.input_mass == 0 && !negative(&r), "negative mass clamped to 0");

    b = (Batch){ .id = 5, .original_mass = 25, .current_mass = 24,
                 .oil_potential = 0.2, .quality = 0.8 };
    separation_compute(&b, &cfg, 0.0, &r);
    CHECK(fabs(r.water - 24 * cfg.veg_water.min) < 1e-9, "water at range min");
    separation_compute(&b, &cfg, 1.0, &r);
    CHECK(fabs(r.water - 24 * cfg.veg_water.max) < 1e-9, "water at range max");
}

int main(int argc, char **argv)
{
    Config cfg;
    char err[128];
    int n = argc > 2 ? atoi(argv[2]) : 2000000;
    Batch *batches;
    double *water_u;
    ProductionResult *res_seq, *res_par;
    double t0, t_seq, t_par;
    double err_seq = 0, err_par = 0, oil_seq = 0, oil_par = 0;
    int neg_seq = 0, neg_par = 0;
    unsigned int seed;

    config_defaults(&cfg);
    if (argc > 1 && config_load(&cfg, argv[1]) != 0)
        return 2;
    if (config_validate(&cfg, err, sizeof err) != 0) {
        fprintf(stderr, "invalid config: %s\n", err);
        return 2;
    }
    if (n < 10)
        n = 10;

    printf("== Separation mass-balance test (%d batches, seed %u) ==\n",
           n, cfg.seed);

    batches = malloc(sizeof *batches * (size_t)n);
    water_u = malloc(sizeof *water_u * (size_t)n);
    res_seq = malloc(sizeof *res_seq * (size_t)n);
    res_par = malloc(sizeof *res_par * (size_t)n);
    if (!batches || !water_u || !res_seq || !res_par) {
        perror("malloc");
        return 2;
    }

    /* deterministic inputs, generated once */
    seed = cfg.seed;
    for (int i = 0; i < n; i++) {
        batches[i] = make_batch(&cfg, &seed, i + 1);
        water_u[i] = (double)rand_r(&seed) / RAND_MAX;
    }

    printf("sample batches\n");
    printf("  %5s %8s %8s %7s %6s %6s | %7s %7s %7s %7s | %s\n", "batch",
           "olives", "paste", "losses", "oil%", "qual", "oil", "water",
           "pomace", "sum", "error");
    for (int i = 0; i < 6; i++) {
        ProductionResult r;
        separation_compute(&batches[i], &cfg, water_u[i], &r);
        printf("  %5d %8.3f %8.3f %7.3f %5.1f%% %5.0f%% | %7.3f %7.3f %7.3f "
               "%7.3f | %.1e\n", r.batch_id, r.original_mass, r.input_mass,
               r.losses, batches[i].oil_potential * 100, r.quality * 100,
               r.oil, r.water, r.pomace, r.oil + r.water + r.pomace,
               balance_error(&r));
    }

    /* sequential */
    t0 = omp_get_wtime();
    for (int i = 0; i < n; i++) {
        double e;
        separation_compute(&batches[i], &cfg, water_u[i], &res_seq[i]);
        e = balance_error(&res_seq[i]);
        if (e > err_seq)
            err_seq = e;
        neg_seq += negative(&res_seq[i]);
        oil_seq += res_seq[i].oil;
    }
    t_seq = omp_get_wtime() - t0;

    /* OpenMP */
    t0 = omp_get_wtime();
    #pragma omp parallel for reduction(max:err_par) reduction(+:neg_par, oil_par)
    for (int i = 0; i < n; i++) {
        double e;
        separation_compute(&batches[i], &cfg, water_u[i], &res_par[i]);
        e = balance_error(&res_par[i]);
        if (e > err_par)
            err_par = e;
        neg_par += negative(&res_par[i]);
        oil_par += res_par[i].oil;
    }
    t_par = omp_get_wtime() - t0;

    printf("bulk check\n");
    printf("  max relative balance error: %.3e (limit 1e-12)\n", err_seq);
    printf("  negative outputs:           %d\n", neg_seq);
    printf("  average oil per batch:      %.4f kg\n", oil_seq / n);
    CHECK(err_seq < 1e-12, "mass not conserved: %.3e", err_seq);
    CHECK(neg_seq == 0, "%d results with negative mass", neg_seq);
    CHECK(memcmp(res_seq, res_par, sizeof *res_seq * (size_t)n) == 0,
          "OpenMP results differ from sequential");
    CHECK(err_par == err_seq && neg_par == neg_seq,
          "OpenMP reductions differ");
    CHECK(fabs(oil_par - oil_seq) < 1e-6,
          "OpenMP oil total differs from sequential");

    printf("OpenMP benchmark (%d threads)\n", omp_get_max_threads());
    printf("  sequential: %.4f s\n  parallel:   %.4f s\n  speed-up:   %.2fx "
           "(%s)\n", t_seq, t_par, t_par > 0 ? t_seq / t_par : 0.0,
           t_par < t_seq ? "parallel faster" : "parallel slower");

    edge_cases(&cfg);

    free(batches);
    free(water_u);
    free(res_seq);
    free(res_par);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
