/*
  est_n0.c
  David Rowe Dec 2019

  Estimate the position of n0, the impulse that excites each frame in
  the source/filter model.  This defines the linear component of the
  phase spectra.  Uses a file of Codec 2 model parameters on stdin as
  input.
*/

#include <assert.h>
#include <complex.h>
#include <getopt.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include "defines.h"

int main(int argc, char *argv[]) {
    MODEL model;

    int opt;
    int extract = 0;
    while ((opt = getopt(argc, argv, "r")) != -1) {
        switch (opt) {
        case 'r': extract = 1; break;
        default:
            fprintf(stderr, "usage: %s [-r]\n", argv[0]);
	    fprintf(stderr, "-r remove linear phase term and ouput model records\n");
            exit(1);
        }
    }
	
    while(fread(&model, sizeof(MODEL), 1, stdin)) {
	float Wo = model.Wo; int L = model.L;
	float P = 2.0*M_PI/Wo;
	float best_error = 1E32; float best_n0=0.0;

	/* note weighting MSE by log10(Am) works much better than
           Am*AM, the latter tends to fit a linear phase model between
           the two highest amplitude harmonics */

	for(float n0=0; n0<=P; n0+=0.25) {
	    float error = 0.0;
	    for(int m=1; m<=L; m++) {
		complex diff = cexp(I*model.phi[m]) - cexp(I*n0*m*Wo);
		error += log10(model.A[m])*cabs(diff)*cabs(diff);
	    }
	    if (error < best_error) {
		best_error = error;
		best_n0 = n0;
	    }
	}
	if (extract) {
	    for(int m=1; m<=L; m++) {
		complex diff = cexp(I*model.phi[m])*cexp(-I*best_n0*m*Wo);		
		model.phi[m] = carg(diff);
	    }
	    assert(fwrite(&model, sizeof(MODEL), 1, stdout));
	}
	else
	    printf("%f\n", best_n0);
    }
    return 0;
}