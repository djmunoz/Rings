#include"rings.h"

#include<gsl/gsl_integration.h>
#include<gsl/gsl_odeiv.h>
#include<gsl/gsl_errno.h>

#include<string.h>

size_t
body_size_to_vector_size(const size_t nbodies) { return nbodies*BODY_VECTOR_SIZE + CENTRAL_BODY_VECTOR_SIZE; }

void
bodies_to_vector(const central_body *bc, const body bs[], const size_t nbodies, double y[]) {
  size_t i;

  central_body_to_vector(bc, y);

  for(i = 0; i < nbodies; i++) {
    body_to_vector(bs+i, y+i*BODY_VECTOR_SIZE+CENTRAL_BODY_VECTOR_SIZE);
  }
}

void
vector_to_bodies(const double y[], const size_t nbodies, central_body *bc, body bs[]) {
  size_t i;

  vector_to_central_body(y, bc);

  for (i = 0; i < nbodies; i++) {
    vector_to_body(y+i*BODY_VECTOR_SIZE + CENTRAL_BODY_VECTOR_SIZE, bs+i);
  }
}

typedef struct {
  size_t nbodies;
  double eps;
  double epsquad;
} fparams;

static int
f(double t, const double y[], double dydt[], void *vparams) {
  fparams *p = (fparams *)vparams;
  size_t i;
  int status = GSL_SUCCESS;
  int overall_status = GSL_SUCCESS;

  memset(dydt, 0, body_size_to_vector_size(p->nbodies)*sizeof(double));
  
#pragma omp parallel for
  for (i = 0; i < p->nbodies; i++) {
    size_t j;
    body bi;
    central_body bc;
    double rhs[BODY_VECTOR_SIZE];
    double dsunomega[3];

    vector_to_body(y+i*BODY_VECTOR_SIZE+CENTRAL_BODY_VECTOR_SIZE, &bi);
    vector_to_central_body(y, &bc);

    for (j = 0; j < p->nbodies; j++) {
      if (j != i) {
        body bj;
        double rhs[BODY_VECTOR_SIZE];
        size_t k;
        int status;

        vector_to_body(y+j*BODY_VECTOR_SIZE + CENTRAL_BODY_VECTOR_SIZE, &bj);
        
        status = average_rhs(p->eps, &bi, &bj, p->epsquad, rhs);

        if (status != GSL_SUCCESS) {
          overall_status = GSL_EBADFUNC;
        }

        for (k = 0; k < BODY_VECTOR_SIZE; k++) {
          dydt[CENTRAL_BODY_VECTOR_SIZE + i*BODY_VECTOR_SIZE+k] += rhs[k];
        }
      }
    }

    /* Tidal Contributions */
    memset(rhs, 0, BODY_VECTOR_SIZE*sizeof(double));
    memset(dsunomega, 0, 3*sizeof(double));

    tidal_rhs(&bi, &bc, rhs, dsunomega);

    for (j = 0; j < BODY_VECTOR_SIZE; j++) {
      if (isnan(rhs[j])) overall_status = GSL_EBADFUNC;
    }

    for (j = 0; j < 3; j++) {
      if (isnan(dsunomega[j])) overall_status = GSL_EBADFUNC;
    }

    for (j = 0; j < 3; j++) {
      dydt[CENTRAL_BODY_SPIN_INDEX + j] += dsunomega[j];
    }
    for (j = 0; j < BODY_VECTOR_SIZE; j++) {
      dydt[CENTRAL_BODY_VECTOR_SIZE + i*BODY_VECTOR_SIZE + j] += rhs[j];
    }
  }

  return overall_status;
}

int
evolve_system(gsl_odeiv_evolve *e, gsl_odeiv_control *con, gsl_odeiv_step *step, 
              double *t, const double t1, double *h, central_body *bc, body bs[], double y[], 
              size_t nbodies, const double epsquad, const double eps) {
  fparams p;
  gsl_odeiv_system sys;
  int status = 0;
  int i;

  p.eps = eps;
  p.epsquad = epsquad;
  p.nbodies = nbodies;

  sys.function = (int (*)(double, const double [], double [], void *))f;
  sys.jacobian = 0;
  sys.dimension = body_size_to_vector_size(nbodies);
  sys.params = &p;

  bodies_to_vector(bc, bs, nbodies, y);

  status = gsl_odeiv_evolve_apply(e, con, step, &sys, t, t1, h, y);

  /* Bail early without changing bodies. */
  if (status != GSL_SUCCESS) return status;

  vector_to_bodies(y, nbodies, bc, bs);

  return status;
}

/* A new GSL Control object */
typedef struct {
  double eps;
} sco_data;

static const char *sco_name = "Secular Control Object";

static void *
sco_alloc() {
  sco_data *data = malloc(sizeof(sco_data));

  return data;
}

static int
sco_init(void *vstate, double eps_abs, double eps_rel, double a_y, double a_dydt) {
  sco_data *state = (sco_data *)vstate;

  state->eps = eps_abs;

  return GSL_SUCCESS;
}

static int
sco_hadjust(void *vstate, size_t dim, unsigned int ord, const double y[], const double yerr[], const double yp[], double *h) {
  sco_data *state = (sco_data *)vstate;
  double eps = state->eps;
  double max_error_factor = -1.0/0.0;
  double error_factor;
  size_t i;
  double hold = *h;
  double hnew;
  const double S = 0.9;
  central_body bc;
  double Ltot[3], Ltotmag;

  /* Find total angular momentum: */
  memset(Ltot, 0, 3*sizeof(double));
  vector_to_central_body(y, &bc);
  for (i = 0; i < 3; i++) {
    Ltot[i] += bc.I*bc.spin[i];
  }

  for (i = CENTRAL_BODY_VECTOR_SIZE; i < dim; i += BODY_VECTOR_SIZE) {
    body b;
    double n;
    int j;

    vector_to_body(&(y[i]), &b);
    n = mean_motion(&b);
    for (j = 0; j < 3; j++) {
      Ltot[j] += b.m*n*b.a*b.a*b.L[j];
      Ltot[j] += b.I*b.spin[j];
    }
  }
  
  Ltotmag = norm(Ltot);

  /* Match secular constraints. */
  for (i = CENTRAL_BODY_VECTOR_SIZE; i < dim; i += BODY_VECTOR_SIZE) {
    body b, berr;

    vector_to_body(&(y[i]), &b);
    vector_to_body(&(yerr[i]), &berr);

    /* d(L^2 + A^2) = 2*(L*dL + A*dA) */
    error_factor = 2.0 * (dot(b.L, berr.L) + dot(b.A, berr.A)) / eps;
    max_error_factor = (max_error_factor > error_factor ? max_error_factor : error_factor);
    
    /* d(L*A) = L*dA + dL*A */
    error_factor = (dot(b.L, berr.A) + dot(berr.L, b.A)) / eps;
    max_error_factor = (max_error_factor > error_factor ? max_error_factor : error_factor);
  }

  /* Errors in central body. */
  /* Relative in viscous timescale. Note comparsion works even if tV
     == 0, and we get NaNs. */
  error_factor = fabs(yerr[CENTRAL_BODY_TV_INDEX]/y[CENTRAL_BODY_TV_INDEX]) / eps;
  max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

  /* Absolute in k. */
  error_factor = fabs(yerr[CENTRAL_BODY_K_INDEX]) / eps;
  max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

  /* Relative in I */
  error_factor = fabs(yerr[CENTRAL_BODY_I_INDEX]/y[CENTRAL_BODY_I_INDEX]) / eps;
  max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

  /* Relative in R. */
  error_factor = fabs(yerr[CENTRAL_BODY_R_INDEX]/y[CENTRAL_BODY_R_INDEX]) / eps;
  max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

  /* Error in spin relative to total angular momentum. */
  error_factor = fabs(y[CENTRAL_BODY_I_INDEX]*norm(&(yerr[CENTRAL_BODY_SPIN_INDEX]))/Ltotmag) / eps;
  max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

  for (i = CENTRAL_BODY_VECTOR_SIZE; i < dim; i += BODY_VECTOR_SIZE) {
    body b;
    int j;

    vector_to_body(&(y[i]), &b);

    /* Relative in body m. */
    error_factor = fabs(yerr[i+BODY_M_INDEX]/b.m) / eps;
    max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

    /* Relative in body a. */
    error_factor = fabs(yerr[i+BODY_a_INDEX]/b.a) / eps;
    max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

    /* Relative in body tV. */
    error_factor = fabs(yerr[i+BODY_TV_INDEX]/b.tV) / eps;
    max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

    /* Absolute in k. */
    error_factor = fabs(yerr[i+BODY_K_INDEX]) / eps;
    max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

    /* Relative in I. */
    error_factor = fabs(yerr[i+BODY_I_INDEX]/b.I) / eps;
    max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

    /* Relative in R. */
    error_factor = fabs(yerr[i+BODY_R_INDEX]/b.R) / eps;
    max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

    /* Absolute in L = sqrt(1-e^2). */
    error_factor = norm(&(yerr[i+BODY_L_INDEX])) / eps;
    max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

    /* Absolute in A = e. */
    error_factor = norm(&(yerr[i+BODY_A_INDEX])) / eps;
    max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);

    /* Relative in spin to total L. */
    error_factor = fabs(b.I*norm(&(yerr[i+BODY_SPIN_INDEX]))/Ltotmag) / eps;
    max_error_factor = (error_factor > max_error_factor ? error_factor : max_error_factor);
  }

  if (max_error_factor > 1.1) {
    /* Should be decreasing the stepsize. */
    hnew = hold * S / pow(max_error_factor, 1.0/ord);
    
    if (hnew < 0.2*hold) hnew = 0.2*hold;

    *h = hnew;
    return GSL_ODEIV_HADJ_DEC;
  }  else if (max_error_factor < 0.5) {
    /* Should be increasing the stepsize. */
    hnew = hold * S / pow(max_error_factor, 1.0/(ord+1));

    if (hnew > 5*hold) hnew = 5*hold;

    *h = hnew;
    return GSL_ODEIV_HADJ_INC;
  } else {
    /* Do not change the stepsize */
    return GSL_ODEIV_HADJ_NIL;
  }
}

void 
sco_free(void *vstate) {
  free(vstate);
}

static gsl_odeiv_control_type sco_type;

gsl_odeiv_control *
gsl_odeiv_control_secular_new(double eps) {
  gsl_odeiv_control *con;

  /* Make sure that sco_type is set up. */
  sco_type.name = sco_name;
  sco_type.alloc = &sco_alloc;
  sco_type.init = &sco_init;
  sco_type.hadjust = &sco_hadjust;
  sco_type.free = &sco_free;

  con = gsl_odeiv_control_alloc(&sco_type);
  if (con == NULL) return con;

  gsl_odeiv_control_init(con, eps, 0.0, 0.0, 0.0); /* Remaining arguments are ignored. */

  return con;
}
