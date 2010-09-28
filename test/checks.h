#ifndef __CHECKS_H__
#define __CHECKS_H__

#include"../src/rings.h"

#include<gsl/gsl_rng.h>
#include<assert.h> /* Not strictly needed for this header, but useful
                      for files that include it. */

int
check_close(const double epsabs, const double epsrel, const double x, const double y);

int
check_vector_close(const double epsabs, const double epsrel, const double x[3], const double y[3]);

/* Random utilities. */
void
seed_random(gsl_rng *rng);

double
random_between(gsl_rng *rng, const double a, const double b);

void
init_random_body(gsl_rng *rng, body *b, const double m, const double a);

#endif /* __CHECKS_H__ */
