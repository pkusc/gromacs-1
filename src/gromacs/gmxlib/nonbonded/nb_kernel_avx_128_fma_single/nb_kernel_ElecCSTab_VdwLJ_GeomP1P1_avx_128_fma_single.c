/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2012,2013,2014,2015, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/*
 * Note: this file was generated by the GROMACS avx_128_fma_single kernel generator.
 */
#include "gmxpre.h"

#include "config.h"

#include <math.h>

#include "../nb_kernel.h"
#include "gromacs/math/vec.h"
#include "gromacs/legacyheaders/nrnb.h"

#include "gromacs/simd/math_x86_avx_128_fma_single.h"
#include "kernelutil_x86_avx_128_fma_single.h"

/*
 * Gromacs nonbonded kernel:   nb_kernel_ElecCSTab_VdwLJ_GeomP1P1_VF_avx_128_fma_single
 * Electrostatics interaction: CubicSplineTable
 * VdW interaction:            LennardJones
 * Geometry:                   Particle-Particle
 * Calculate force/pot:        PotentialAndForce
 */
void
nb_kernel_ElecCSTab_VdwLJ_GeomP1P1_VF_avx_128_fma_single
                    (t_nblist                    * gmx_restrict       nlist,
                     rvec                        * gmx_restrict          xx,
                     rvec                        * gmx_restrict          ff,
                     t_forcerec                  * gmx_restrict          fr,
                     t_mdatoms                   * gmx_restrict     mdatoms,
                     nb_kernel_data_t gmx_unused * gmx_restrict kernel_data,
                     t_nrnb                      * gmx_restrict        nrnb)
{
    /* Suffixes 0,1,2,3 refer to particle indices for waters in the inner or outer loop, or
     * just 0 for non-waters.
     * Suffixes A,B,C,D refer to j loop unrolling done with AVX_128, e.g. for the four different
     * jnr indices corresponding to data put in the four positions in the SIMD register.
     */
    int              i_shift_offset,i_coord_offset,outeriter,inneriter;
    int              j_index_start,j_index_end,jidx,nri,inr,ggid,iidx;
    int              jnrA,jnrB,jnrC,jnrD;
    int              jnrlistA,jnrlistB,jnrlistC,jnrlistD;
    int              j_coord_offsetA,j_coord_offsetB,j_coord_offsetC,j_coord_offsetD;
    int              *iinr,*jindex,*jjnr,*shiftidx,*gid;
    real             rcutoff_scalar;
    real             *shiftvec,*fshift,*x,*f;
    real             *fjptrA,*fjptrB,*fjptrC,*fjptrD;
    real             scratch[4*DIM];
    __m128           fscal,rcutoff,rcutoff2,jidxall;
    int              vdwioffset0;
    __m128           ix0,iy0,iz0,fix0,fiy0,fiz0,iq0,isai0;
    int              vdwjidx0A,vdwjidx0B,vdwjidx0C,vdwjidx0D;
    __m128           jx0,jy0,jz0,fjx0,fjy0,fjz0,jq0,isaj0;
    __m128           dx00,dy00,dz00,rsq00,rinv00,rinvsq00,r00,qq00,c6_00,c12_00;
    __m128           velec,felec,velecsum,facel,crf,krf,krf2;
    real             *charge;
    int              nvdwtype;
    __m128           rinvsix,rvdw,vvdw,vvdw6,vvdw12,fvdw,fvdw6,fvdw12,vvdwsum,sh_vdw_invrcut6;
    int              *vdwtype;
    real             *vdwparam;
    __m128           one_sixth   = _mm_set1_ps(1.0/6.0);
    __m128           one_twelfth = _mm_set1_ps(1.0/12.0);
    __m128i          vfitab;
    __m128i          ifour       = _mm_set1_epi32(4);
    __m128           rt,vfeps,twovfeps,vftabscale,Y,F,G,H,Fp,VV,FF;
    real             *vftab;
    __m128           dummy_mask,cutoff_mask;
    __m128           signbit = _mm_castsi128_ps( _mm_set1_epi32(0x80000000) );
    __m128           one     = _mm_set1_ps(1.0);
    __m128           two     = _mm_set1_ps(2.0);
    x                = xx[0];
    f                = ff[0];

    nri              = nlist->nri;
    iinr             = nlist->iinr;
    jindex           = nlist->jindex;
    jjnr             = nlist->jjnr;
    shiftidx         = nlist->shift;
    gid              = nlist->gid;
    shiftvec         = fr->shift_vec[0];
    fshift           = fr->fshift[0];
    facel            = _mm_set1_ps(fr->epsfac);
    charge           = mdatoms->chargeA;
    nvdwtype         = fr->ntype;
    vdwparam         = fr->nbfp;
    vdwtype          = mdatoms->typeA;

    vftab            = kernel_data->table_elec->data;
    vftabscale       = _mm_set1_ps(kernel_data->table_elec->scale);

    /* Avoid stupid compiler warnings */
    jnrA = jnrB = jnrC = jnrD = 0;
    j_coord_offsetA = 0;
    j_coord_offsetB = 0;
    j_coord_offsetC = 0;
    j_coord_offsetD = 0;

    outeriter        = 0;
    inneriter        = 0;

    for(iidx=0;iidx<4*DIM;iidx++)
    {
        scratch[iidx] = 0.0;
    }

    /* Start outer loop over neighborlists */
    for(iidx=0; iidx<nri; iidx++)
    {
        /* Load shift vector for this list */
        i_shift_offset   = DIM*shiftidx[iidx];

        /* Load limits for loop over neighbors */
        j_index_start    = jindex[iidx];
        j_index_end      = jindex[iidx+1];

        /* Get outer coordinate index */
        inr              = iinr[iidx];
        i_coord_offset   = DIM*inr;

        /* Load i particle coords and add shift vector */
        gmx_mm_load_shift_and_1rvec_broadcast_ps(shiftvec+i_shift_offset,x+i_coord_offset,&ix0,&iy0,&iz0);

        fix0             = _mm_setzero_ps();
        fiy0             = _mm_setzero_ps();
        fiz0             = _mm_setzero_ps();

        /* Load parameters for i particles */
        iq0              = _mm_mul_ps(facel,_mm_load1_ps(charge+inr+0));
        vdwioffset0      = 2*nvdwtype*vdwtype[inr+0];

        /* Reset potential sums */
        velecsum         = _mm_setzero_ps();
        vvdwsum          = _mm_setzero_ps();

        /* Start inner kernel loop */
        for(jidx=j_index_start; jidx<j_index_end && jjnr[jidx+3]>=0; jidx+=4)
        {

            /* Get j neighbor index, and coordinate index */
            jnrA             = jjnr[jidx];
            jnrB             = jjnr[jidx+1];
            jnrC             = jjnr[jidx+2];
            jnrD             = jjnr[jidx+3];
            j_coord_offsetA  = DIM*jnrA;
            j_coord_offsetB  = DIM*jnrB;
            j_coord_offsetC  = DIM*jnrC;
            j_coord_offsetD  = DIM*jnrD;

            /* load j atom coordinates */
            gmx_mm_load_1rvec_4ptr_swizzle_ps(x+j_coord_offsetA,x+j_coord_offsetB,
                                              x+j_coord_offsetC,x+j_coord_offsetD,
                                              &jx0,&jy0,&jz0);

            /* Calculate displacement vector */
            dx00             = _mm_sub_ps(ix0,jx0);
            dy00             = _mm_sub_ps(iy0,jy0);
            dz00             = _mm_sub_ps(iz0,jz0);

            /* Calculate squared distance and things based on it */
            rsq00            = gmx_mm_calc_rsq_ps(dx00,dy00,dz00);

            rinv00           = gmx_mm_invsqrt_ps(rsq00);

            rinvsq00         = _mm_mul_ps(rinv00,rinv00);

            /* Load parameters for j particles */
            jq0              = gmx_mm_load_4real_swizzle_ps(charge+jnrA+0,charge+jnrB+0,
                                                              charge+jnrC+0,charge+jnrD+0);
            vdwjidx0A        = 2*vdwtype[jnrA+0];
            vdwjidx0B        = 2*vdwtype[jnrB+0];
            vdwjidx0C        = 2*vdwtype[jnrC+0];
            vdwjidx0D        = 2*vdwtype[jnrD+0];

            /**************************
             * CALCULATE INTERACTIONS *
             **************************/

            r00              = _mm_mul_ps(rsq00,rinv00);

            /* Compute parameters for interactions between i and j atoms */
            qq00             = _mm_mul_ps(iq0,jq0);
            gmx_mm_load_4pair_swizzle_ps(vdwparam+vdwioffset0+vdwjidx0A,
                                         vdwparam+vdwioffset0+vdwjidx0B,
                                         vdwparam+vdwioffset0+vdwjidx0C,
                                         vdwparam+vdwioffset0+vdwjidx0D,
                                         &c6_00,&c12_00);

            /* Calculate table index by multiplying r with table scale and truncate to integer */
            rt               = _mm_mul_ps(r00,vftabscale);
            vfitab           = _mm_cvttps_epi32(rt);
#ifdef __XOP__
            vfeps            = _mm_frcz_ps(rt);
#else
            vfeps            = _mm_sub_ps(rt,_mm_round_ps(rt, _MM_FROUND_FLOOR));
#endif
            twovfeps         = _mm_add_ps(vfeps,vfeps);
            vfitab           = _mm_slli_epi32(vfitab,2);

            /* CUBIC SPLINE TABLE ELECTROSTATICS */
            Y                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,0) );
            F                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,1) );
            G                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,2) );
            H                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,3) );
            _MM_TRANSPOSE4_PS(Y,F,G,H);
            Fp               = _mm_macc_ps(vfeps,_mm_macc_ps(H,vfeps,G),F);
            VV               = _mm_macc_ps(vfeps,Fp,Y);
            velec            = _mm_mul_ps(qq00,VV);
            FF               = _mm_macc_ps(vfeps,_mm_macc_ps(twovfeps,H,G),Fp);
            felec            = _mm_xor_ps(signbit,_mm_mul_ps(_mm_mul_ps(qq00,FF),_mm_mul_ps(vftabscale,rinv00)));

            /* LENNARD-JONES DISPERSION/REPULSION */

            rinvsix          = _mm_mul_ps(_mm_mul_ps(rinvsq00,rinvsq00),rinvsq00);
            vvdw6            = _mm_mul_ps(c6_00,rinvsix);
            vvdw12           = _mm_mul_ps(c12_00,_mm_mul_ps(rinvsix,rinvsix));
            vvdw             = _mm_msub_ps(vvdw12,one_twelfth,_mm_mul_ps(vvdw6,one_sixth));
            fvdw             = _mm_mul_ps(_mm_sub_ps(vvdw12,vvdw6),rinvsq00);

            /* Update potential sum for this i atom from the interaction with this j atom. */
            velecsum         = _mm_add_ps(velecsum,velec);
            vvdwsum          = _mm_add_ps(vvdwsum,vvdw);

            fscal            = _mm_add_ps(felec,fvdw);

             /* Update vectorial force */
            fix0             = _mm_macc_ps(dx00,fscal,fix0);
            fiy0             = _mm_macc_ps(dy00,fscal,fiy0);
            fiz0             = _mm_macc_ps(dz00,fscal,fiz0);

            fjptrA             = f+j_coord_offsetA;
            fjptrB             = f+j_coord_offsetB;
            fjptrC             = f+j_coord_offsetC;
            fjptrD             = f+j_coord_offsetD;
            gmx_mm_decrement_1rvec_4ptr_swizzle_ps(fjptrA,fjptrB,fjptrC,fjptrD,
                                                   _mm_mul_ps(dx00,fscal),
                                                   _mm_mul_ps(dy00,fscal),
                                                   _mm_mul_ps(dz00,fscal));

            /* Inner loop uses 59 flops */
        }

        if(jidx<j_index_end)
        {

            /* Get j neighbor index, and coordinate index */
            jnrlistA         = jjnr[jidx];
            jnrlistB         = jjnr[jidx+1];
            jnrlistC         = jjnr[jidx+2];
            jnrlistD         = jjnr[jidx+3];
            /* Sign of each element will be negative for non-real atoms.
             * This mask will be 0xFFFFFFFF for dummy entries and 0x0 for real ones,
             * so use it as val = _mm_andnot_ps(mask,val) to clear dummy entries.
             */
            dummy_mask = gmx_mm_castsi128_ps(_mm_cmplt_epi32(_mm_loadu_si128((const __m128i *)(jjnr+jidx)),_mm_setzero_si128()));
            jnrA       = (jnrlistA>=0) ? jnrlistA : 0;
            jnrB       = (jnrlistB>=0) ? jnrlistB : 0;
            jnrC       = (jnrlistC>=0) ? jnrlistC : 0;
            jnrD       = (jnrlistD>=0) ? jnrlistD : 0;
            j_coord_offsetA  = DIM*jnrA;
            j_coord_offsetB  = DIM*jnrB;
            j_coord_offsetC  = DIM*jnrC;
            j_coord_offsetD  = DIM*jnrD;

            /* load j atom coordinates */
            gmx_mm_load_1rvec_4ptr_swizzle_ps(x+j_coord_offsetA,x+j_coord_offsetB,
                                              x+j_coord_offsetC,x+j_coord_offsetD,
                                              &jx0,&jy0,&jz0);

            /* Calculate displacement vector */
            dx00             = _mm_sub_ps(ix0,jx0);
            dy00             = _mm_sub_ps(iy0,jy0);
            dz00             = _mm_sub_ps(iz0,jz0);

            /* Calculate squared distance and things based on it */
            rsq00            = gmx_mm_calc_rsq_ps(dx00,dy00,dz00);

            rinv00           = gmx_mm_invsqrt_ps(rsq00);

            rinvsq00         = _mm_mul_ps(rinv00,rinv00);

            /* Load parameters for j particles */
            jq0              = gmx_mm_load_4real_swizzle_ps(charge+jnrA+0,charge+jnrB+0,
                                                              charge+jnrC+0,charge+jnrD+0);
            vdwjidx0A        = 2*vdwtype[jnrA+0];
            vdwjidx0B        = 2*vdwtype[jnrB+0];
            vdwjidx0C        = 2*vdwtype[jnrC+0];
            vdwjidx0D        = 2*vdwtype[jnrD+0];

            /**************************
             * CALCULATE INTERACTIONS *
             **************************/

            r00              = _mm_mul_ps(rsq00,rinv00);
            r00              = _mm_andnot_ps(dummy_mask,r00);

            /* Compute parameters for interactions between i and j atoms */
            qq00             = _mm_mul_ps(iq0,jq0);
            gmx_mm_load_4pair_swizzle_ps(vdwparam+vdwioffset0+vdwjidx0A,
                                         vdwparam+vdwioffset0+vdwjidx0B,
                                         vdwparam+vdwioffset0+vdwjidx0C,
                                         vdwparam+vdwioffset0+vdwjidx0D,
                                         &c6_00,&c12_00);

            /* Calculate table index by multiplying r with table scale and truncate to integer */
            rt               = _mm_mul_ps(r00,vftabscale);
            vfitab           = _mm_cvttps_epi32(rt);
#ifdef __XOP__
            vfeps            = _mm_frcz_ps(rt);
#else
            vfeps            = _mm_sub_ps(rt,_mm_round_ps(rt, _MM_FROUND_FLOOR));
#endif
            twovfeps         = _mm_add_ps(vfeps,vfeps);
            vfitab           = _mm_slli_epi32(vfitab,2);

            /* CUBIC SPLINE TABLE ELECTROSTATICS */
            Y                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,0) );
            F                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,1) );
            G                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,2) );
            H                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,3) );
            _MM_TRANSPOSE4_PS(Y,F,G,H);
            Fp               = _mm_macc_ps(vfeps,_mm_macc_ps(H,vfeps,G),F);
            VV               = _mm_macc_ps(vfeps,Fp,Y);
            velec            = _mm_mul_ps(qq00,VV);
            FF               = _mm_macc_ps(vfeps,_mm_macc_ps(twovfeps,H,G),Fp);
            felec            = _mm_xor_ps(signbit,_mm_mul_ps(_mm_mul_ps(qq00,FF),_mm_mul_ps(vftabscale,rinv00)));

            /* LENNARD-JONES DISPERSION/REPULSION */

            rinvsix          = _mm_mul_ps(_mm_mul_ps(rinvsq00,rinvsq00),rinvsq00);
            vvdw6            = _mm_mul_ps(c6_00,rinvsix);
            vvdw12           = _mm_mul_ps(c12_00,_mm_mul_ps(rinvsix,rinvsix));
            vvdw             = _mm_msub_ps(vvdw12,one_twelfth,_mm_mul_ps(vvdw6,one_sixth));
            fvdw             = _mm_mul_ps(_mm_sub_ps(vvdw12,vvdw6),rinvsq00);

            /* Update potential sum for this i atom from the interaction with this j atom. */
            velec            = _mm_andnot_ps(dummy_mask,velec);
            velecsum         = _mm_add_ps(velecsum,velec);
            vvdw             = _mm_andnot_ps(dummy_mask,vvdw);
            vvdwsum          = _mm_add_ps(vvdwsum,vvdw);

            fscal            = _mm_add_ps(felec,fvdw);

            fscal            = _mm_andnot_ps(dummy_mask,fscal);

             /* Update vectorial force */
            fix0             = _mm_macc_ps(dx00,fscal,fix0);
            fiy0             = _mm_macc_ps(dy00,fscal,fiy0);
            fiz0             = _mm_macc_ps(dz00,fscal,fiz0);

            fjptrA             = (jnrlistA>=0) ? f+j_coord_offsetA : scratch;
            fjptrB             = (jnrlistB>=0) ? f+j_coord_offsetB : scratch;
            fjptrC             = (jnrlistC>=0) ? f+j_coord_offsetC : scratch;
            fjptrD             = (jnrlistD>=0) ? f+j_coord_offsetD : scratch;
            gmx_mm_decrement_1rvec_4ptr_swizzle_ps(fjptrA,fjptrB,fjptrC,fjptrD,
                                                   _mm_mul_ps(dx00,fscal),
                                                   _mm_mul_ps(dy00,fscal),
                                                   _mm_mul_ps(dz00,fscal));

            /* Inner loop uses 60 flops */
        }

        /* End of innermost loop */

        gmx_mm_update_iforce_1atom_swizzle_ps(fix0,fiy0,fiz0,
                                              f+i_coord_offset,fshift+i_shift_offset);

        ggid                        = gid[iidx];
        /* Update potential energies */
        gmx_mm_update_1pot_ps(velecsum,kernel_data->energygrp_elec+ggid);
        gmx_mm_update_1pot_ps(vvdwsum,kernel_data->energygrp_vdw+ggid);

        /* Increment number of inner iterations */
        inneriter                  += j_index_end - j_index_start;

        /* Outer loop uses 9 flops */
    }

    /* Increment number of outer iterations */
    outeriter        += nri;

    /* Update outer/inner flops */

    inc_nrnb(nrnb,eNR_NBKERNEL_ELEC_VDW_VF,outeriter*9 + inneriter*60);
}
/*
 * Gromacs nonbonded kernel:   nb_kernel_ElecCSTab_VdwLJ_GeomP1P1_F_avx_128_fma_single
 * Electrostatics interaction: CubicSplineTable
 * VdW interaction:            LennardJones
 * Geometry:                   Particle-Particle
 * Calculate force/pot:        Force
 */
void
nb_kernel_ElecCSTab_VdwLJ_GeomP1P1_F_avx_128_fma_single
                    (t_nblist                    * gmx_restrict       nlist,
                     rvec                        * gmx_restrict          xx,
                     rvec                        * gmx_restrict          ff,
                     t_forcerec                  * gmx_restrict          fr,
                     t_mdatoms                   * gmx_restrict     mdatoms,
                     nb_kernel_data_t gmx_unused * gmx_restrict kernel_data,
                     t_nrnb                      * gmx_restrict        nrnb)
{
    /* Suffixes 0,1,2,3 refer to particle indices for waters in the inner or outer loop, or
     * just 0 for non-waters.
     * Suffixes A,B,C,D refer to j loop unrolling done with AVX_128, e.g. for the four different
     * jnr indices corresponding to data put in the four positions in the SIMD register.
     */
    int              i_shift_offset,i_coord_offset,outeriter,inneriter;
    int              j_index_start,j_index_end,jidx,nri,inr,ggid,iidx;
    int              jnrA,jnrB,jnrC,jnrD;
    int              jnrlistA,jnrlistB,jnrlistC,jnrlistD;
    int              j_coord_offsetA,j_coord_offsetB,j_coord_offsetC,j_coord_offsetD;
    int              *iinr,*jindex,*jjnr,*shiftidx,*gid;
    real             rcutoff_scalar;
    real             *shiftvec,*fshift,*x,*f;
    real             *fjptrA,*fjptrB,*fjptrC,*fjptrD;
    real             scratch[4*DIM];
    __m128           fscal,rcutoff,rcutoff2,jidxall;
    int              vdwioffset0;
    __m128           ix0,iy0,iz0,fix0,fiy0,fiz0,iq0,isai0;
    int              vdwjidx0A,vdwjidx0B,vdwjidx0C,vdwjidx0D;
    __m128           jx0,jy0,jz0,fjx0,fjy0,fjz0,jq0,isaj0;
    __m128           dx00,dy00,dz00,rsq00,rinv00,rinvsq00,r00,qq00,c6_00,c12_00;
    __m128           velec,felec,velecsum,facel,crf,krf,krf2;
    real             *charge;
    int              nvdwtype;
    __m128           rinvsix,rvdw,vvdw,vvdw6,vvdw12,fvdw,fvdw6,fvdw12,vvdwsum,sh_vdw_invrcut6;
    int              *vdwtype;
    real             *vdwparam;
    __m128           one_sixth   = _mm_set1_ps(1.0/6.0);
    __m128           one_twelfth = _mm_set1_ps(1.0/12.0);
    __m128i          vfitab;
    __m128i          ifour       = _mm_set1_epi32(4);
    __m128           rt,vfeps,twovfeps,vftabscale,Y,F,G,H,Fp,VV,FF;
    real             *vftab;
    __m128           dummy_mask,cutoff_mask;
    __m128           signbit = _mm_castsi128_ps( _mm_set1_epi32(0x80000000) );
    __m128           one     = _mm_set1_ps(1.0);
    __m128           two     = _mm_set1_ps(2.0);
    x                = xx[0];
    f                = ff[0];

    nri              = nlist->nri;
    iinr             = nlist->iinr;
    jindex           = nlist->jindex;
    jjnr             = nlist->jjnr;
    shiftidx         = nlist->shift;
    gid              = nlist->gid;
    shiftvec         = fr->shift_vec[0];
    fshift           = fr->fshift[0];
    facel            = _mm_set1_ps(fr->epsfac);
    charge           = mdatoms->chargeA;
    nvdwtype         = fr->ntype;
    vdwparam         = fr->nbfp;
    vdwtype          = mdatoms->typeA;

    vftab            = kernel_data->table_elec->data;
    vftabscale       = _mm_set1_ps(kernel_data->table_elec->scale);

    /* Avoid stupid compiler warnings */
    jnrA = jnrB = jnrC = jnrD = 0;
    j_coord_offsetA = 0;
    j_coord_offsetB = 0;
    j_coord_offsetC = 0;
    j_coord_offsetD = 0;

    outeriter        = 0;
    inneriter        = 0;

    for(iidx=0;iidx<4*DIM;iidx++)
    {
        scratch[iidx] = 0.0;
    }

    /* Start outer loop over neighborlists */
    for(iidx=0; iidx<nri; iidx++)
    {
        /* Load shift vector for this list */
        i_shift_offset   = DIM*shiftidx[iidx];

        /* Load limits for loop over neighbors */
        j_index_start    = jindex[iidx];
        j_index_end      = jindex[iidx+1];

        /* Get outer coordinate index */
        inr              = iinr[iidx];
        i_coord_offset   = DIM*inr;

        /* Load i particle coords and add shift vector */
        gmx_mm_load_shift_and_1rvec_broadcast_ps(shiftvec+i_shift_offset,x+i_coord_offset,&ix0,&iy0,&iz0);

        fix0             = _mm_setzero_ps();
        fiy0             = _mm_setzero_ps();
        fiz0             = _mm_setzero_ps();

        /* Load parameters for i particles */
        iq0              = _mm_mul_ps(facel,_mm_load1_ps(charge+inr+0));
        vdwioffset0      = 2*nvdwtype*vdwtype[inr+0];

        /* Start inner kernel loop */
        for(jidx=j_index_start; jidx<j_index_end && jjnr[jidx+3]>=0; jidx+=4)
        {

            /* Get j neighbor index, and coordinate index */
            jnrA             = jjnr[jidx];
            jnrB             = jjnr[jidx+1];
            jnrC             = jjnr[jidx+2];
            jnrD             = jjnr[jidx+3];
            j_coord_offsetA  = DIM*jnrA;
            j_coord_offsetB  = DIM*jnrB;
            j_coord_offsetC  = DIM*jnrC;
            j_coord_offsetD  = DIM*jnrD;

            /* load j atom coordinates */
            gmx_mm_load_1rvec_4ptr_swizzle_ps(x+j_coord_offsetA,x+j_coord_offsetB,
                                              x+j_coord_offsetC,x+j_coord_offsetD,
                                              &jx0,&jy0,&jz0);

            /* Calculate displacement vector */
            dx00             = _mm_sub_ps(ix0,jx0);
            dy00             = _mm_sub_ps(iy0,jy0);
            dz00             = _mm_sub_ps(iz0,jz0);

            /* Calculate squared distance and things based on it */
            rsq00            = gmx_mm_calc_rsq_ps(dx00,dy00,dz00);

            rinv00           = gmx_mm_invsqrt_ps(rsq00);

            rinvsq00         = _mm_mul_ps(rinv00,rinv00);

            /* Load parameters for j particles */
            jq0              = gmx_mm_load_4real_swizzle_ps(charge+jnrA+0,charge+jnrB+0,
                                                              charge+jnrC+0,charge+jnrD+0);
            vdwjidx0A        = 2*vdwtype[jnrA+0];
            vdwjidx0B        = 2*vdwtype[jnrB+0];
            vdwjidx0C        = 2*vdwtype[jnrC+0];
            vdwjidx0D        = 2*vdwtype[jnrD+0];

            /**************************
             * CALCULATE INTERACTIONS *
             **************************/

            r00              = _mm_mul_ps(rsq00,rinv00);

            /* Compute parameters for interactions between i and j atoms */
            qq00             = _mm_mul_ps(iq0,jq0);
            gmx_mm_load_4pair_swizzle_ps(vdwparam+vdwioffset0+vdwjidx0A,
                                         vdwparam+vdwioffset0+vdwjidx0B,
                                         vdwparam+vdwioffset0+vdwjidx0C,
                                         vdwparam+vdwioffset0+vdwjidx0D,
                                         &c6_00,&c12_00);

            /* Calculate table index by multiplying r with table scale and truncate to integer */
            rt               = _mm_mul_ps(r00,vftabscale);
            vfitab           = _mm_cvttps_epi32(rt);
#ifdef __XOP__
            vfeps            = _mm_frcz_ps(rt);
#else
            vfeps            = _mm_sub_ps(rt,_mm_round_ps(rt, _MM_FROUND_FLOOR));
#endif
            twovfeps         = _mm_add_ps(vfeps,vfeps);
            vfitab           = _mm_slli_epi32(vfitab,2);

            /* CUBIC SPLINE TABLE ELECTROSTATICS */
            Y                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,0) );
            F                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,1) );
            G                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,2) );
            H                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,3) );
            _MM_TRANSPOSE4_PS(Y,F,G,H);
            Fp               = _mm_macc_ps(vfeps,_mm_macc_ps(H,vfeps,G),F);
            FF               = _mm_macc_ps(vfeps,_mm_macc_ps(twovfeps,H,G),Fp);
            felec            = _mm_xor_ps(signbit,_mm_mul_ps(_mm_mul_ps(qq00,FF),_mm_mul_ps(vftabscale,rinv00)));

            /* LENNARD-JONES DISPERSION/REPULSION */

            rinvsix          = _mm_mul_ps(_mm_mul_ps(rinvsq00,rinvsq00),rinvsq00);
            fvdw             = _mm_mul_ps(_mm_msub_ps(c12_00,rinvsix,c6_00),_mm_mul_ps(rinvsix,rinvsq00));

            fscal            = _mm_add_ps(felec,fvdw);

             /* Update vectorial force */
            fix0             = _mm_macc_ps(dx00,fscal,fix0);
            fiy0             = _mm_macc_ps(dy00,fscal,fiy0);
            fiz0             = _mm_macc_ps(dz00,fscal,fiz0);

            fjptrA             = f+j_coord_offsetA;
            fjptrB             = f+j_coord_offsetB;
            fjptrC             = f+j_coord_offsetC;
            fjptrD             = f+j_coord_offsetD;
            gmx_mm_decrement_1rvec_4ptr_swizzle_ps(fjptrA,fjptrB,fjptrC,fjptrD,
                                                   _mm_mul_ps(dx00,fscal),
                                                   _mm_mul_ps(dy00,fscal),
                                                   _mm_mul_ps(dz00,fscal));

            /* Inner loop uses 50 flops */
        }

        if(jidx<j_index_end)
        {

            /* Get j neighbor index, and coordinate index */
            jnrlistA         = jjnr[jidx];
            jnrlistB         = jjnr[jidx+1];
            jnrlistC         = jjnr[jidx+2];
            jnrlistD         = jjnr[jidx+3];
            /* Sign of each element will be negative for non-real atoms.
             * This mask will be 0xFFFFFFFF for dummy entries and 0x0 for real ones,
             * so use it as val = _mm_andnot_ps(mask,val) to clear dummy entries.
             */
            dummy_mask = gmx_mm_castsi128_ps(_mm_cmplt_epi32(_mm_loadu_si128((const __m128i *)(jjnr+jidx)),_mm_setzero_si128()));
            jnrA       = (jnrlistA>=0) ? jnrlistA : 0;
            jnrB       = (jnrlistB>=0) ? jnrlistB : 0;
            jnrC       = (jnrlistC>=0) ? jnrlistC : 0;
            jnrD       = (jnrlistD>=0) ? jnrlistD : 0;
            j_coord_offsetA  = DIM*jnrA;
            j_coord_offsetB  = DIM*jnrB;
            j_coord_offsetC  = DIM*jnrC;
            j_coord_offsetD  = DIM*jnrD;

            /* load j atom coordinates */
            gmx_mm_load_1rvec_4ptr_swizzle_ps(x+j_coord_offsetA,x+j_coord_offsetB,
                                              x+j_coord_offsetC,x+j_coord_offsetD,
                                              &jx0,&jy0,&jz0);

            /* Calculate displacement vector */
            dx00             = _mm_sub_ps(ix0,jx0);
            dy00             = _mm_sub_ps(iy0,jy0);
            dz00             = _mm_sub_ps(iz0,jz0);

            /* Calculate squared distance and things based on it */
            rsq00            = gmx_mm_calc_rsq_ps(dx00,dy00,dz00);

            rinv00           = gmx_mm_invsqrt_ps(rsq00);

            rinvsq00         = _mm_mul_ps(rinv00,rinv00);

            /* Load parameters for j particles */
            jq0              = gmx_mm_load_4real_swizzle_ps(charge+jnrA+0,charge+jnrB+0,
                                                              charge+jnrC+0,charge+jnrD+0);
            vdwjidx0A        = 2*vdwtype[jnrA+0];
            vdwjidx0B        = 2*vdwtype[jnrB+0];
            vdwjidx0C        = 2*vdwtype[jnrC+0];
            vdwjidx0D        = 2*vdwtype[jnrD+0];

            /**************************
             * CALCULATE INTERACTIONS *
             **************************/

            r00              = _mm_mul_ps(rsq00,rinv00);
            r00              = _mm_andnot_ps(dummy_mask,r00);

            /* Compute parameters for interactions between i and j atoms */
            qq00             = _mm_mul_ps(iq0,jq0);
            gmx_mm_load_4pair_swizzle_ps(vdwparam+vdwioffset0+vdwjidx0A,
                                         vdwparam+vdwioffset0+vdwjidx0B,
                                         vdwparam+vdwioffset0+vdwjidx0C,
                                         vdwparam+vdwioffset0+vdwjidx0D,
                                         &c6_00,&c12_00);

            /* Calculate table index by multiplying r with table scale and truncate to integer */
            rt               = _mm_mul_ps(r00,vftabscale);
            vfitab           = _mm_cvttps_epi32(rt);
#ifdef __XOP__
            vfeps            = _mm_frcz_ps(rt);
#else
            vfeps            = _mm_sub_ps(rt,_mm_round_ps(rt, _MM_FROUND_FLOOR));
#endif
            twovfeps         = _mm_add_ps(vfeps,vfeps);
            vfitab           = _mm_slli_epi32(vfitab,2);

            /* CUBIC SPLINE TABLE ELECTROSTATICS */
            Y                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,0) );
            F                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,1) );
            G                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,2) );
            H                = _mm_load_ps( vftab + _mm_extract_epi32(vfitab,3) );
            _MM_TRANSPOSE4_PS(Y,F,G,H);
            Fp               = _mm_macc_ps(vfeps,_mm_macc_ps(H,vfeps,G),F);
            FF               = _mm_macc_ps(vfeps,_mm_macc_ps(twovfeps,H,G),Fp);
            felec            = _mm_xor_ps(signbit,_mm_mul_ps(_mm_mul_ps(qq00,FF),_mm_mul_ps(vftabscale,rinv00)));

            /* LENNARD-JONES DISPERSION/REPULSION */

            rinvsix          = _mm_mul_ps(_mm_mul_ps(rinvsq00,rinvsq00),rinvsq00);
            fvdw             = _mm_mul_ps(_mm_msub_ps(c12_00,rinvsix,c6_00),_mm_mul_ps(rinvsix,rinvsq00));

            fscal            = _mm_add_ps(felec,fvdw);

            fscal            = _mm_andnot_ps(dummy_mask,fscal);

             /* Update vectorial force */
            fix0             = _mm_macc_ps(dx00,fscal,fix0);
            fiy0             = _mm_macc_ps(dy00,fscal,fiy0);
            fiz0             = _mm_macc_ps(dz00,fscal,fiz0);

            fjptrA             = (jnrlistA>=0) ? f+j_coord_offsetA : scratch;
            fjptrB             = (jnrlistB>=0) ? f+j_coord_offsetB : scratch;
            fjptrC             = (jnrlistC>=0) ? f+j_coord_offsetC : scratch;
            fjptrD             = (jnrlistD>=0) ? f+j_coord_offsetD : scratch;
            gmx_mm_decrement_1rvec_4ptr_swizzle_ps(fjptrA,fjptrB,fjptrC,fjptrD,
                                                   _mm_mul_ps(dx00,fscal),
                                                   _mm_mul_ps(dy00,fscal),
                                                   _mm_mul_ps(dz00,fscal));

            /* Inner loop uses 51 flops */
        }

        /* End of innermost loop */

        gmx_mm_update_iforce_1atom_swizzle_ps(fix0,fiy0,fiz0,
                                              f+i_coord_offset,fshift+i_shift_offset);

        /* Increment number of inner iterations */
        inneriter                  += j_index_end - j_index_start;

        /* Outer loop uses 7 flops */
    }

    /* Increment number of outer iterations */
    outeriter        += nri;

    /* Update outer/inner flops */

    inc_nrnb(nrnb,eNR_NBKERNEL_ELEC_VDW_F,outeriter*7 + inneriter*51);
}
