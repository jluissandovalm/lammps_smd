/* ----------------------------------------------------------------------
 *
 *                    *** Smooth Mach Dynamics ***
 *
 * This file is part of the USER-SMD package for LAMMPS.
 * Copyright (2014) Georg C. Ganzenmueller, georg.ganzenmueller@emi.fhg.de
 * Fraunhofer Ernst-Mach Institute for High-Speed Dynamics, EMI,
 * Eckerstrasse 4, D-79104 Freiburg i.Br, Germany.
 *
 * ----------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
 LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
 http://lammps.sandia.gov, Sandia National Laboratories
 Steve Plimpton, sjplimp@sandia.gov

 Copyright (2003) Sandia Corporation.  Under the terms of Contract
 DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
 certain rights in this software.  This software is distributed under
 the GNU General Public License.

 See the README file in the top-level LAMMPS directory.
 ------------------------------------------------------------------------- */

#include "stdio.h"
#include "string.h"
#include "fix_smd_integrate_mpm.h"
#include "math.h"
#include "stdlib.h"
#include "string.h"
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "update.h"
#include "integrate.h"
#include "respa.h"
#include "memory.h"
#include "error.h"
#include "pair.h"
#include "domain.h"
#include <Eigen/Eigen>

using namespace Eigen;
using namespace LAMMPS_NS;
using namespace FixConst;

/* ---------------------------------------------------------------------- */

FixSMDIntegrateMpm::FixSMDIntegrateMpm(LAMMPS *lmp, int narg, char **arg) :
		Fix(lmp, narg, arg) {

	if ((atom->e_flag != 1) || (atom->vfrac_flag != 1))
		error->all(FLERR, "fix smd/integrate_mpm command requires atom_style with both energy and volume");

	if (narg < 3)
		error->all(FLERR, "Illegal number of arguments for fix smd/integrate_mpm command");

	adjust_radius_flag = false;
	vlimit = -1.0;
	smooth_density_field_factor = 0.0;
	int iarg = 3;

	if (comm->me == 0) {
		printf("\n>>========>>========>>========>>========>>========>>========>>========>>========\n");
		printf("fix smd/integrate_mpm is active for group: %s \n", arg[1]);
	}
	while (true) {

		if (iarg >= narg) {
			break;
		}

		if (strcmp(arg[iarg], "limit_velocity") == 0) {
			iarg++;
			if (iarg == narg) {
				error->all(FLERR, "expected number following limit_velocity");
			}
			vlimit = force->numeric(FLERR, arg[iarg]);

			if (comm->me == 0) {
				printf("... will limit velocities to <= %g\n", vlimit);
			}
		} else {
			char msg[128];
			sprintf(msg, "Illegal keyword for smd/integrate_mpm: %s\n", arg[iarg]);
			error->all(FLERR, msg);
		}

		iarg++;

	}

	if (comm->me == 0) {
		printf(">>========>>========>>========>>========>>========>>========>>========>>========\n\n");
	}

	// set comm sizes needed by this fix
	atom->add_callback(0);

	time_integrate = 1;
}

/* ---------------------------------------------------------------------- */

int FixSMDIntegrateMpm::setmask() {
	int mask = 0;
	mask |= INITIAL_INTEGRATE;
	mask |= FINAL_INTEGRATE;
	return mask;
}

/* ---------------------------------------------------------------------- */

void FixSMDIntegrateMpm::init() {
	dtv = update->dt;
	dtf = 0.5 * update->dt * force->ftm2v;
	vlimitsq = vlimit * vlimit;
}

/* ----------------------------------------------------------------------
 allow for both per-type and per-atom mass
 ------------------------------------------------------------------------- */

void FixSMDIntegrateMpm::initial_integrate(int vflag) {
	double **v = atom->v;
	double **f = atom->f;
	double **vest = atom->vest;
	double *rmass = atom->rmass;

	int *mask = atom->mask;
	int nlocal = atom->nlocal;
	double dtfm;
	int i;

	if (igroup == atom->firstgroup)
		nlocal = atom->nfirst;

//	for (i = 0; i < nlocal; i++) {
//		if (mask[i] & groupbit) {
//			dtfm = dtv / rmass[i];
//
//			v[i][0] += dtfm * f[i][0];
//			v[i][1] += dtfm * f[i][1];
//			v[i][2] += dtfm * f[i][2];
//
//			// extrapolate velocity from half- to full-step
//			vest[i][0] = v[i][0] + dtfm * f[i][0];
//			vest[i][1] = v[i][1] + dtfm * f[i][1];
//			vest[i][2] = v[i][2] + dtfm * f[i][2];
//
//		}
//	}

}

/* ---------------------------------------------------------------------- */

void FixSMDIntegrateMpm::final_integrate() {

	double **x = atom->x;
	double **v = atom->v;
	double **f = atom->f;
	double *e = atom->e;
	double *de = atom->de;
	double *rmass = atom->rmass;

	int *mask = atom->mask;
	int nlocal = atom->nlocal;
	double vsq, scale, dtfm;
	int i, itmp;

	Vector3d *particleVelocities = (Vector3d *) force->pair->extract("smd/mpm/particleVelocities_ptr", itmp);
	if (particleVelocities == NULL) {
		error->one(FLERR, "fix smd/integrate_mpm failed to accesss particleVelocities array");
	}

	Vector3d *particleAccelerations = (Vector3d *) force->pair->extract("smd/mpm/particleAccelerations_ptr", itmp);
	if (particleAccelerations == NULL) {
		error->one(FLERR, "fix smd/integrate_mpm failed to accesss particleAccelerations array");
	}

	if (igroup == atom->firstgroup)
		nlocal = atom->nfirst;

	for (i = 0; i < nlocal; i++) {
		if (mask[i] & groupbit) {

			if (vlimit > 0.0) {
				vsq = v[i][0] * v[i][0] + v[i][1] * v[i][1] + v[i][2] * v[i][2];
				if (vsq > vlimitsq) {
					scale = sqrt(vlimitsq / vsq);
					v[i][0] *= scale;
					v[i][1] *= scale;
					v[i][2] *= scale;
				}
			}

			dtfm = dtv / rmass[i];
			//v[i][0] = particleVelocities[i](0) + dtv * particleAccelerations[i](0) + dtfm * f[i][0];
			//v[i][1] = particleVelocities[i](1) + dtv * particleAccelerations[i](1) + dtfm * f[i][1];
			//v[i][2] = particleVelocities[i](2) + dtv * particleAccelerations[i](2) + dtfm * f[i][2];
			v[i][0] = particleVelocities[i](0) + dtfm * f[i][0];
			v[i][1] = particleVelocities[i](1) + dtfm * f[i][1];
			v[i][2] = particleVelocities[i](2) + dtfm * f[i][2];

			x[i][0] += dtv * v[i][0];
			x[i][1] += dtv * v[i][1];
			x[i][2] += dtv * v[i][2];

			v[i][0] += dtv * particleAccelerations[i](0);
			v[i][1] += dtv * particleAccelerations[i](1);
			v[i][2] += dtv * particleAccelerations[i](2);

			// ODER --
//			x[i][0] += dtv * particleVelocities[i](0);
//			x[i][1] += dtv * particleVelocities[i](1);
//			x[i][2] += dtv * particleVelocities[i](2);
//
//			v[i][0] += dtv * particleAccelerations[i](0) + dtfm * f[i][0];
//			v[i][1] += dtv * particleAccelerations[i](1) + dtfm * f[i][1];
//			v[i][2] += dtv * particleAccelerations[i](2) + dtfm * f[i][2];

			e[i] += dtv * de[i];

		}
	}

}

/* ---------------------------------------------------------------------- */

void FixSMDIntegrateMpm::reset_dt() {
	dtv = update->dt;
	dtf = 0.5 * update->dt * force->ftm2v;
}

