/*
 * $Id$
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * Gromacs Runs On Most of All Computer Systems
 */

#ifndef _bondf_h
#define _bondf_h

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif


#ifdef CPLUSPLUS
extern "C" {
#endif

#include <stdio.h>
#include "typedefs.h"
#include "nrnb.h"
#include "pbc.h"

extern void calc_bonds(FILE *log,t_commrec *cr,t_commrec *mcr,t_idef *idef,
                       rvec x[],rvec f[],t_forcerec *fr,t_graph *g,
                       real epot[],t_nrnb *nrnb,matrix box,real lambda,
		       t_mdatoms *md,int ngrp,real egnb[],real egcoul[],
		       t_fcdata *fcd,
		       int step,bool bSepDVDL);
/* 
 * The function calc_bonds() caluclates all bonded force interactions.
 * The "bonds" are specified as follows:
 *   int nbonds
 *	    the total number of bonded interactions.
 *   t_iatom *forceatoms
 *     specifies which atoms are involved in a bond of a certain 
 *     type, see also struct t_idef.
 *   t_functype *functype
 *	    defines for every bonded force type what type of function to 
 *     use, see also struct t_idef.
 *   t_iparams *forceparams
 *	    defines the parameters for every bond type, see also struct 
 *     t_idef.
 *   real epot[NR_F]
 *     total potential energy split up over the function types.
 *   return value:
 *	    the total potential energy (sum over epot).
 */

extern real bond_angle(matrix box,
		       rvec xi,rvec xj,rvec xk,	                /* in  */
		       rvec r_ij,rvec r_kj,real *costh,
		       int *t1,int *t2);	/* out */
/* Calculate bond-angle. No PBC is taken into account (use mol-shift) */

extern real dih_angle(matrix box,
		      rvec xi,rvec xj,rvec xk,rvec xl,   /* in */
		      rvec r_ij,rvec r_kj,rvec r_kl,rvec m,rvec n, /* out */
		      real *cos_phi,real *sign,
		      int *t1,int *t2,int *t3);
/* Calculate dihedral-angle. No PBC is taken into account (use mol-shift) */

extern void do_dih_fup(int i,int j,int k,int l,real ddphi,
		       rvec r_ij,rvec r_kj,rvec r_kl,
		       rvec m,rvec n,rvec f[],t_forcerec *fr,t_graph *g,
		       rvec x[],int t1,int t2,int t3);
/* Do an update of the forces for dihedral potentials */

/*************************************************************************
 *
 *  Bonded force functions
 *
 *************************************************************************/
  extern t_ifunc bonds,g96bonds,morsebonds,cubicbonds;
  extern t_ifunc angles,g96angles,cross_bond_bond,cross_bond_angle,urey_bradley;
  extern t_ifunc pdihs,idihs,rbdihs;
  extern t_ifunc water_pol,posres,angres,angresz,do_14,unimplemented;

#ifdef CPLUSPLUS
}
#endif

#endif	/* _bondf_h */
