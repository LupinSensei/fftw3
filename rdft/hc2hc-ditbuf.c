/*
 * Copyright (c) 2002 Matteo Frigo
 * Copyright (c) 2002 Steven G. Johnson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/* $Id: hc2hc-ditbuf.c,v 1.1 2002-07-22 23:43:39 stevenj Exp $ */

/* decimation in time Cooley-Tukey */
#include "rdft.h"
#include "hc2hc.h"

/*
   Copy A -> B, where A and B are n0 x n1 complex matrices
   such that the (i0, i1) element has index (i0 * s0 + i1 * s1).
   The imaginary strides are of opposite signs to the real strides.
*/
static void cpy(uint n0, uint n1,
                const R *rA, const R *iA, int sa0, int sa1,
                R *rB, R *iB, int sb0, int sb1)
{
     uint i0, i1;

     for (i0 = 0; i0 < n0; ++i0) {
          const R *pra, *pia;
          R *prb, *pib;
          pra = rA; rA += sa0;
          pia = iA; iA -= sa0;
          prb = rB; rB += sb0;
          pib = iB; iB -= sb0;

          for (i1 = 0; i1 < n1; ++i1) {
               R xr, xi;
               xr = *pra; pra += sa1;
               xi = *pia; pia -= sa1;
               *prb = xr; prb += sb1;
               *pib = xi; pib -= sb1;
          }
     }
}

static const R *doit(khc2hc k, R *rA, R *iA, const R *W, int ios, int dist,
                     uint r, uint batchsz, R *buf, stride bufstride)
{
     cpy(r, batchsz, rA, iA, ios, dist, buf, buf + 2*batchsz*r-1, 1, r);
     W = k(buf, buf + 2*batchsz*r-1, W, bufstride, 2*batchsz + 1, r);
     cpy(r, batchsz, buf, buf + 2*batchsz*r-1, 1, r, rA, iA, ios, dist);
     return W;
}

#define BATCHSZ 4 /* FIXME: parametrize? */

static void apply(plan *ego_, R *I, R *O)
{
     plan_hc2hc *ego = (plan_hc2hc *) ego_;

     /* two-dimensional r x vl sub-transform: */
     {
	  plan_rdft *cld = (plan_rdft *) ego->cld;
	  cld->apply((plan *) cld, I, O);
     }

     {
          plan_rdft *cld0 = (plan_rdft *) ego->cld0;
          plan_rdft *cldm = (plan_rdft *) ego->cldm;
          uint i, j, r = ego->r, m = ego->m, vl = ego->vl;
          int os = ego->os, ovs = ego->ovs, ios = ego->iios;
	  R *buf;

	  STACK_MALLOC(R *, buf, r * BATCHSZ * 2 * sizeof(R));

          for (i = 0; i < vl; ++i, O += ovs) {
	       R *rA, *iA;
	       const R *W;

	       cld0->apply((plan *) cld0, O, O);
	       
	       rA = O + os; iA = O + (r * m - 1) * os;
	       W = ego->W;
	       for (j = (m-1)/2; j >= BATCHSZ; j -= BATCHSZ) {
		    W = doit(ego->k, rA, iA, W, ios, os, r, BATCHSZ, buf,
			     ego->vs);
		    rA += os * (int)BATCHSZ;
		    iA -= os * (int)BATCHSZ;
	       }
	       /* do remaining j calls, if any */
               if (j > 0)
                    doit(ego->k, rA, iA, W, ios, os, r, j, buf, ego->vs);

	       cldm->apply((plan *) cldm, O + os*(m/2), O + os*(m/2));
	  }

	  STACK_FREE(buf);
     }
}

static int applicable(const solver_hc2hc *ego, const problem *p_)
{
     if (X(rdft_hc2hc_applicable)(ego, p_)) {
          const hc2hc_desc *e = ego->desc;
          const problem_rdft *p = (const problem_rdft *) p_;
          iodim *d = p->sz.dims;
	  uint r = e->radix, m = d[0].n / e->radix;
          return (1
                  /* check both batch size and remainder */
                  && (m < BATCHSZ ||
                      (e->genus->okp(e, 0, ((R *)0) + 2*BATCHSZ*r-1, 1, 0, 
				     2*BATCHSZ + 1, r)))
                  && (m < BATCHSZ ||
                      (e->genus->okp(e, 0, ((R *)0) 
				     + 2*(((m-1)/2) % BATCHSZ)*r-1, 1, 0, 
				     2*(((m-1)/2) % BATCHSZ) + 1, r)))
	       );
     }
     return 0;
}

static void finish(plan_hc2hc *ego)
{
     const hc2hc_desc *d = ego->slv->desc;
     ego->iios = ego->m * ego->os;
     ego->vs = X(mkstride)(ego->r, 1);
     ego->super.super.ops =
          X(ops_add3)(X(ops_add)(ego->cld->ops,
				 X(ops_mul)(ego->vl,
					    X(ops_add)(ego->cld0->ops,
						       ego->cldm->ops))),
		      /* 4 load/stores * N * VL */
                      X(ops_other)(4 * ego->r * ((ego->m - 1)/2) * ego->vl),
		      X(ops_mul)(ego->vl * ((ego->m - 1)/2) / d->genus->vl,
				 d->ops));
}

static int score(const solver *ego_, const problem *p_, int flags)
{
     const solver_hc2hc *ego = (const solver_hc2hc *) ego_;
     const problem_rdft *p;
     uint n;

     if (!applicable(ego, p_))
          return BAD;

     p = (const problem_rdft *) p_;

     /* emulate fftw2 behavior */
     if ((p->vecsz.rnk > 0) && NO_VRECURSE(flags))
	  return BAD;

     n = p->sz.dims[0].n;
     if (0
	 || n <= 512 /* favor non-buffered version */ 
	 || n / ego->desc->radix <= 4
	  )
	  return UGLY;

     return GOOD;
}


static plan *mkplan(const solver *ego, const problem *p, planner *plnr)
{
     static const hc2hcadt adt = {
	  X(rdft_mkcldrn_dit), finish, applicable, apply
     };
     return X(mkplan_rdft_hc2hc)((const solver_hc2hc *) ego, p, plnr, &adt);
}


solver *X(mksolver_rdft_hc2hc_ditbuf)(khc2hc codelet, const hc2hc_desc *desc)
{
     static const solver_adt sadt = { mkplan, score };
     static const char name[] = "rdft-ditbuf";

     return X(mksolver_rdft_hc2hc)(codelet, desc, name, &sadt);
}
