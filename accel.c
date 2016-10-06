#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gsl/gsl_math.h>

#include "allvars.h"
#include "proto.h"

#ifdef VORONOI
#include "voronoi.h"
#endif



#ifdef CS_MODEL
#include "cs_metals.h"
#endif

/*! \file accel.c
 *  \brief driver routines to carry out force computation
 */


/*! This routine computes the accelerations for all active particles.  First, the gravitational forces are
 * computed. This also reconstructs the tree, if needed, otherwise the drift/kick operations have updated the
 * tree to make it fullu usable at the current time.
 *
 * If gas particles are presented, the `interior' of the local domain is determined. This region is guaranteed
 * to contain only particles local to the processor. This information will be used to reduce communication in
 * the hydro part.  The density for active SPH particles is computed next. If the number of neighbours should
 * be outside the allowed bounds, it will be readjusted by the function ensure_neighbours(), and for those
 * particle, the densities are recomputed accordingly. Finally, the hydrodynamical forces are added.
 */
void compute_accelerations(int mode)
{
#ifdef RADTRANSFER
  double timeeach = 0, timeall = 0, tstart = 0, tend = 0;
#endif
#if defined(BUBBLES) || defined(MULTI_BUBBLES)
  double hubble_a;
#endif

  if(ThisTask == 0)
    {
      printf("Start force computation...\n");
      fflush(stdout);
    }

#ifdef REIONIZATION
  heating();
#endif

  CPU_Step[CPU_MISC] += measure_time();

#ifdef PMGRID
  if(All.PM_Ti_endstep == All.Ti_Current)
    {
      long_range_force();

      CPU_Step[CPU_MESH] += measure_time();

    }
#endif


#ifndef ONLY_PM
#ifdef GRAVITY_CENTROID

  CPU_Step[CPU_MISC] += measure_time();

  /* set new softening lengths */
#if !defined(SIM_ADAPTIVE_SOFT) && !defined(SIM_COMOVING_SOFT)
  if(All.ComovingIntegrationOn)
    set_softenings();
#endif

  /* contruct tree if needed */
  if(TreeReconstructFlag)
    {
      if(ThisTask == 0)
	printf("Tree construction.  (presently allocated=%g MB)\n", AllocatedBytes / (1024.0 * 1024.0));

      CPU_Step[CPU_MISC] += measure_time();

      force_treebuild(NumPart, NULL);

      CPU_Step[CPU_TREEBUILD] += measure_time();

      TreeReconstructFlag = 0;

      if(ThisTask == 0)
	printf("Tree construction done.\n");
    }

#else
  gravity_tree();		/* computes gravity accel. */

  if(All.TypeOfOpeningCriterion == 1 && All.Ti_Current == 0)
    gravity_tree();		/* For the first timestep, we redo it
				 * to allow usage of relative opening
				 * criterion for consistent accuracy.
				 */
#endif
#endif


  if(All.Ti_Current == 0 && RestartFlag == 0 && header.flag_ic_info == FLAG_SECOND_ORDER_ICS)
    second_order_ics();		/* produces the actual ICs from the special second order IC file */


#ifdef FORCETEST
  gravity_forcetest();
#endif


  if(All.TotN_gas > 0)
    {
      /***** density *****/
      if(ThisTask == 0)
	{
	  printf("Start density computation...\n");
	  fflush(stdout);
	}

#ifdef CS_MODEL
      CPU_Step[CPU_MISC] += measure_time();

#if defined(CS_SNI) || defined(CS_SNII)
      cs_flag_SN_starparticles();	/* mark SNI star particles */
#endif
      cs_copy_densities();
      cs_find_low_density_tail();

      CPU_Step[CPU_CSMISC] += measure_time();
#endif

#ifndef VORONOI
      density();		/* computes density, and pressure */
#else
      voronoi_mesh();
      voronoi_setup_exchange();

      voronoi_density();
#endif

#if (defined(DIVBCLEANING_DEDNER) || defined(SMOOTH_ROTB) || defined(BSMOOTH) || defined(VECT_POTENTIAL) || (defined(LT_STELLAREVOLUTION) && !defined(LT_DONTUSE_DENSITY_in_WEIGHT)) || defined(LT_SMOOTH_Z) || defined(LT_SMOOTH_XCLD) || defined(LT_TRACK_WINDS) || defined(LT_BH) || defined(LT_BH_LOG) )
      smoothed_values();
#endif


#if defined(SNIA_HEATING)
      snIa_heating();
#endif


#if defined(CS_MODEL) && defined(CS_FEEDBACK)

      CPU_Step[CPU_CSMISC] += measure_time();

      cs_find_hot_neighbours();

      cs_promotion();
      cs_copy_densities();
      CPU_Step[CPU_CSMISC] += measure_time();
      density();		/* recalculate densities again */
      CPU_Step[CPU_CSMISC] += measure_time();
#endif

      /***** update smoothing lengths in tree *****/
#ifndef VORONOI
      force_update_hmax();
#endif

      /***** hydro forces *****/
      if(ThisTask == 0)
	{
	  printf("Start hydro-force computation...\n");
	  fflush(stdout);
	}

#ifndef VORONOI
      hydro_force();		/* adds hydrodynamical accelerations  and computes du/dt  */
#else
      voronoi_hydro_force();
#endif

#ifdef CONDUCTION
      if(All.Conduction_Ti_endstep == All.Ti_Current)
	conduction();
#endif

#ifdef CR_DIFFUSION
      if(All.CR_Diffusion_Ti_endstep == All.Ti_Current)
	cosmic_ray_diffusion();
#endif

#ifdef RADTRANSFER
      if(Flag_FullStep)		/* only do it for full timesteps */
	{
	  All.Radiation_Ti_endstep = All.Ti_Current;


	  if(ThisTask == 0)
	    {
	      printf("Start Eddington tensor computation...\n");
	      fflush(stdout);
	    }

	  eddington();

#ifdef RT_RAD_PRESSURE
	  n();
#endif

	  if(ThisTask == 0)
	    {
	      printf("done Eddington tensor! \n");
	      fflush(stdout);
	    }

#ifdef EDDINGTON_TENSOR_SFR
	  density_sfr();
	  sfr_lum();
#endif

#ifdef EDDINGTON_TENSOR_STARS
	  rt_get_lum_stars();
	  star_lum();
#endif

#ifdef EDDINGTON_TENSOR_GAS
	  gas_lum();
#endif

#ifdef EDDINGTON_TENSOR_BH
	  bh_lum();
#endif

	  /***** evolve the transport of radiation *****/
	  if(ThisTask == 0)
	    {
	      printf("start radtransfer...\n");
	      fflush(stdout);
	    }

	  tstart = second();

	  radtransfer();

	  radtransfer_update_chemistry();
	  
	  tend = second();
	  timeeach = timediff(tstart, tend);
	  MPI_Allreduce(&timeeach, &timeall, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

	  if(ThisTask == 0)
	    {
	      printf("time consumed is %g \n", timeall);
	      printf("done with radtransfer! \n");
	      fflush(stdout);
	    }

	  All.Radiation_Ti_begstep = All.Radiation_Ti_endstep;
	}
#endif

#ifdef MHM
      /***** kinetic feedback *****/
      kinetic_feedback_mhm();
#endif


#ifdef BLACK_HOLES
      /***** black hole accretion and feedback *****/
      blackhole_accretion();
#ifdef FOF
      /* this will find new black hole seed halos */
      if(All.Time >= All.TimeNextBlackHoleCheck)
	{
	  fof_fof(-1);

	  if(All.ComovingIntegrationOn)
	    All.TimeNextBlackHoleCheck *= All.TimeBetBlackHoleSearch;
	  else
	    All.TimeNextBlackHoleCheck += All.TimeBetBlackHoleSearch;
	}
#endif
#endif


#ifdef COOLING	/**** radiative cooling and star formation *****/

#ifdef CS_MODEL
      cs_cooling_and_starformation();
#else

#ifdef SFR
      cooling_and_starformation();

#ifdef LT_STELLAREVOLUTION
      if(ThisTask == 0)
	{
	  printf("Start supernovae computation...\n");
	  fflush(stdout);
	}

      //tstartSn = second();

      evolve_SN();

      //tendSn = second();
      //      CPU_sev = timediff(tstartSn, tendSn);

      //MPI_Reduce(&CPU_sev, &sumCPU_sev, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
      //if(ThisTask == 0)
      //All.CPU_SEv += sumCPU_sev / NTask;
#endif

#else
      cooling_only();
#endif

#endif
      CPU_Step[CPU_COOLINGSFR] += measure_time();
#endif /*ends COOLING */

#ifdef CHEMCOOL
      do_chemcool(-1, 0);
#endif

#if defined(CS_MODEL) && defined(CS_ENRICH)
#ifndef CS_FEEDBACK
      Flag_phase = 0;		/* no destinction between phases */

      cs_update_weights();
      CPU_Step[CPU_WEIGHTS_HOT] += measure_time();
      cs_enrichment();
      CPU_Step[CPU_ENRICH_HOT] += measure_time();
#else

      Flag_phase = 1;		/* COLD phase Flag_phase  = 1 */

      cs_update_weights();
      CPU_Step[CPU_WEIGHTS_HOT] += measure_time();
      cs_enrichment();
      CPU_Step[CPU_ENRICH_HOT] += measure_time();

      Flag_phase = 2;		/* HOT phase Flag_phase = 2 */

      cs_update_weights();
      CPU_Step[CPU_WEIGHTS_COLD] += measure_time();
      cs_enrichment();
      CPU_Step[CPU_ENRICH_COLD] += measure_time();

      Flag_phase = 0;
#endif
#endif

#ifdef CS_TESTS
      cs_energy_test();
#endif



#ifndef BH_BUBBLES
#ifdef BUBBLES
      /**** bubble feedback *****/
      if(All.Time >= All.TimeOfNextBubble)
	{
#ifdef FOFs
	  fof_fof(-1);
	  bubble();
#else
	  bubble();
#endif
	  if(All.ComovingIntegrationOn)
	    {
	      hubble_a = hubble_function(All.Time);
	      All.TimeOfNextBubble *= (1.0 + All.BubbleTimeInterval * hubble_a);
	    }
	  else
	    All.TimeOfNextBubble += All.BubbleTimeInterval / All.UnitTime_in_Megayears;

	  if(ThisTask == 0)
	    printf("Time of the bubble generation: %g\n", 1. / All.TimeOfNextBubble - 1.);
	}
#endif
#endif

#if defined(MULTI_BUBBLES) && defined(FOF)
      if(All.Time >= All.TimeOfNextBubble)
	{
	  fof_fof(-1);

	  if(All.ComovingIntegrationOn)
	    {
	      hubble_a = hubble_func(All.Time);
	      All.TimeOfNextBubble *= (1.0 + All.BubbleTimeInterval * hubble_a);
	    }
	  else
	    All.TimeOfNextBubble += All.BubbleTimeInterval / All.UnitTime_in_Megayears;

	  if(ThisTask == 0)
	    printf("Time of the bubble generation: %g\n", 1. / All.TimeOfNextBubble - 1.);
	}
#endif

    }

#ifdef GRAVITY_CENTROID
#ifndef ONLY_PM

  force_update_node_center_of_mass_recursive(All.MaxPart, -1, -1);

  force_exchange_pseudodata();

  force_treeupdate_pseudos(All.MaxPart);


  gravity_tree();		/* computes gravity accel. */

  if(All.TypeOfOpeningCriterion == 1 && All.Ti_Current == 0)
    gravity_tree();		/* For the first timestep, we redo it
				 * to allow usage of relative opening
				 * criterion for consistent accuracy.
				 */
#endif
#endif


  if(ThisTask == 0)
    {
      printf("force computation done.\n");
      fflush(stdout);
    }
}
