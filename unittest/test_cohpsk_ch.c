/*---------------------------------------------------------------------------*\
                                                                             
  FILE........: test_cohpsk_ch.c
  AUTHOR......: David Rowe  
  DATE CREATED: April 2015
                                                                             
  Tests for the C version of the coherent PSK FDM modem with channel
  impairments generated by Octave.
                                                                             
\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2015 David Rowe

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "codec2_cohpsk.h"
#include "test_bits_coh.h"
#include "octave.h"
#include "comp_prim.h"
#include "noise_samples.h"

#define FRAMES   350
#define FOFF_HZ  10.5
#define ES_NO_DB  12.0

int main(int argc, char *argv[])
{
    struct COHPSK *coh;
    int            tx_bits[COHPSK_BITS_PER_FRAME];
    COMP           tx_fdm[COHPSK_SAMPLES_PER_FRAME];
    COMP           ch_fdm[COHPSK_SAMPLES_PER_FRAME];
    int            rx_bits[COHPSK_BITS_PER_FRAME];
                                            
    int            f, r, i;
    int           *ptest_bits_coh, *ptest_bits_coh_end, *ptest_bits_coh_rx;
    COMP           phase_ch;
    int            noise_r, noise_end;
    float          corr;
    int            state, next_state, nerrors, nbits, reliable_sync_bit;
    float          EsNo, variance;
    COMP           scaled_noise;
    float          EsNodB, foff_hz;

    EsNodB = ES_NO_DB;
    foff_hz =  FOFF_HZ;
    if (argc == 3) {
        EsNodB = atof(argv[1]);
        foff_hz = atof(argv[2]);
    }
    fprintf(stderr, "EsNodB: %4.2f foff: %4.2f Hz\n", EsNodB, foff_hz);

    coh = cohpsk_create();
    assert(coh != NULL);

    ptest_bits_coh = ptest_bits_coh_rx = (int*)test_bits_coh;
    ptest_bits_coh_end = (int*)test_bits_coh + sizeof(test_bits_coh)/sizeof(int);
    phase_ch.real = 1.0; phase_ch.imag = 0.0; 
    noise_r = 0; 
    noise_end = sizeof(noise)/sizeof(COMP);
    
    /*  each carrier has power = 2, total power 2Nc, total symbol rate
        NcRs, noise BW B=Fs Es/No = (C/Rs)/(N/B), N = var =
        2NcFs/NcRs(Es/No) = 2Fs/Rs(Es/No) */

    EsNo = pow(10.0, EsNodB/10.0);
    variance = 2.0*COHPSK_FS/(COHPSK_RS*EsNo);

    /* Main Loop ---------------------------------------------------------------------*/

    for(f=0; f<FRAMES; f++) {
        
	/* --------------------------------------------------------*\
	                          Mod
	\*---------------------------------------------------------*/

        memcpy(tx_bits, ptest_bits_coh, sizeof(int)*COHPSK_BITS_PER_FRAME);
        ptest_bits_coh += COHPSK_BITS_PER_FRAME;
        if (ptest_bits_coh >= ptest_bits_coh_end) {
            ptest_bits_coh = (int*)test_bits_coh;
        }

	cohpsk_mod(coh, tx_fdm, tx_bits);

	/* --------------------------------------------------------*\
	                          Channel
	\*---------------------------------------------------------*/

        fdmdv_freq_shift(ch_fdm, tx_fdm, foff_hz, &phase_ch, COHPSK_SAMPLES_PER_FRAME);

        for(r=0; r<COHPSK_SAMPLES_PER_FRAME; r++) {
            scaled_noise = fcmult(sqrt(variance), noise[noise_r]);
            ch_fdm[r] = cadd(ch_fdm[r], scaled_noise);
            noise_r++;
            if (noise_r > noise_end)
                noise_r = 0;
        }

	/* --------------------------------------------------------*\
	                          Demod
	\*---------------------------------------------------------*/

 	cohpsk_demod(coh, rx_bits, &reliable_sync_bit, ch_fdm);

        corr = 0.0;
        for(i=0; i<COHPSK_BITS_PER_FRAME; i++) {
            corr += (1.0 - 2.0*(rx_bits[i] & 0x1)) * (1.0 - 2.0*ptest_bits_coh_rx[i]);
        }

        /* state logic to sync up to test data */

        next_state = state;

        if (state == 0) {
            if (reliable_sync_bit && (corr == COHPSK_BITS_PER_FRAME)) {
                next_state = 1;
                ptest_bits_coh_rx += COHPSK_BITS_PER_FRAME;
                nerrors = COHPSK_BITS_PER_FRAME - corr;
                nbits = COHPSK_BITS_PER_FRAME;
                fprintf(stderr, "  test data sync\n");            

            }
        }

        if (state == 1) {
            nerrors += COHPSK_BITS_PER_FRAME - corr;
            nbits   += COHPSK_BITS_PER_FRAME;
            ptest_bits_coh_rx += COHPSK_BITS_PER_FRAME;
            if (ptest_bits_coh_rx >= ptest_bits_coh_end) {
                ptest_bits_coh_rx = (int*)test_bits_coh;
            }
        }

        state = next_state;
    }
    
    printf("%4.3f %d %d\n", (float)nerrors/nbits, nbits, nerrors);

    cohpsk_destroy(coh);

    return 0;
}
