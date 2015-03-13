 /*---------------------------------------------------------------------------*\

  FILE........: iir_duc.c
  AUTHOR......: Brady O'Brien
  DATE CREATED: 6 Mar 2015

  Interapolator/Filter for IF upconversion

  Unit testing:
  
    ~/codec2-dev/stm32$ gcc -D__UNITTEST__ -Iinc src/iir_duc.c -o iir_duc -lm -Wall
    ~/codec2-dev/stm32$ ./iir_duc

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2015 Brady O'Brien

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2.1, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include "stm32f4_dacduc.h"
#include "iir_duc.h"

#define BETA1                    0.99002			// B1MUL/(2**B1SHFT)
#define B1MUL			 32441	
#define B1SMUL			 -38328
#define B1SHFT			 15				// 10 bits gives us plenty of headroom between 31 bits of int and 14 bits of ADC
#define B2MUL			 24593				// This actually matches BETA2 exactly with the supplied BETA1
#define B2SHFT			 15				// 10 is also the lowest we can go without beta1=1
#define BETA2                    (1.0 - (1.0-BETA1)*DUC_M)	// B2MUL/(2**B2SHFT)
#define IN_SCALE                 2.0                            //Input scaling factor. Should be as large as the amplitude of the incoming samples
#define DAC_SCALE                4096                           //Maximum output to DAC
#define DAC_SCALE_2		 2040


//IIR and FIR filter states. Global for go fast.
float f_1,f_2,f;
int   n_1,n_2,n;

/*
   Upconvert and bandpass filter a chunk of spectrum from Fs/M to Fs. We're going for 700khz here.
   modin needs to be DUC_N long and dac_out needs to be DUC_N*DUC_M long. This 
*/

void iir_upconv(float modin[], unsigned short dac_out[]){
    int i,j,k;
    int m;
    k=0;
    //Iterate through input samples and apply pre-eq FIR, interpolate, and apply BPF IIR
    for(i=0;i<DUC_N;i++){
        f = modin[i]+f_2*BETA2;
        f_2 = f_1;
        f_1 = modin[i];                                             //Scale fir output and convert to fixed.
        m = (int)((f/(IN_SCALE))*DAC_SCALE_2);                      //Scale fir output and convert to fixed
        n = m + ((B1SMUL*n_1)>>B1SHFT) - ((B1MUL*n_2)>>B1SHFT);   //Apply one cycle of IIR. This feeds the fir-ed sample into the output filter
        n_2 = n_1;
        n_1 = n;
        dac_out[k]=(unsigned short)(n+DAC_SCALE_2);
        k++;
        //now do the rest of the filtering. Because we're zero-stuffing we can neglect the sample from the fir filter.
        for(j=1;j<DUC_M;j++,k++){
            n = ((B1SMUL*n_1)>>B1SHFT) - ((B1MUL*n_2)>>B1SHFT);
            n_2 = n_1;
            n_1 = n;
            dac_out[k]=(unsigned short)((n)+DAC_SCALE_2);
        }
    }
}

#define F8C80R_LEN 21                                       //Number of taps in the 8C80R filters
#define F8C80R_MUL 1024
static int int1r,int2r,int3r,int4r,cmb1r,cmb2r,cmb3r,cmb4r; //States for re combs and integrators
static int int1i,int2i,int3i,int4i,cmb1i,cmb2i,cmb3i,cmb4i; //States for im combs and integrators
static float fir_8c80r_cic[];                               //FIR Coeffs
static float fir_8c80r_re[F8C80R_LEN*2];                    //FIR delay line for re
static float fir_8c80r_im[F8C80R_LEN*2];                    //FIR delay line for im
static float f8c80r_temp_re[DUC_N/5];
static float f8c80r_temp_im[DUC_N/5];
static int ptr_8c80r;                                       //circular buffer ptr fir_8c80r_re
static int w8c80r = 0;                                      //Omega for upconversion
static int cosf4[] = {1,0,-1,0};                            //Cosine table for fs/4. precomputed by meat computer
static int sinf4[] = {0,1,0,-1};                            //Sine table for fs/4.
/*
   Interpolate and shift from 8k complex to 80k real, centered on Fs/4.
    comp_8 - Input samples - 8Kc complex - must be (DUC_N/10)*count long
    upout - Output samples - must be DUC_N*count long
    count - how many chunks of samples must be processed
*/

void upconv_8c_80r(COMP comp_8[],float real_80[],int count){
    int i,j,k;
    float nr,ni,ret;
    int cmbr,cmbi,cmbrr,cmbii,rein,imin;
    for(i=0;i<count;i++){                //Iterate through chunks of samples
        for(j=0;j<DUC_N/10;j++){          //zero stuff
            f8c80r_temp_re[ j*2   ] = comp_8[j + (i * (DUC_N/10)) ].real;
            f8c80r_temp_re[(j*2)+1] = 0; //Stuff a zero in here
            f8c80r_temp_im[ j*2   ] = comp_8[j + (i * (DUC_N/10)) ].imag;
            f8c80r_temp_im[(j*2)+1] = 0; //Stuff a zero in here
        }
        for(j=0;j<DUC_N/5;j++){          //run samples through a fir
            fir_8c80r_re[ptr_8c80r]=f8c80r_temp_re[j];
            fir_8c80r_im[ptr_8c80r]=f8c80r_temp_im[j];
            fir_8c80r_re[ptr_8c80r+F8C80R_LEN]=f8c80r_temp_re[j];
            fir_8c80r_im[ptr_8c80r+F8C80R_LEN]=f8c80r_temp_im[j];
            nr=0; ni=0;
            for(k=0;k<F8C80R_LEN;k++){
                nr=nr+fir_8c80r_cic[k]*fir_8c80r_re[(ptr_8c80r+F8C80R_LEN)-k];
                ni=ni+fir_8c80r_cic[k]*fir_8c80r_im[(ptr_8c80r+F8C80R_LEN)-k];
            }
            f8c80r_temp_re[j]=nr; //Can be optimized out. Will do later
            f8c80r_temp_im[j]=ni; //Ditto
            ptr_8c80r++;
            if(ptr_8c80r>F8C80R_LEN)
                ptr_8c80r=0;
        }
        for(j=0;j<DUC_N/5;j++){   //Comb and Integrate
            rein = (int)(f8c80r_temp_re[j]*F8C80R_MUL); //Can be optimized out. Will do later
            imin = (int)(f8c80r_temp_im[j]*F8C80R_MUL); //Ditto
           
            cmbr =  rein - cmb1r; cmb1r = rein;   //Comb 1 real
            cmbrr = cmbr - cmb2r; cmb2r = cmbr;   //Comb 2 real
            cmbr = cmbrr - cmb3r; cmb3r = cmbrr;  //Comb 3 real
            cmbrr = cmbr - cmb4r; cmb4r = cmbr;   //Comb 4 real

            cmbi =  imin - cmb1i; cmb1i = imin;   //Comb 1 im
            cmbii = cmbi - cmb2i; cmb2i = cmbi;   //Comb 2 im
            cmbi = cmbii - cmb3i; cmb3i = cmbii;  //Comb 3 im
            cmbii = cmbi - cmb4i; cmb4i = cmbi;   //Comb 4 im
            //Do one cycle of integration
            int1r = cmbrr + int1r;                //Integrator stage 1 re
            int2r = int1r + int2r;                //Integrator stage 2 re
            int3r = int2r + int3r;                //Integrator stage 3 re
            int4r = int3r + int4r;                //Integrator stage 4 re

            int1i = cmbii + int1i;                //Integrator stage 1 im
            int2i = int1i + int2i;                //Integrator stage 2 im
            int3i = int2i + int3i;                //Integrator stage 3 im
            int4i = int3i + int4i;                //Integrator stage 4 im
            //Convert this complex into real and cancel out the gain from CIC
	    ret = (float) (((-cosf4[w8c80r]*int4r)+(sinf4[w8c80r]*int4i))>>0);
            real_80[(i*DUC_N)+(j*5)] = ret/F8C80R_MUL;
            w8c80r = (w8c80r+1)&0x3;
            for(k=1;k<5;k++){                     //Next 4 stages of integration
                //int1r = 0 + int1r;            //Integrator stage 1 re
                int2r = int1r + int2r;            //Integrator stage 2 re
                int3r = int2r + int3r;            //Integrator stage 3 re
                int4r = int3r + int4r;            //Integrator stage 4 re
                //int1i = 0 + int1i;            //Integrator stage 1 im
                int2i = int1i + int2i;            //Integrator stage 2 im
                int3i = int2i + int3i;            //Integrator stage 3 im
                int4i = int3i + int4i;            //Integrator stage 4 im
	        ret = (float)(((-cosf4[w8c80r]*int4r)+(sinf4[w8c80r]*int4i))>>0);
               // printf("%f,",ret);
                real_80[(i*DUC_N)+(j*5)+k] = ret/F8C80R_MUL;
		w8c80r = (w8c80r+1)&0x3;
            }
        }
    } 
}

#ifdef __UNITTEST__

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#define FS     80000
#define AMP_MAX 1

#define NOUT_BUFS    1000
#define NIN          (NOUT_BUFS*DUC_N)
#define NOUT         (NIN*DUC_M)

void synth_line(float us[], float f, float amp, int n) {
    float w, sam;
    int   i;

    w = 2*M_PI*f/(float)FS;

    for(i=0; i<n; i++) {
        sam = amp*AMP_MAX*cos(w*i);
        us[i] += sam;
    }
}

COMP       in[NIN/10];
float 	   s[NIN];
float      fout[NIN];
unsigned short todac[NOUT];

int main(void) {
    float          f1,f2,f3;
    FILE          *f;
    int            i;

    f1 = 20E3;	          /* center of passband */
    f2 = f1;	  /* wanted */
    f3 = f1 + 7E3;        /* wanted */

    for(i=0;i<NIN/10;i++){
        in[i].real=cos((float)i*(M_PI*.25));
        printf("%f\n",in[i].real);
        //in[i].real=0.70710678118;
        in[i].imag=0;
    }
    //in[0].real=1;
    /*for(i=0; i<NIN; i++)
        s[i] = 0.01;*/
    /*synth_line(s, f2, 0.5, NIN);
    synth_line(s, f3, 0.5, NIN);*/
    printf("\n");
    upconv_8c_80r(in,s,NOUT_BUFS);
    printf("\n");
    //for(i=0;i<NOUT_BUFS;i++)
    //    iir_upconv(&s[i*(DUC_N)],&todac[i*(DUC_N*DUC_M)]);
    
    f = fopen("iir_duc_s.txt", "wt");  assert(f != NULL);
    for(i=0; i<NIN; i++)
        fprintf(f, "%f\n", s[i]);
    fprintf(f, "\n");
    fclose(f);

    f = fopen("iir_duc_f.txt", "wt");  assert(f != NULL);
    for(i=0; i<NIN; i++)
        fprintf(f, "%f\n", fout[i]);
    fprintf(f, "\n");
    fclose(f);

    f = fopen("iir_duc.txt", "wt");  assert(f != NULL);
    for(i=0; i<NOUT; i++)
        fprintf(f, "%d\n", todac[i]);
    fprintf(f, "\n");
    fclose(f);
    return 0;
}

#endif

// constants for a fir filter used to convert 8kc complex to 80kc real
static float fir_8c80r_cic[] = {
-0.00000215,
-0.00008715,
0.00073915,
-0.00674415,
0.05618415,
0.01629015,
-0.19074815,
-0.04231615,
0.53620515,
0.09933915,
-1.32978715,
-0.38797815,
3.97887715,
6.70888315,
3.97887715,
-0.38797815,
-1.32978715,
0.09933915,
0.53620515,
-0.04231615,
-0.19074815,
};




