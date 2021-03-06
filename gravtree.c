#include <mpi.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#ifdef NUM_THREADS
#include <pthread.h>
#endif
#include "allvars.h"
#include "proto.h"

/*! \file gravtree.c
 *  \brief main driver routines for gravitational (short-range) force computation
 *
 *  This file contains the code for the gravitational force computation by
 *  means of the tree algorithm. To this end, a tree force is computed for all
 *  active local particles, and particles are exported to other processors if
 *  needed, where they can receive additional force contributions. If the
 *  TreePM algorithm is enabled, the force computed will only be the
 *  short-range part.
 */

#ifdef NUM_THREADS
pthread_mutex_t mutex_nexport;
pthread_mutex_t mutex_workcount;
pthread_mutex_t mutex_partnodedrift;

#define LOCK_NEXPORT     pthread_mutex_lock(&mutex_nexport);
#define UNLOCK_NEXPORT   pthread_mutex_unlock(&mutex_nexport);
#define LOCK_WORKCOUNT   pthread_mutex_lock(&mutex_workcount);
#define UNLOCK_WORKCOUNT pthread_mutex_unlock(&mutex_workcount);

#else
#define LOCK_NEXPORT
#define UNLOCK_NEXPORT
#define LOCK_WORKCOUNT
#define UNLOCK_WORKCOUNT
#endif



int NextParticle;
int Nexport, Nimport;
int BufferFullFlag;
int NextJ;
int TimerFlag;


double Ewaldcount, Costtotal;
long long N_nodesinlist;


int Ewald_iter;			/* global in file scope, for simplicity */


#ifdef SHELL_CODE
static struct radius_data
{
  MyDouble radius;
  MyDouble enclosed_mass;
  MyDouble dMdr;
  int GrNr;
  int SubNr;
}
 *rad_data;

int compare_radius(const void *a, const void *b)
{
  if(((struct radius_data *) a)->radius < ((struct radius_data *) b)->radius)
    return -1;

  if(((struct radius_data *) a)->radius > ((struct radius_data *) b)->radius)
    return +1;

  return 0;
}

int compare_GrNr_SubNr(const void *a, const void *b)
{
  if(((struct radius_data *) a)->GrNr < (((struct radius_data *) b)->GrNr))
    return -1;

  if(((struct radius_data *) a)->GrNr > (((struct radius_data *) b)->GrNr))
    return +1;

  if(((struct radius_data *) a)->SubNr < (((struct radius_data *) b)->SubNr))
    return -1;

  if(((struct radius_data *) a)->SubNr > (((struct radius_data *) b)->SubNr))
    return +1;

  return 0;
}
#endif




#ifdef REINIT_AT_TURNAROUND_CMS
void calculate_centre_of_mass(void)
{
  int i, iter = 0;
  double cms_mass_local = 0.0, cms_x_local = 0.0, cms_y_local = 0.0, cms_z_local = 0.0, cms_N_local = 0.0;
  double cms_mass_global = 0.0, cms_x_global = 0.0, cms_y_global = 0.0, cms_z_global = 0.0, cms_N_global =
    Ntype[1] * 1.0;
  double r_part, max_r_local = 0.0, max_r_global = 0.0;

  /* get maximum radius */
  for(i = 0; i < NumPart; i++)
    {
      r_part = sqrt((P[i].Pos[0] - cms_x_global) * (P[i].Pos[0] - cms_x_global) +
		    (P[i].Pos[1] - cms_y_global) * (P[i].Pos[1] - cms_y_global) +
		    (P[i].Pos[2] - cms_z_global) * (P[i].Pos[2] - cms_z_global));
      if(r_part > max_r_local)
	max_r_local = r_part;
    }

  MPI_Allreduce(&max_r_local, &max_r_global, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

  if(ThisTask == 0)
    {
      printf("REINIT_AT_TURNAROUND_CMS: max_r=%g\n", max_r_global);
      fflush(stdout);
    }


  while((cms_N_global > 1000.0) || (cms_N_global > 0.1 * Ntype[1]))
    {
      /* reset local numbers */
      cms_x_local = 0.0;
      cms_y_local = 0.0;
      cms_z_local = 0.0;
      cms_mass_local = 0.0;
      cms_N_local = 0.0;


      /* get local centre of mass contributions */
      for(i = 0; i < NumPart; i++)
	{
	  r_part = sqrt((P[i].Pos[0] - cms_x_global) * (P[i].Pos[0] - cms_x_global) +
			(P[i].Pos[1] - cms_y_global) * (P[i].Pos[1] - cms_y_global) +
			(P[i].Pos[2] - cms_z_global) * (P[i].Pos[2] - cms_z_global));



	  if(r_part < max_r_global * pow(REINIT_AT_TURNAROUND_CMS, iter))
	    {
	      cms_x_local += P[i].Mass * P[i].Pos[0];
	      cms_y_local += P[i].Mass * P[i].Pos[1];
	      cms_z_local += P[i].Mass * P[i].Pos[2];
	      cms_mass_local += P[i].Mass;
	      cms_N_local += 1.0;
	    }
	}

      /* collect data and sum up */
      MPI_Allreduce(&cms_mass_local, &cms_mass_global, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
      MPI_Allreduce(&cms_x_local, &cms_x_global, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
      MPI_Allreduce(&cms_y_local, &cms_y_global, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
      MPI_Allreduce(&cms_z_local, &cms_z_global, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
      MPI_Allreduce(&cms_N_local, &cms_N_global, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

      cms_x_global /= cms_mass_global;
      cms_y_global /= cms_mass_global;
      cms_z_global /= cms_mass_global;

      if(ThisTask == 0)
	{
	  printf("REINIT_AT_TURNAROUND_CMS: cms_N=%g  iter=%d  R=%g\n", cms_N_global, iter,
		 max_r_global * pow(REINIT_AT_TURNAROUND_CMS, iter));
	  printf("REINIT_AT_TURNAROUND_CMS: r_cms=(%g,%g,%g)\n", cms_x_global, cms_y_global, cms_z_global);
	  fflush(stdout);
	}

      iter++;
    }


  All.cms_x = cms_x_global;
  All.cms_y = cms_y_global;
  All.cms_z = cms_z_global;

  if(ThisTask == 0)
    {
      printf("REINIT_AT_TURNAROUND_CMS: time=%g r_cms=(%g,%g,%g) mass=%g r_TA=%g\n",
	     All.Time, All.cms_x, All.cms_y, All.cms_z, cms_mass_global, All.CurrentTurnaroundRadius);
      fflush(stdout);
    }

}
#endif


/*! This function computes the gravitational forces for all active particles.
 *  If needed, a new tree is constructed, otherwise the dynamically updated
 *  tree is used.  Particles are only exported to other processors when really
 *  needed, thereby allowing a good use of the communication buffer.
 */
void gravity_tree(void)
{
  long long n_exported = 0;
  int i, j, maxnumnodes, iter = 0;
  double t0, t1;
  double timeall = 0, timetree1 = 0, timetree2 = 0;
  double timetree, timewait, timecomm;
  double timecommsumm1 = 0, timecommsumm2 = 0, timewait1 = 0, timewait2 = 0;
  double sum_costtotal, ewaldtot;
  double maxt, sumt, maxt1, sumt1, maxt2, sumt2, sumcommall, sumwaitall;
  double plb, plb_max;

#ifdef FIXEDTIMEINFIRSTPHASE
  int counter;
  double min_time_first_phase, min_time_first_phase_glob;
#endif
#ifndef NOGRAVITY
  int k, ewald_max, save_NextParticle;
  int ndone, ndone_flag, ngrp;
  int place;
  int sendTask, recvTask;
  double tstart, tend, ax, ay, az;
  MPI_Status status;

#ifdef DISTORTIONTENSORPS
  int i1, i2;
#endif
#endif

#ifndef GRAVITY_CENTROID
  CPU_Step[CPU_MISC] += measure_time();

  /* set new softening lengths */
#ifndef SIM_ADAPTIVE_SOFT
  if(All.ComovingIntegrationOn)
    set_softenings();
#endif

#if PHYS_COMOVING_SOFT
  set_softenings();
#endif

  /* construct tree if needed */
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
#endif

#ifndef NOGRAVITY


#if defined(SIM_ADAPTIVE_SOFT) || defined(REINIT_AT_TURNAROUND)
  double turnaround_radius_local = 0.0, turnaround_radius_global = 0.0, v_part, r_part;

#ifdef REINIT_AT_TURNAROUND_CMS
  calculate_centre_of_mass();
#endif

  /* get local turnaroundradius */
  for(i = 0; i < NumPart; i++)
    {
      r_part = sqrt((P[i].Pos[0] - All.cms_x) * (P[i].Pos[0] - All.cms_x) +
		    (P[i].Pos[1] - All.cms_y) * (P[i].Pos[1] - All.cms_y) +
		    (P[i].Pos[2] - All.cms_z) * (P[i].Pos[2] - All.cms_z));
      v_part = (P[i].Pos[0] * P[i].Vel[0] + P[i].Pos[1] * P[i].Vel[1] + P[i].Pos[2] * P[i].Vel[2]);
      if((v_part < 0.0) && (r_part > turnaround_radius_local))
	turnaround_radius_local = r_part;
    }

  /* find global turnaround radius by taking maximum of all CPUs */
  MPI_Allreduce(&turnaround_radius_local, &turnaround_radius_global, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

#ifdef ANALYTIC_TURNAROUND
#ifdef COMOVING_DISTORTION
  /* comoving turnaround radius */
  All.CurrentTurnaroundRadius =
    All.InitialTurnaroundRadius * pow(All.Time / All.TimeBegin, 1.0 / (3.0 * All.SIM_epsilon));
#else
  All.CurrentTurnaroundRadius =
    All.InitialTurnaroundRadius * pow(All.Time / All.TimeBegin,
				      2.0 / 3.0 + 2.0 / (3.0 * 3 * All.SIM_epsilon));
#endif
#else
  All.CurrentTurnaroundRadius = turnaround_radius_global;
#endif /* ANALYTIC_TURNAROUND */

  if(ThisTask == 0)
    {
      printf("REINIT_AT_TURNAROUND: current turnaround radius = %g\n", All.CurrentTurnaroundRadius);
      fflush(stdout);
    }

#ifdef SIM_ADAPTIVE_SOFT
  if(ThisTask == 0)
    {
#ifdef ANALYTIC_TURNAROUND
#ifdef COMOVING_DISTORTION
      printf("COMOVING_DISTORTION: comoving turnaround radius = %g\n", All.CurrentTurnaroundRadius);
#else
      printf("SIM/SHEL_CODE adaptive core softening: simulation turnaround radius = %g\n",
	     turnaround_radius_global);
      printf("SIM/SHEL_CODE adaptive core softening: analytic turnaround radius   = %g\n",
	     All.CurrentTurnaroundRadius);
#endif
#else
      printf("SIM/TREE adaptive softening: current turnaround radius  = %g\n", All.CurrentTurnaroundRadius);
#endif /* ANALYTIC_TURNAROUND */
      fflush(stdout);
    }

  /* set the table values, because it is used for the time stepping, the Plummer equivalent softening length */
  All.SofteningTable[0] = All.CurrentTurnaroundRadius * All.SofteningGas;
  All.SofteningTable[1] = All.CurrentTurnaroundRadius * All.SofteningHalo;
  All.SofteningTable[2] = All.CurrentTurnaroundRadius * All.SofteningDisk;
  All.SofteningTable[3] = All.CurrentTurnaroundRadius * All.SofteningBulge;
  All.SofteningTable[4] = All.CurrentTurnaroundRadius * All.SofteningStars;
  All.SofteningTable[5] = All.CurrentTurnaroundRadius * All.SofteningBndry;

  /* this is used in tree, the spline softening length */
  for(i = 0; i < 6; i++)
    All.ForceSoftening[i] = 2.8 * All.SofteningTable[i];
#endif /* SIM_ADAPTIVE_SOFT */
#endif /* SIM_ADAPTIVE_SOFT || REINIT_AT_TURNAROUND */


  /* allocate buffers to arrange communication */
  if(ThisTask == 0)
    printf("Begin tree force.  (presently allocated=%g MB)\n", AllocatedBytes / (1024.0 * 1024.0));

  All.BunchSize =
    (int) ((All.BufferSize * 1024 * 1024) / (sizeof(struct data_index) + sizeof(struct data_nodelist) +
					     sizeof(struct gravdata_in) + sizeof(struct gravdata_out) +
					     sizemax(sizeof(struct gravdata_in),
						     sizeof(struct gravdata_out))));
  DataIndexTable =
    (struct data_index *) mymalloc("DataIndexTable", All.BunchSize * sizeof(struct data_index));
  DataNodeList =
    (struct data_nodelist *) mymalloc("DataNodeList", All.BunchSize * sizeof(struct data_nodelist));

  if(ThisTask == 0)
    printf("All.BunchSize=%d\n", All.BunchSize);

  Ewaldcount = 0;
  N_nodesinlist = 0;


  CPU_Step[CPU_TREEMISC] += measure_time();
  t0 = second();

#if defined(PERIODIC) && !defined(PMGRID) && !defined(GRAVITY_NOT_PERIODIC)
  ewald_max = 1;
#else
  ewald_max = 0;
#endif

#ifdef SCF_HYBRID
  int scf_counter;
  /* 
   calculates the following forces:
   STAR<->STAR, STAR->DM (scf_counter=0)
   DM<->DM (scf_counter=1)
  */  
   
  for(scf_counter = 0; scf_counter <= 1; scf_counter++)
  {  
   /* set DM mass to zero and set gravsum to zero */
   if (scf_counter==0)
    { 
     for(i = 0; i < NumPart; i++)
      {
       if (P[i].Type==1) /* DM particle */
        P[i].Mass=0.0;

       for(j = 0; j < 3; j++)
	 P[i].GravAccelSum[j] = 0.0;	
      }
    }
   /* set stellar mass to zero */    
   if (scf_counter==1)
    { 
     for(i = 0; i < NumPart; i++)
      {
       if (P[i].Type==2) /* stellar particle */
        P[i].Mass=0.0;
      }
    }

   /* particle masses changed, so reconstruct tree */
   if(ThisTask == 0) 
    printf("SCF Tree construction %d\n", scf_counter);
   force_treebuild(NumPart, NULL);
   if(ThisTask == 0)
    printf("done.\n");    
#endif
  for(Ewald_iter = 0; Ewald_iter <= ewald_max; Ewald_iter++)
    {

      NextParticle = FirstActiveParticle;	/* beginn with this index */

      do
	{
	  iter++;
	  BufferFullFlag = 0;
	  Nexport = 0;
	  save_NextParticle = NextParticle;

	  tstart = second();

#ifdef NUM_THREADS
	  pthread_t mythreads[NUM_THREADS - 1];
	  int threadid[NUM_THREADS - 1];
	  pthread_attr_t attr;

	  pthread_attr_init(&attr);
	  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	  pthread_mutex_init(&mutex_workcount, NULL);
	  pthread_mutex_init(&mutex_nexport, NULL);
	  pthread_mutex_init(&mutex_partnodedrift, NULL);

	  TimerFlag = 0;

	  for(j = 0; j < NUM_THREADS - 1; j++)
	    {
	      threadid[j] = j + 1;
	      pthread_create(&mythreads[j], &attr, gravity_primary_loop, &threadid[j]);
	    }
#endif
	  int mainthreadid = 0;

	  gravity_primary_loop(&mainthreadid);	/* do local particles and prepare export list */

#ifdef NUM_THREADS
	  for(j = 0; j < NUM_THREADS - 1; j++)
	    pthread_join(mythreads[j], NULL);
#endif

	  tend = second();
	  timetree1 += timediff(tstart, tend);


	  if(BufferFullFlag)
	    {
	      int last_nextparticle = NextParticle;

	      NextParticle = save_NextParticle;

	      while(NextParticle >= 0)
		{
		  if(NextParticle == last_nextparticle)
		    break;

		  if(ProcessedFlag[NextParticle] != 1)
		    break;

		  ProcessedFlag[NextParticle] = 2;

		  NextParticle = NextActiveParticle[NextParticle];
		}

	      if(NextParticle == save_NextParticle)
		{
		  /* in this case, the buffer is too small to process even a single particle */
		  endrun(12998);
		}


	      int new_export = 0;

	      for(j = 0, k = 0; j < Nexport; j++)
		if(ProcessedFlag[DataIndexTable[j].Index] != 2)
		  {
		    if(k < j + 1)
		      k = j + 1;

		    for(; k < Nexport; k++)
		      if(ProcessedFlag[DataIndexTable[k].Index] == 2)
			{
			  int old_index = DataIndexTable[j].Index;

			  DataIndexTable[j] = DataIndexTable[k];
			  DataNodeList[j] = DataNodeList[k];
			  DataIndexTable[j].IndexGet = j;
			  new_export++;

			  DataIndexTable[k].Index = old_index;
			  k++;
			  break;
			}
		  }
		else
		  new_export++;

	      Nexport = new_export;

	    }


	  n_exported += Nexport;

	  for(j = 0; j < NTask; j++)
	    Send_count[j] = 0;
	  for(j = 0; j < Nexport; j++)
	    Send_count[DataIndexTable[j].Task]++;


#ifdef MYSORT
	  mysort_dataindex(DataIndexTable, Nexport, sizeof(struct data_index), data_index_compare);
#else
	  qsort(DataIndexTable, Nexport, sizeof(struct data_index), data_index_compare);
#endif


	  tstart = second();

	  MPI_Alltoall(Send_count, 1, MPI_INT, Recv_count, 1, MPI_INT, MPI_COMM_WORLD);

	  tend = second();
	  timewait1 += timediff(tstart, tend);


	  for(j = 0, Nimport = 0, Recv_offset[0] = 0, Send_offset[0] = 0; j < NTask; j++)
	    {
	      Nimport += Recv_count[j];

	      if(j > 0)
		{
		  Send_offset[j] = Send_offset[j - 1] + Send_count[j - 1];
		  Recv_offset[j] = Recv_offset[j - 1] + Recv_count[j - 1];
		}
	    }

	  GravDataGet = (struct gravdata_in *) mymalloc("GravDataGet", Nimport * sizeof(struct gravdata_in));
	  GravDataIn = (struct gravdata_in *) mymalloc("GravDataIn", Nexport * sizeof(struct gravdata_in));

	  /* prepare particle data for export */

	  for(j = 0; j < Nexport; j++)
	    {
	      place = DataIndexTable[j].Index;

#ifdef GRAVITY_CENTROID
	      if(P[place].Type == 0)
		{
		  for(k = 0; k < 3; k++)
		    GravDataIn[j].Pos[k] = SphP[place].Center[k];
		}
	      else
		{
		  for(k = 0; k < 3; k++)
		    GravDataIn[j].Pos[k] = P[place].Pos[k];
		}
#else
	      for(k = 0; k < 3; k++)
		GravDataIn[j].Pos[k] = P[place].Pos[k];
#endif

#if defined(UNEQUALSOFTENINGS) || defined(SCALARFIELD)
	      GravDataIn[j].Type = P[place].Type;
#ifdef ADAPTIVE_GRAVSOFT_FORGAS
	      if(P[place].Type == 0)
		GravDataIn[j].Soft = SphP[place].Hsml;
#endif
#endif
	      GravDataIn[j].OldAcc = P[place].OldAcc;

	      memcpy(GravDataIn[j].NodeList,
		     DataNodeList[DataIndexTable[j].IndexGet].NodeList, NODELISTLENGTH * sizeof(int));
	    }


	  /* exchange particle data */

	  tstart = second();
	  for(ngrp = 1; ngrp < (1 << PTask); ngrp++)
	    {
	      sendTask = ThisTask;
	      recvTask = ThisTask ^ ngrp;

	      if(recvTask < NTask)
		{
		  if(Send_count[recvTask] > 0 || Recv_count[recvTask] > 0)
		    {
		      /* get the particles */
		      MPI_Sendrecv(&GravDataIn[Send_offset[recvTask]],
				   Send_count[recvTask] * sizeof(struct gravdata_in), MPI_BYTE,
				   recvTask, TAG_GRAV_A,
				   &GravDataGet[Recv_offset[recvTask]],
				   Recv_count[recvTask] * sizeof(struct gravdata_in), MPI_BYTE,
				   recvTask, TAG_GRAV_A, MPI_COMM_WORLD, &status);
		    }
		}
	    }
	  tend = second();
	  timecommsumm1 += timediff(tstart, tend);


	  myfree(GravDataIn);
	  GravDataResult =
	    (struct gravdata_out *) mymalloc("GravDataResult", Nimport * sizeof(struct gravdata_out));
	  GravDataOut =
	    (struct gravdata_out *) mymalloc("GravDataOut", Nexport * sizeof(struct gravdata_out));

	  report_memory_usage(&HighMark_gravtree, "GRAVTREE");

	  /* now do the particles that were sent to us */
	  tstart = second();

	  NextJ = 0;

#ifdef NUM_THREADS
	  for(j = 0; j < NUM_THREADS - 1; j++)
	    pthread_create(&mythreads[j], &attr, gravity_secondary_loop, &threadid[j]);
#endif
	  gravity_secondary_loop(&mainthreadid);

#ifdef NUM_THREADS
	  for(j = 0; j < NUM_THREADS - 1; j++)
	    pthread_join(mythreads[j], NULL);

	  pthread_mutex_destroy(&mutex_partnodedrift);
	  pthread_mutex_destroy(&mutex_nexport);
	  pthread_mutex_destroy(&mutex_workcount);
	  pthread_attr_destroy(&attr);
#endif

	  tend = second();
	  timetree2 += timediff(tstart, tend);

	  if(NextParticle < 0)
	    ndone_flag = 1;
	  else
	    ndone_flag = 0;

	  tstart = second();
	  MPI_Allreduce(&ndone_flag, &ndone, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
	  tend = second();
	  timewait2 += timediff(tstart, tend);


	  /* get the result */
	  tstart = second();
	  for(ngrp = 1; ngrp < (1 << PTask); ngrp++)
	    {
	      sendTask = ThisTask;
	      recvTask = ThisTask ^ ngrp;
	      if(recvTask < NTask)
		{
		  if(Send_count[recvTask] > 0 || Recv_count[recvTask] > 0)
		    {
		      /* send the results */
		      MPI_Sendrecv(&GravDataResult[Recv_offset[recvTask]],
				   Recv_count[recvTask] * sizeof(struct gravdata_out),
				   MPI_BYTE, recvTask, TAG_GRAV_B,
				   &GravDataOut[Send_offset[recvTask]],
				   Send_count[recvTask] * sizeof(struct gravdata_out),
				   MPI_BYTE, recvTask, TAG_GRAV_B, MPI_COMM_WORLD, &status);
		    }
		}

	    }
	  tend = second();
	  timecommsumm2 += timediff(tstart, tend);


	  /* add the results to the local particles */
	  tstart = second();
	  for(j = 0; j < Nexport; j++)
	    {
	      place = DataIndexTable[j].Index;

	      for(k = 0; k < 3; k++)
		P[place].g.dGravAccel[k] += GravDataOut[j].Acc[k];


#ifdef DISTORTIONTENSORPS
	      for(i1 = 0; i1 < 3; i1++)
		for(i2 = 0; i2 < 3; i2++)
		  P[place].tidal_tensorps[i1][i2] += GravDataOut[j].tidal_tensorps[i1][i2];
#endif

	      P[place].GravCost += GravDataOut[j].Ninteractions;
#ifdef EVALPOTENTIAL
	      P[place].p.dPotential += GravDataOut[j].Potential;
#endif
	    }
	  tend = second();
	  timetree1 += timediff(tstart, tend);

	  myfree(GravDataOut);
	  myfree(GravDataResult);
	  myfree(GravDataGet);
	}
      while(ndone < NTask);
    } /* Ewald_iter */

#ifdef SCF_HYBRID
  /* restore particle masses */
  for(i = 0; i < NumPart; i++)
    P[i].Mass=P[i].MassBackup;


  /* add up accelerations from tree to AccelSum */
  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
     /* ignore STAR<-DM contribution */
     if (scf_counter==1 && P[i].Type==2)
      { 
       continue;
      }       
     else 
      {
       for(j = 0; j < 3; j++)
	 P[i].GravAccelSum[j] += P[i].g.dGravAccel[j];
      }
    } 
  } /* scf_counter */

  /* set acceleration to summed up accelerations */
  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
   {
    for(j = 0; j < 3; j++)
     P[i].g.dGravAccel[j] = P[i].GravAccelSum[j];
   }  
#endif
    


#ifdef FLTROUNDOFFREDUCTION
  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
#ifdef EVALPOTENTIAL
      P[i].p.Potential = FLT(P[i].p.dPotential);
#endif
      for(j = 0; j < 3; j++)
	P[i].g.GravAccel[j] = FLT(P[i].g.dGravAccel[j]);
    }
#endif


  myfree(DataNodeList);
  myfree(DataIndexTable);

  /* now add things for comoving integration */

#ifndef PERIODIC
#ifndef PMGRID
  if(All.ComovingIntegrationOn)
    {
      double fac = 0.5 * All.Hubble * All.Hubble * All.Omega0 / All.G;

      for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
	for(j = 0; j < 3; j++)
	  P[i].g.GravAccel[j] += fac * P[i].Pos[j];
    }
#endif
#endif

  Costtotal = 0;

  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
      Costtotal += P[i].GravCost;

#ifdef PMGRID
      ax = P[i].g.GravAccel[0] + P[i].GravPM[0] / All.G;
      ay = P[i].g.GravAccel[1] + P[i].GravPM[1] / All.G;
      az = P[i].g.GravAccel[2] + P[i].GravPM[2] / All.G;
#else
      ax = P[i].g.GravAccel[0];
      ay = P[i].g.GravAccel[1];
      az = P[i].g.GravAccel[2];
#endif

      if(header.flag_ic_info == FLAG_SECOND_ORDER_ICS && All.Ti_Current == 0 && RestartFlag == 0)
	continue;		/* to prevent that we overwrite OldAcc in the first evaluation for 2lpt ICs */

      P[i].OldAcc = sqrt(ax * ax + ay * ay + az * az);
    }

  if(header.flag_ic_info == FLAG_SECOND_ORDER_ICS)
    {
      if(!(All.Ti_Current == 0 && RestartFlag == 0))
	if(All.TypeOfOpeningCriterion == 1)
	  All.ErrTolTheta = 0;	/* This will switch to the relative opening criterion for the following force computations */
    }
  else
    {
      if(All.TypeOfOpeningCriterion == 1)
	All.ErrTolTheta = 0;	/* This will switch to the relative opening criterion for the following force computations */
    }

  /*  muliply by G */
  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
      for(j = 0; j < 3; j++)
	P[i].g.GravAccel[j] *= All.G;


#ifdef DISTORTIONTENSORPS
      /*
         Diaganol terms of tidal tensor need correction, because tree is running over
         all particles -> also over target particle -> extra term -> correct it
       */
#ifdef RADIAL_TREE
      /* 1D -> only radial forces */
      MyDouble r2 = P[i].Pos[0] * P[i].Pos[0] + P[i].Pos[1] * P[i].Pos[1] + P[i].Pos[2] * P[i].Pos[2];

      P[i].tidal_tensorps[0][0] += P[i].Mass /
	(All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type]) *
	10.666666666667 * P[i].Pos[0] * P[i].Pos[0] / r2;;

      P[i].tidal_tensorps[0][1] += P[i].Mass /
	(All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type]) *
	10.666666666667 * P[i].Pos[0] * P[i].Pos[1] / r2;;

      P[i].tidal_tensorps[0][2] += P[i].Mass /
	(All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type]) *
	10.666666666667 * P[i].Pos[0] * P[i].Pos[2] / r2;;

      P[i].tidal_tensorps[1][0] += P[i].Mass /
	(All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type]) *
	10.666666666667 * P[i].Pos[1] * P[i].Pos[0] / r2;;

      P[i].tidal_tensorps[1][1] += P[i].Mass /
	(All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type]) *
	10.666666666667 * P[i].Pos[1] * P[i].Pos[1] / r2;;

      P[i].tidal_tensorps[1][2] += P[i].Mass /
	(All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type]) *
	10.666666666667 * P[i].Pos[1] * P[i].Pos[2] / r2;;

      P[i].tidal_tensorps[2][0] += P[i].Mass /
	(All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type]) *
	10.666666666667 * P[i].Pos[2] * P[i].Pos[0] / r2;;

      P[i].tidal_tensorps[2][1] += P[i].Mass /
	(All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type]) *
	10.666666666667 * P[i].Pos[2] * P[i].Pos[1] / r2;;

      P[i].tidal_tensorps[2][2] += P[i].Mass /
	(All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type]) *
	10.666666666667 * P[i].Pos[2] * P[i].Pos[2] / r2;;

#else
      /* 3D -> full forces */
      P[i].tidal_tensorps[0][0] +=
	P[i].Mass / (All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] *
		     All.ForceSoftening[P[i].Type]) * 10.666666666667;

      P[i].tidal_tensorps[1][1] +=
	P[i].Mass / (All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] *
		     All.ForceSoftening[P[i].Type]) * 10.666666666667;

      P[i].tidal_tensorps[2][2] +=
	P[i].Mass / (All.ForceSoftening[P[i].Type] * All.ForceSoftening[P[i].Type] *
		     All.ForceSoftening[P[i].Type]) * 10.666666666667;

#endif

#ifdef COMOVING_DISTORTION
      P[i].tidal_tensorps[0][0] +=
	4.0 * M_PI / 3.0 * (All.Omega0 * 3 * All.Hubble * All.Hubble / (8 * M_PI * All.G));
      P[i].tidal_tensorps[1][1] +=
	4.0 * M_PI / 3.0 * (All.Omega0 * 3 * All.Hubble * All.Hubble / (8 * M_PI * All.G));
      P[i].tidal_tensorps[2][2] +=
	4.0 * M_PI / 3.0 * (All.Omega0 * 3 * All.Hubble * All.Hubble / (8 * M_PI * All.G));
#endif

      /*now muliply by All.G */
      for(i1 = 0; i1 < 3; i1++)
	for(i2 = 0; i2 < 3; i2++)
	  P[i].tidal_tensorps[i1][i2] *= All.G;
#endif /* DISTORTIONTENSORPS */

#ifdef EVALPOTENTIAL
      /* remove self-potential */
      P[i].p.Potential += P[i].Mass / All.SofteningTable[P[i].Type];

      if(All.ComovingIntegrationOn)
	if(All.PeriodicBoundariesOn)
	  P[i].p.Potential -= 2.8372975 * pow(P[i].Mass, 2.0 / 3) *
	    pow(All.Omega0 * 3 * All.Hubble * All.Hubble / (8 * M_PI * All.G), 1.0 / 3);

      P[i].p.Potential *= All.G;

#ifdef PMGRID
      P[i].p.Potential += P[i].PM_Potential;	/* add in long-range potential */
#endif

      if(All.ComovingIntegrationOn)
	{
#ifndef PERIODIC
	  double fac, r2;

	  fac = -0.5 * All.Omega0 * All.Hubble * All.Hubble;

	  for(k = 0, r2 = 0; k < 3; k++)
	    r2 += P[i].Pos[k] * P[i].Pos[k];

	  P[i].p.Potential += fac * r2;
#endif
	}
      else
	{
	  double fac, r2;

	  fac = -0.5 * All.OmegaLambda * All.Hubble * All.Hubble;

	  if(fac != 0)
	    {
	      for(k = 0, r2 = 0; k < 3; k++)
		r2 += P[i].Pos[k] * P[i].Pos[k];

	      P[i].p.Potential += fac * r2;
	    }
	}
#endif
    }

  /* Finally, the following factor allows a computation of a cosmological simulation
     with vacuum energy in physical coordinates */
#ifndef PERIODIC
#ifndef PMGRID
  if(All.ComovingIntegrationOn == 0)
    {
      double fac = All.OmegaLambda * All.Hubble * All.Hubble;

      for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
	for(j = 0; j < 3; j++)
	  P[i].g.GravAccel[j] += fac * P[i].Pos[j];
    }
#endif
#endif


  if(ThisTask == 0)
    printf("tree is done.\n");

#else /* gravity is switched off */
  t0 = second();

  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    for(j = 0; j < 3; j++)
      P[i].g.GravAccel[j] = 0;


#ifdef DISTORTIONTENSORPS
  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
      P[i].tidal_tensorps[0][0] = 0.0;
      P[i].tidal_tensorps[0][1] = 0.0;
      P[i].tidal_tensorps[0][2] = 0.0;
      P[i].tidal_tensorps[1][0] = 0.0;
      P[i].tidal_tensorps[1][1] = 0.0;
      P[i].tidal_tensorps[1][2] = 0.0;
      P[i].tidal_tensorps[2][0] = 0.0;
      P[i].tidal_tensorps[2][1] = 0.0;
      P[i].tidal_tensorps[2][2] = 0.0;
    }
#endif
#endif /* end of NOGRAVITY */

#ifdef NOGRAVITY
  int k;
#endif




#ifdef SCFPOTENTIAL
  MyDouble xs, ys, zs;
  MyDouble pots, axs, ays, azs;

  if(ThisTask == 0)
    {
      printf("Starting SCF calculation...\n");
      fflush(stdout);
    }

  /* reset the expansion coefficients to zero */
  SCF_reset();
#ifdef SCF_HYBRID
  /* 
   calculate SCF coefficients for local DM particles.
   sum them up from all processors, so every processor
   sees the same expansion coefficients 
  */
  SCF_calc_from_particles();

  /* sum up local coefficients */
  MPI_Allreduce(sinsum, sinsum_all, (SCF_NMAX+1)*(SCF_LMAX+1)*(SCF_LMAX+1), MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(cossum, cossum_all, (SCF_NMAX+1)*(SCF_LMAX+1)*(SCF_LMAX+1), MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

  /* update local coefficients to global coefficients -> every processor has now complete SCF expansion */
  SCF_collect_update();  
  if(ThisTask == 0)
    {
      printf("calculated and collected coefficients.\n");
      fflush(stdout);
    }
  
#else  
  long old_seed, global_seed_min, global_seed_max;

  /* 
    resample coefficients for expansion 
    make sure that every processors sees the SAME potential, 
    i.e. has the same seed to generate coefficients  
  */
  old_seed=scf_seed;
  SCF_calc_from_random(&scf_seed);
  /* check that all cpus have the same random seed (min max must be the same) */
  MPI_Allreduce(&scf_seed, &global_seed_max, 1, MPI_LONG, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(&scf_seed, &global_seed_min, 1, MPI_LONG, MPI_MIN, MPI_COMM_WORLD);  
  if(ThisTask == 0)
    {
      printf("sampled coefficients with old/new seed = %ld/%ld         min/max=%ld/%ld\n", old_seed, scf_seed, global_seed_min, global_seed_max);
      fflush(stdout);
    }
#endif


  /* get accelerations for all active particles based on current expansion */
  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
      /* convert to unit sphere */
      to_unit(P[i].Pos[0], P[i].Pos[1], P[i].Pos[2], &xs, &ys, &zs) ;
      /* OR: not */
      //xs = P[i].Pos[0]; ys = P[i].Pos[1]; zs = P[i].Pos[2];
      
      /* evaluate potential and acceleration */
      SCF_evaluate(xs, ys, zs, &pots, &axs, &ays, &azs);      

      /* scale to system size and add to acceleration*/
#ifdef SCF_HYBRID
      /* 
       add missing STAR<-DM force from SCF (was excluded in tree above)
      */
      if (P[i].Type==2)  
       {
#endif
         /* scale */
        P[i].g.GravAccel[0] += All.G * SCF_HQ_MASS/(SCF_HQ_A*SCF_HQ_A) * axs;
        P[i].g.GravAccel[1] += All.G * SCF_HQ_MASS/(SCF_HQ_A*SCF_HQ_A) * ays;      
        P[i].g.GravAccel[2] += All.G * SCF_HQ_MASS/(SCF_HQ_A*SCF_HQ_A) * azs;            
	/* OR: not */
        //P[i].g.GravAccel[0] += All.G * axs;
        //P[i].g.GravAccel[1] += All.G * ays;      
        //P[i].g.GravAccel[2] += All.G * azs;            

#ifdef DEBUG
        if (P[i].ID==150000)
	 {
          printf("SCF-ACCEL (scf)   %d  (%g|%g|%g)\n", All.NumCurrentTiStep, All.G *  SCF_HQ_MASS/(SCF_HQ_A*SCF_HQ_A)*axs, All.G *  SCF_HQ_MASS/(SCF_HQ_A*SCF_HQ_A)*ays, All.G *  SCF_HQ_MASS/(SCF_HQ_A*SCF_HQ_A)*azs);
	  /* analyic potential of zeroth order of expansion */
          sphere_acc(xs, ys, zs, &axs, &ays, &azs);
          printf("SCF-ACCEL (exact) %d  (%g|%g|%g)\n", All.NumCurrentTiStep, All.G * axs, All.G * ays, All.G * azs);	  
	 } 
#endif

#ifdef SCF_HYBRID
       }
#endif	
    }

  if(ThisTask == 0)
    {
      printf("done.\n");
      fflush(stdout);
    }
#endif





#ifdef SHELL_CODE
  /* core softening */
  MyDouble hsoft, hsoft_tidal;

  /* cumul. masses from other CPUs */
  double *masslist;

  /* number of particles used to smooth out mass profile to get dM/dr */
  int ndiff = SHELL_CODE;

  if(ThisTask == 0)
    {
      printf("Starting shell code calculation...\n");
      fflush(stdout);
    }
#ifdef SIM_ADAPTIVE_SOFT
  double turnaround_radius_local = 0.0, turnaround_radius_global, v;
#endif

  /* set up data for sorting */
  rad_data = (struct radius_data *) mymalloc("rad_data", sizeof(struct radius_data) * NumPart);


  /* set up particle data */
  for(i = 0; i < NumPart; i++)
    {
      P[i].radius = sqrt(P[i].Pos[0] * P[i].Pos[0] + P[i].Pos[1] * P[i].Pos[1] + P[i].Pos[2] * P[i].Pos[2]);

      rad_data[i].radius = P[i].radius;
      rad_data[i].enclosed_mass = P[i].Mass;
      rad_data[i].GrNr = ThisTask;
      rad_data[i].SubNr = i;

#ifdef SIM_ADAPTIVE_SOFT
      v = (P[i].Pos[0] * P[i].Vel[0] + P[i].Pos[1] * P[i].Vel[1] + P[i].Pos[2] * P[i].Vel[2]);
      if((v < 0.0) && (P[i].radius > turnaround_radius_local))
	turnaround_radius_local = P[i].radius;
#endif
    }

#ifdef SIM_ADAPTIVE_SOFT
  /* find global turnaround radius by taking maximum of all CPUs */
  MPI_Allreduce(&turnaround_radius_local, &turnaround_radius_global, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

#ifdef ANALYTIC_TURNAROUND
#ifdef COMOVING_DISTORTION
  /* comoving turnaround radius */
  All.CurrentTurnaroundRadius =
    All.InitialTurnaroundRadius * pow(All.Time / All.TimeBegin, 1.0 / (3.0 * All.SIM_epsilon));
#else
  All.CurrentTurnaroundRadius =
    All.InitialTurnaroundRadius * pow(All.Time / All.TimeBegin,
				      2.0 / 3.0 + 2.0 / (3.0 * 3 * All.SIM_epsilon));
#endif
#else
  All.CurrentTurnaroundRadius = turnaround_radius_global;
#endif /* ANALYTIC_TURNAROUND */

  if(ThisTask == 0)
    {
#ifdef ANALYTIC_TURNAROUND
#ifdef COMOVING_DISTORTION
      printf("COMOVING_DISTORTION: comoving turnaround radius = %g\n", All.CurrentTurnaroundRadius);
#else
      printf("SIM/SHEL_CODE adaptive core softening: simulation turnaround radius = %g\n",
	     turnaround_radius_global);
      printf("SIM/SHEL_CODE adaptive core softening: analytic turnaround radius   = %g\n",
	     All.CurrentTurnaroundRadius);
#endif
#else
      printf("SIM/SHEL_CODE adaptive core softening: current turnaround radius  = %g\n",
	     All.CurrentTurnaroundRadius);
#endif /* ANALYTIC TURNAROUND */
      fflush(stdout);
    }

#endif /* SIM_ADAPTIVE_SOFT */

  parallel_sort(rad_data, NumPart, sizeof(struct radius_data), compare_radius);

  /* add up masses to get enclosed mass M(<r) */
  for(i = 1; i < NumPart; i++)
    rad_data[i].enclosed_mass = rad_data[i - 1].enclosed_mass + rad_data[i].enclosed_mass;

  /* get masses from other CPUs */
  masslist = (double *) mymalloc("masslist", NTask * sizeof(double));
  MPI_Allgather(&rad_data[NumPart - 1].enclosed_mass, 1, MPI_DOUBLE, masslist, 1, MPI_DOUBLE, MPI_COMM_WORLD);

  /* add results from other cpus */
  if(ThisTask > 0)
    {
      for(i = 0; i < NumPart; i++)
	{
	  for(k = 0; k < ThisTask; k++)
	    rad_data[i].enclosed_mass += masslist[k];
	}
    }

#ifdef COMOVING_DISTORTION
  /* subtract background mass */
  for(i = 0; i < NumPart; i++)
    rad_data[i].enclosed_mass -=
      All.Omega0 * 3.0 * All.Hubble * All.Hubble / (8.0 * M_PI * All.G) * (4.0 * M_PI / 3.0) *
      pow(rad_data[i].radius, 3.0);
#endif

  for(i = ndiff; i < NumPart - ndiff; i++)
    {
      /* simple finite difference estimate for derivative */
      rad_data[i].dMdr = (rad_data[i + ndiff].enclosed_mass - rad_data[i - ndiff].enclosed_mass) /
	(rad_data[i + ndiff].radius - rad_data[i - ndiff].radius);
    }

  /* set the remaining derivatives (quick&dirty solution that avoids CPU communication) */
  for(i = 0; i < ndiff; i++)
    rad_data[i].dMdr = rad_data[ndiff].dMdr;

  for(i = NumPart - ndiff; i < NumPart; i++)
    rad_data[i].dMdr = rad_data[NumPart - ndiff - 1].dMdr;


  /* sort back -> associate with particle data structure */
  parallel_sort(rad_data, NumPart, sizeof(struct radius_data), compare_GrNr_SubNr);

  /* write data into particle data */
  for(i = 0; i < NumPart; i++)
    {
      P[i].enclosed_mass = rad_data[i].enclosed_mass - P[i].Mass;
      P[i].dMdr = rad_data[i].dMdr;
    }

  /* get the core softening length */
#ifdef SIM_ADAPTIVE_SOFT
  /* adaptive softening */
  hsoft = All.SofteningHalo * All.CurrentTurnaroundRadius;
  hsoft_tidal = All.SofteningHalo * All.CurrentTurnaroundRadius;
#else
  /* fixed softening */
  hsoft = All.SofteningHalo;
  hsoft_tidal = All.SofteningHalo;
#endif

  /* set the table values, because it is used for the time stepping, softening table contains Plummer equivalent softening length */
  for(i = 0; i < 6; i++)
    All.SofteningTable[i] = hsoft;

  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
      if(P[i].radius != 0.0)
	{
	  /* radial forces on shell */
	  P[i].g.GravAccel[0] +=
	    -All.G * P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft * hsoft, 1.5) * P[i].Pos[0];
	  P[i].g.GravAccel[1] +=
	    -All.G * P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft * hsoft, 1.5) * P[i].Pos[1];
	  P[i].g.GravAccel[2] +=
	    -All.G * P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft * hsoft, 1.5) * P[i].Pos[2];

#ifdef DISTORTIONTENSORPS
	  /* tidal tensor */
	  P[i].tidal_tensorps[0][0] +=
	    All.G * (-P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal, 1.5) -
		     P[i].dMdr / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
				     1.5) * P[i].Pos[0] * P[i].Pos[0] / pow(P[i].radius * P[i].radius +
									    0.0 * hsoft_tidal * hsoft_tidal,
									    0.5) +
		     3 * P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
						  2.5) * P[i].Pos[0] * P[i].Pos[0]);
	  P[i].tidal_tensorps[0][1] +=
	    All.G * (-0.0 * P[i].enclosed_mass /
		     pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
			 1.5) + -P[i].dMdr / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
						 1.5) * P[i].Pos[0] * P[i].Pos[1] / pow(P[i].radius *
											P[i].radius +
											0.0 * hsoft_tidal *
											hsoft_tidal,
											0.5) +
		     3 * P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
						  2.5) * P[i].Pos[0] * P[i].Pos[1]);
	  P[i].tidal_tensorps[0][2] +=
	    All.G * (-0.0 * P[i].enclosed_mass /
		     pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
			 1.5) + -P[i].dMdr / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
						 1.5) * P[i].Pos[0] * P[i].Pos[2] / pow(P[i].radius *
											P[i].radius +
											0.0 * hsoft_tidal *
											hsoft_tidal,
											0.5) +
		     3 * P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
						  2.5) * P[i].Pos[0] * P[i].Pos[2]);
	  P[i].tidal_tensorps[1][1] +=
	    All.G * (-P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal, 1.5) +
		     -P[i].dMdr / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
				      1.5) * P[i].Pos[1] * P[i].Pos[1] / pow(P[i].radius * P[i].radius +
									     0.0 * hsoft_tidal * hsoft_tidal,
									     0.5) +
		     3 * P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
						  2.5) * P[i].Pos[1] * P[i].Pos[1]);
	  P[i].tidal_tensorps[1][2] +=
	    All.G * (-0.0 * P[i].enclosed_mass /
		     pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
			 1.5) + -P[i].dMdr / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
						 1.5) * P[i].Pos[1] * P[i].Pos[2] / pow(P[i].radius *
											P[i].radius +
											0.0 * hsoft_tidal *
											hsoft_tidal,
											0.5) +
		     3 * P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
						  2.5) * P[i].Pos[1] * P[i].Pos[2]);
	  P[i].tidal_tensorps[2][2] +=
	    All.G * (-P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal, 1.5) +
		     -P[i].dMdr / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
				      1.5) * P[i].Pos[2] * P[i].Pos[2] / pow(P[i].radius * P[i].radius +
									     0.0 * hsoft_tidal * hsoft_tidal,
									     0.5) +
		     3 * P[i].enclosed_mass / pow(P[i].radius * P[i].radius + hsoft_tidal * hsoft_tidal,
						  2.5) * P[i].Pos[2] * P[i].Pos[2]);
	  P[i].tidal_tensorps[1][0] = P[i].tidal_tensorps[0][1];
	  P[i].tidal_tensorps[2][0] = P[i].tidal_tensorps[0][2];
	  P[i].tidal_tensorps[2][1] = P[i].tidal_tensorps[1][2];
#endif
	}
    }

  /* free data */
  myfree(masslist);
  myfree(rad_data);

  if(ThisTask == 0)
    {
      printf("done with shell code calculation.\n");
      fflush(stdout);
    }

#endif /* SHELL_CODE */

#ifdef STATICNFW
  double r, m;
  int l;

  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
      r = sqrt(P[i].Pos[0] * P[i].Pos[0] + P[i].Pos[1] * P[i].Pos[1] + P[i].Pos[2] * P[i].Pos[2]);
      m = enclosed_mass(r);
#ifdef NFW_DARKFRACTION
      m *= NFW_DARKFRACTION;
#endif
      if(r > 0)
	{
	  for(l = 0; l < 3; l++)
	    P[i].g.GravAccel[l] += -All.G * m * P[i].Pos[l] / (r * r * r);

#ifdef DISTORTIONTENSORPS
	  double R200 = pow(NFW_M200 * All.G / (100 * All.Hubble * All.Hubble), 1.0 / 3);
	  double Rs = R200 / NFW_C;
	  double K = All.G * NFW_M200 / (Rs * (log(1 + NFW_C) - NFW_C / (1 + NFW_C)));
	  double r_red = r / Rs;
	  double x, y, z;

	  x = P[i].Pos[0];
	  y = P[i].Pos[1];
	  z = P[i].Pos[2];

	  P[i].tidal_tensorps[0][0] +=
	    -(-K * (1.0 / (r * (1 + r_red)) - log(1 + r_red) / (r * r_red)) * (1 / r - x * x / (r * r * r)) -
	      K * (-2.0 / (r * r * (1 + r_red)) - 1.0 / (r * (1 + r_red) * (1 + r_red) * Rs) +
		   2.0 * Rs * log(1 + r_red) / (r * r * r)) * x * x / (r * r));
	  P[i].tidal_tensorps[0][1] +=
	    -(-K * (1.0 / (r * (1 + r_red)) - log(1 + r_red) / (r * r_red)) * (0 - x * y / (r * r * r)) -
	      K * (-2.0 / (r * r * (1 + r_red)) - 1.0 / (r * (1 + r_red) * (1 + r_red) * Rs) +
		   2.0 * Rs * log(1 + r_red) / (r * r * r)) * x * y / (r * r));
	  P[i].tidal_tensorps[0][2] +=
	    -(-K * (1.0 / (r * (1 + r_red)) - log(1 + r_red) / (r * r_red)) * (0 - x * z / (r * r * r)) -
	      K * (-2.0 / (r * r * (1 + r_red)) - 1.0 / (r * (1 + r_red) * (1 + r_red) * Rs) +
		   2.0 * Rs * log(1 + r_red) / (r * r * r)) * x * z / (r * r));
	  P[i].tidal_tensorps[1][1] +=
	    -(-K * (1.0 / (r * (1 + r_red)) - log(1 + r_red) / (r * r_red)) * (1 / r - y * y / (r * r * r)) -
	      K * (-2.0 / (r * r * (1 + r_red)) - 1.0 / (r * (1 + r_red) * (1 + r_red) * Rs) +
		   2.0 * Rs * log(1 + r_red) / (r * r * r)) * y * y / (r * r));
	  P[i].tidal_tensorps[1][2] +=
	    -(-K * (1.0 / (r * (1 + r_red)) - log(1 + r_red) / (r * r_red)) * (0 - y * z / (r * r * r)) -
	      K * (-2.0 / (r * r * (1 + r_red)) - 1.0 / (r * (1 + r_red) * (1 + r_red) * Rs) +
		   2.0 * Rs * log(1 + r_red) / (r * r * r)) * y * z / (r * r));
	  P[i].tidal_tensorps[2][2] +=
	    -(-K * (1.0 / (r * (1 + r_red)) - log(1 + r_red) / (r * r_red)) * (1 / r - z * z / (r * r * r)) -
	      K * (-2.0 / (r * r * (1 + r_red)) - 1.0 / (r * (1 + r_red) * (1 + r_red) * Rs) +
		   2.0 * Rs * log(1 + r_red) / (r * r * r)) * z * z / (r * r));

	  P[i].tidal_tensorps[1][0] += P[i].tidal_tensorps[0][1];
	  P[i].tidal_tensorps[2][0] += P[i].tidal_tensorps[0][2];
	  P[i].tidal_tensorps[2][1] += P[i].tidal_tensorps[1][2];
#endif

	}
    }
#endif



#ifdef STATICPLUMMER
  int l;
  double r;


  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
      r = sqrt(P[i].Pos[0] * P[i].Pos[0] + P[i].Pos[1] * P[i].Pos[1] + P[i].Pos[2] * P[i].Pos[2]);

      for(l = 0; l < 3; l++)
	P[i].g.GravAccel[l] += -P[i].Pos[l] / pow(r * r + 1, 1.5);

#ifdef DISTORTIONTENSORPS
      double x, y, z, r2, f, f2;

      x = P[i].Pos[0];
      y = P[i].Pos[1];
      z = P[i].Pos[2];

      r2 = r * r;;
      f = pow(r2 + 1, 1.5);
      f2 = pow(r2 + 1, 2.5);


      P[i].tidal_tensorps[0][0] += -1.0 / f + 3.0 * x * x / f2;
      P[i].tidal_tensorps[0][1] += -0.0 / f + 3.0 * x * y / f2;
      P[i].tidal_tensorps[0][2] += -0.0 / f + 3.0 * x * z / f2;
      P[i].tidal_tensorps[1][1] += -1.0 / f + 3.0 * y * y / f2;
      P[i].tidal_tensorps[1][2] += -0.0 / f + 3.0 * y * z / f2;
      P[i].tidal_tensorps[2][2] += -1.0 / f + 3.0 * z * z / f2;
      P[i].tidal_tensorps[1][0] += P[i].tidal_tensorps[0][1];
      P[i].tidal_tensorps[2][0] += P[i].tidal_tensorps[0][2];
      P[i].tidal_tensorps[2][1] += P[i].tidal_tensorps[1][2];
#endif
    }
#endif



#ifdef STATICHQ
  double r, m, a;
  int l;


  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
      r = sqrt(P[i].Pos[0] * P[i].Pos[0] + P[i].Pos[1] * P[i].Pos[1] + P[i].Pos[2] * P[i].Pos[2]);

      a = pow(All.G * HQ_M200 / (100 * All.Hubble * All.Hubble), 1.0 / 3) / HQ_C *
	sqrt(2 * (log(1 + HQ_C) - HQ_C / (1 + HQ_C)));

      m = HQ_M200 * pow(r / (r + a), 2);
#ifdef HQ_DARKFRACTION
      m *= HQ_DARKFRACTION;
#endif
      if(r > 0)
	{
	  for(l = 0; l < 3; l++)
	    P[i].g.GravAccel[l] += -All.G * m * P[i].Pos[l] / (r * r * r);

#ifdef DISTORTIONTENSORPS
	  double x, y, z, r2, r3, f, f2, f3;

	  x = P[i].Pos[0];
	  y = P[i].Pos[1];
	  z = P[i].Pos[2];

	  r2 = r * r;
	  r3 = r * r2;
	  f = r + a;
	  f2 = f * f;
	  f3 = f2 * f;


	  P[i].tidal_tensorps[0][0] +=
	    All.G * (2.0 * HQ_M200 / (r2 * f3) * x * x + HQ_M200 / (r3 * f2) * x * x - HQ_M200 / (r * f2));
	  P[i].tidal_tensorps[0][1] +=
	    All.G * (2.0 * HQ_M200 / (r2 * f3) * x * y + HQ_M200 / (r3 * f2) * x * y);
	  P[i].tidal_tensorps[0][2] +=
	    All.G * (2.0 * HQ_M200 / (r2 * f3) * x * z + HQ_M200 / (r3 * f2) * x * z);
	  P[i].tidal_tensorps[1][1] +=
	    All.G * (2.0 * HQ_M200 / (r2 * f3) * y * y + HQ_M200 / (r3 * f2) * y * y - HQ_M200 / (r * f2));
	  P[i].tidal_tensorps[1][2] +=
	    All.G * (2.0 * HQ_M200 / (r2 * f3) * y * z + HQ_M200 / (r3 * f2) * y * z);
	  P[i].tidal_tensorps[2][2] +=
	    All.G * (2.0 * HQ_M200 / (r2 * f3) * z * z + HQ_M200 / (r3 * f2) * z * z - HQ_M200 / (r * f2));
	  P[i].tidal_tensorps[1][0] += P[i].tidal_tensorps[0][1];
	  P[i].tidal_tensorps[2][0] += P[i].tidal_tensorps[0][2];
	  P[i].tidal_tensorps[2][1] += P[i].tidal_tensorps[1][2];
#endif
	}
    }
#endif

#ifdef STATICLP
  double x, y, z, f;

  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
      x = P[i].Pos[0];
      y = P[i].Pos[1];
      z = P[i].Pos[2];
      f = LP_RC2 + x * x + y * y / LP_Q2 + z * z / LP_P2;
      if(f > 0)
	{
	  P[i].g.GravAccel[0] += -LP_V02 * x / f;
	  P[i].g.GravAccel[1] += -LP_V02 * y / (LP_Q2 * f);
	  P[i].g.GravAccel[2] += -LP_V02 * z / (LP_P2 * f);

#ifdef DISTORTIONTENSORPS
	  double f2;


	  f2 = f * f;

	  P[i].tidal_tensorps[0][0] += 2.0 * LP_V02 * x * x / f2 - LP_V02 / f;
	  P[i].tidal_tensorps[0][1] += 2.0 * LP_V02 * x * y / (LP_Q2 * f2);
	  P[i].tidal_tensorps[0][2] += 2.0 * LP_V02 * x * z / (LP_P2 * f2);
	  P[i].tidal_tensorps[1][1] += 2.0 * LP_V02 * y * y / (LP_Q2 * LP_Q2 * f2) - LP_V02 / (LP_Q2 * f);
	  P[i].tidal_tensorps[1][2] += 2.0 * LP_V02 * y * z / (LP_Q2 * LP_P2 * f2);
	  P[i].tidal_tensorps[2][2] += 2.0 * LP_V02 * z * z / (LP_P2 * LP_P2 * f2) - LP_V02 / (LP_P2 * f);
	  P[i].tidal_tensorps[1][0] += P[i].tidal_tensorps[0][1];
	  P[i].tidal_tensorps[2][0] += P[i].tidal_tensorps[0][2];
	  P[i].tidal_tensorps[2][1] += P[i].tidal_tensorps[1][2];

#endif
	}
    }
#endif

#ifdef STATICSM
  double x, y, z, r, r2;

  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
      x = P[i].Pos[0];
      y = P[i].Pos[1];
      z = P[i].Pos[2];
      r = sqrt(x * x + y * y + z * z);
      r2 = r * r;
      if(r > 0)
	{
	  P[i].g.GravAccel[0] += -SM_V02 / r2 * (1 - SM_a / r * atan(r / SM_a)) * x;
	  P[i].g.GravAccel[1] += -SM_V02 / r2 * (1 - SM_a / r * atan(r / SM_a)) * y;
	  P[i].g.GravAccel[2] += -SM_V02 / r2 * (1 - SM_a / r * atan(r / SM_a)) * z;


#ifdef DISTORTIONTENSORPS
	  double SM_a2 = SM_a * SM_a;


	  P[i].tidal_tensorps[0][0] += -SM_V02 / r2 * (1 - SM_a / r * atan(r / SM_a)) +
	    1.0 / (SM_a2 + r2) * SM_V02 / (r2 * r2) * (3 * SM_a2 + 2 * r2 -
						       3 * (SM_a2 + r2) * SM_a / r * atan(r / SM_a)) * x * x;
	  P[i].tidal_tensorps[0][1] +=
	    -0 + 1.0 / (SM_a2 + r2) * SM_V02 / (r2 * r2) * (3 * SM_a2 + 2 * r2 -
							    3 * (SM_a2 +
								 r2) * SM_a / r * atan(r / SM_a)) * x * y;
	  P[i].tidal_tensorps[0][2] +=
	    -0 + 1.0 / (SM_a2 + r2) * SM_V02 / (r2 * r2) * (3 * SM_a2 + 2 * r2 -
							    3 * (SM_a2 +
								 r2) * SM_a / r * atan(r / SM_a)) * x * z;
	  P[i].tidal_tensorps[1][1] +=
	    -SM_V02 / r2 * (1 - SM_a / r * atan(r / SM_a)) + 1.0 / (SM_a2 +
								    r2) * SM_V02 / (r2 * r2) * (3 * SM_a2 +
												2 * r2 -
												3 * (SM_a2 +
												     r2) *
												SM_a / r *
												atan(r /
												     SM_a)) *
	    y * y;
	  P[i].tidal_tensorps[1][2] +=
	    -0 + 1.0 / (SM_a2 + r2) * SM_V02 / (r2 * r2) * (3 * SM_a2 + 2 * r2 -
							    3 * (SM_a2 +
								 r2) * SM_a / r * atan(r / SM_a)) * y * z;
	  P[i].tidal_tensorps[2][2] +=
	    -SM_V02 / r2 * (1 - SM_a / r * atan(r / SM_a)) + 1.0 / (SM_a2 +
								    r2) * SM_V02 / (r2 * r2) * (3 * SM_a2 +
												2 * r2 -
												3 * (SM_a2 +
												     r2) *
												SM_a / r *
												atan(r /
												     SM_a)) *
	    z * z;
	  P[i].tidal_tensorps[1][0] += P[i].tidal_tensorps[0][1];
	  P[i].tidal_tensorps[2][0] += P[i].tidal_tensorps[0][2];
	  P[i].tidal_tensorps[2][1] += P[i].tidal_tensorps[1][2];

#endif
	}
    }
#endif

#ifdef STATICBRANDT
  double r, m;

  for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {

      r = sqrt((P[i].Pos[0] - 10.0) * (P[i].Pos[0] - 10.0) + (P[i].Pos[1] - 10.0) * (P[i].Pos[1] - 10.0));

      m = (r * r * r * BRANDT_OmegaBr * BRANDT_OmegaBr) / (1 + (r / BRANDT_R0) * (r / BRANDT_R0));

/* note there is no acceleration in z */

      if(r > 0)
	{
	  for(k = 0; k < 2; k++)
	    P[i].g.GravAccel[k] += -All.G * m * (P[i].Pos[k] - 10.0) / (r * r * r);
	}
    }

#endif



  /* Now the force computation is finished */

  t1 = WallclockTime = second();
  timeall += timediff(t0, t1);

  /*  gather some diagnostic information */

  timetree = timetree1 + timetree2;
  timewait = timewait1 + timewait2;
  timecomm = timecommsumm1 + timecommsumm2;

  MPI_Reduce(&timetree, &sumt, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(&timetree, &maxt, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
  MPI_Reduce(&timetree1, &sumt1, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(&timetree1, &maxt1, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
  MPI_Reduce(&timetree2, &sumt2, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(&timetree2, &maxt2, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
  MPI_Reduce(&timewait, &sumwaitall, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(&timecomm, &sumcommall, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(&Costtotal, &sum_costtotal, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(&Ewaldcount, &ewaldtot, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

  sumup_longs(1, &n_exported, &n_exported);
  sumup_longs(1, &N_nodesinlist, &N_nodesinlist);

  All.TotNumOfForces += GlobNumForceUpdate;

  plb = (NumPart / ((double) All.TotNumPart)) * NTask;
  MPI_Reduce(&plb, &plb_max, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
  MPI_Reduce(&Numnodestree, &maxnumnodes, 1, MPI_INT, MPI_MAX, 0, MPI_COMM_WORLD);

  CPU_Step[CPU_TREEMISC] += timeall - (timetree + timewait + timecomm);
  CPU_Step[CPU_TREEWALK1] += timetree1;
  CPU_Step[CPU_TREEWALK2] += timetree2;
  CPU_Step[CPU_TREESEND] += timecommsumm1;
  CPU_Step[CPU_TREERECV] += timecommsumm2;
  CPU_Step[CPU_TREEWAIT1] += timewait1;
  CPU_Step[CPU_TREEWAIT2] += timewait2;


#ifdef FIXEDTIMEINFIRSTPHASE
  MPI_Reduce(&min_time_first_phase, &min_time_first_phase_glob, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
  if(ThisTask == 0)
    {
      printf("FIXEDTIMEINFIRSTPHASE=%g  min_time_first_phase_glob=%g\n",
	     FIXEDTIMEINFIRSTPHASE, min_time_first_phase_glob);
    }
#endif

  if(ThisTask == 0)
    {
      fprintf(FdTimings, "Step= %d  t= %g  dt= %g \n", All.NumCurrentTiStep, All.Time, All.TimeStep);
      fprintf(FdTimings, "Nf= %d%09d  total-Nf= %d%09d  ex-frac= %g (%g) iter= %d\n",
	      (int) (GlobNumForceUpdate / 1000000000), (int) (GlobNumForceUpdate % 1000000000),
	      (int) (All.TotNumOfForces / 1000000000), (int) (All.TotNumOfForces % 1000000000),
	      n_exported / ((double) GlobNumForceUpdate), N_nodesinlist / ((double) n_exported + 1.0e-10),
	      iter);
      /* note: on Linux, the 8-byte integer could be printed with the format identifier "%qd", but doesn't work on AIX */

      fprintf(FdTimings, "work-load balance: %g (%g %g) rel1to2=%g   max=%g avg=%g\n",
	      maxt / (1.0e-6 + sumt / NTask), maxt1 / (1.0e-6 + sumt1 / NTask),
	      maxt2 / (1.0e-6 + sumt2 / NTask), sumt1 / (1.0e-6 + sumt1 + sumt2), maxt, sumt / NTask);
      fprintf(FdTimings, "particle-load balance: %g\n", plb_max);
      fprintf(FdTimings, "max. nodes: %d, filled: %g\n", maxnumnodes,
	      maxnumnodes / (All.TreeAllocFactor * All.MaxPart + NTopnodes));
      fprintf(FdTimings, "part/sec=%g | %g  ia/part=%g (%g)\n", GlobNumForceUpdate / (sumt + 1.0e-20),
	      GlobNumForceUpdate / (1.0e-6 + maxt * NTask),
	      ((double) (sum_costtotal)) / (1.0e-20 + GlobNumForceUpdate),
	      ((double) ewaldtot) / (1.0e-20 + GlobNumForceUpdate));
      fprintf(FdTimings, "\n");

      fflush(FdTimings);
    }

  CPU_Step[CPU_TREEMISC] += measure_time();
}




void *gravity_primary_loop(void *p)
{
  int i, j, ret;
  int thread_id = *(int *) p;

  int *exportflag, *exportnodecount, *exportindex;

  exportflag = Exportflag + thread_id * NTask;
  exportnodecount = Exportnodecount + thread_id * NTask;
  exportindex = Exportindex + thread_id * NTask;

  /* Note: exportflag is local to each thread */
  for(j = 0; j < NTask; j++)
    exportflag[j] = -1;

#ifdef FIXEDTIMEINFIRSTPHASE
  int counter = 0;
  double tstart;

  if(thread_id == 0)
    {
      tstart = second();
    }
#endif

  while(1)
    {
      LOCK_NEXPORT;

      if(BufferFullFlag != 0 || NextParticle < 0)
	{
	  UNLOCK_NEXPORT;
	  break;
	}

      i = NextParticle;
      ProcessedFlag[i] = 0;
      NextParticle = NextActiveParticle[NextParticle];
      UNLOCK_NEXPORT;

#if !defined(PMGRID)
#if defined(PERIODIC) && !defined(GRAVITY_NOT_PERIODIC)
      if(Ewald_iter)
	{
	  ret = force_treeevaluate_ewald_correction(i, 0, exportflag, exportnodecount, exportindex);
	  if(ret >= 0)
	    {
	      LOCK_WORKCOUNT;
	      Ewaldcount += ret;	/* note: ewaldcount may be slightly incorrect for multiple threads if buffer gets filled up */
	      UNLOCK_WORKCOUNT;
	    }
	  else
	    break;		/* export buffer has filled up */
	}
      else
#endif
	{
	  ret = force_treeevaluate(i, 0, exportflag, exportnodecount, exportindex);
	  if(ret < 0)
	    break;		/* export buffer has filled up */
	}
#else

#ifdef NEUTRINOS
      if(P[i].Type != 2)
#endif
	{
	  ret = force_treeevaluate_shortrange(i, 0, exportflag, exportnodecount, exportindex);
	  if(ret < 0)
	    break;		/* export buffer has filled up */
	}

#endif

      ProcessedFlag[i] = 1;	/* particle successfully finished */

#ifdef FIXEDTIMEINFIRSTPHASE
      if(thread_id == 0)
	{
	  counter++;
	  if((counter & 255) == 0)
	    {
	      if(timediff(tstart, second()) > FIXEDTIMEINFIRSTPHASE)
		{
		  TimerFlag = 1;
		  break;
		}
	    }
	}
      else
	{
	  if(TimerFlag)
	    break;
	}
#endif
    }

  return NULL;
}


void *gravity_secondary_loop(void *p)
{
  int j, nodesinlist, dummy;

  while(1)
    {
      LOCK_NEXPORT;
      j = NextJ;
      NextJ++;
      UNLOCK_NEXPORT;

      if(j >= Nimport)
	break;

#if !defined(PMGRID)
#if defined(PERIODIC) && !defined(GRAVITY_NOT_PERIODIC)
      if(Ewald_iter)
	{
	  int cost = force_treeevaluate_ewald_correction(j, 1, &dummy, &dummy, &dummy);

	  LOCK_WORKCOUNT;
	  Ewaldcount += cost;
	  UNLOCK_WORKCOUNT;
	}
      else
#endif
	{
	  force_treeevaluate(j, 1, &nodesinlist, &dummy, &dummy);
	  LOCK_WORKCOUNT;
	  N_nodesinlist += nodesinlist;
	  UNLOCK_WORKCOUNT;
	}
#else
      force_treeevaluate_shortrange(j, 1, &nodesinlist, &dummy, &dummy);
      LOCK_WORKCOUNT;
      N_nodesinlist += nodesinlist;
      UNLOCK_WORKCOUNT;
#endif
    }

  return NULL;
}






/*! This function sets the (comoving) softening length of all particle
 *  types in the table All.SofteningTable[...].  We check that the physical
 *  softening length is bounded by the Softening-MaxPhys values.
 */
void set_softenings(void)
{
  int i;

#if PHYS_COMOVING_SOFT
  double art_a;
#endif
  if(All.ComovingIntegrationOn)
    {
      if(All.SofteningGas * All.Time > All.SofteningGasMaxPhys)
	All.SofteningTable[0] = All.SofteningGasMaxPhys / All.Time;
      else
	All.SofteningTable[0] = All.SofteningGas;

      if(All.SofteningHalo * All.Time > All.SofteningHaloMaxPhys)
	All.SofteningTable[1] = All.SofteningHaloMaxPhys / All.Time;
      else
	All.SofteningTable[1] = All.SofteningHalo;

      if(All.SofteningDisk * All.Time > All.SofteningDiskMaxPhys)
	All.SofteningTable[2] = All.SofteningDiskMaxPhys / All.Time;
      else
	All.SofteningTable[2] = All.SofteningDisk;

      if(All.SofteningBulge * All.Time > All.SofteningBulgeMaxPhys)
	All.SofteningTable[3] = All.SofteningBulgeMaxPhys / All.Time;
      else
	All.SofteningTable[3] = All.SofteningBulge;

      if(All.SofteningStars * All.Time > All.SofteningStarsMaxPhys)
	All.SofteningTable[4] = All.SofteningStarsMaxPhys / All.Time;
      else
	All.SofteningTable[4] = All.SofteningStars;

      if(All.SofteningBndry * All.Time > All.SofteningBndryMaxPhys)
	All.SofteningTable[5] = All.SofteningBndryMaxPhys / All.Time;
      else
	All.SofteningTable[5] = All.SofteningBndry;
#ifdef SINKS
      All.SofteningTable[5] = All.SinkHsml / All.Time * All.HubbleParam;
#endif
    }
  else
    {
#if PHYS_COMOVING_SOFT
      art_a = pow(All.Time / All.TimeMax, 2.0 / 3.0);

      if(ThisTask == 0)
	printf("Test aart: %f SoftInPhys %f \n", art_a, All.SofteningGas * art_a);

      /* in the Initial Parameters one enters Softening* in Comoving Units and SofttenigMaxPhys is in Phys :P
       * and now we want all in Physical Units!*/
      if(All.SofteningGas * art_a > All.SofteningGasMaxPhys)
	All.SofteningTable[0] = All.SofteningGasMaxPhys;
      else
	All.SofteningTable[0] = All.SofteningGas * art_a;

      if(All.SofteningHalo * art_a > All.SofteningHaloMaxPhys)
	All.SofteningTable[1] = All.SofteningHaloMaxPhys;
      else
	All.SofteningTable[1] = All.SofteningHalo * art_a;

      if(All.SofteningDisk * art_a > All.SofteningDiskMaxPhys)
	All.SofteningTable[2] = All.SofteningDiskMaxPhys;
      else
	All.SofteningTable[2] = All.SofteningDisk * art_a;

      if(All.SofteningBulge * art_a > All.SofteningBulgeMaxPhys)
	All.SofteningTable[3] = All.SofteningBulgeMaxPhys;
      else
	All.SofteningTable[3] = All.SofteningBulge * art_a;

      if(All.SofteningStars * art_a > All.SofteningStarsMaxPhys)
	All.SofteningTable[4] = All.SofteningStarsMaxPhys;
      else
	All.SofteningTable[4] = All.SofteningStars * art_a;

      if(All.SofteningBndry * art_a > All.SofteningBndryMaxPhys)
	All.SofteningTable[5] = All.SofteningBndryMaxPhys;
      else
	All.SofteningTable[5] = All.SofteningBndry * art_a;
#else
      All.SofteningTable[0] = All.SofteningGas;
      All.SofteningTable[1] = All.SofteningHalo;
      All.SofteningTable[2] = All.SofteningDisk;
      All.SofteningTable[3] = All.SofteningBulge;
      All.SofteningTable[4] = All.SofteningStars;
      All.SofteningTable[5] = All.SofteningBndry;
#ifdef SINKS
      All.SofteningTable[5] = All.SinkHsml;
#endif
#endif
    }

  for(i = 0; i < 6; i++)
    All.ForceSoftening[i] = 2.8 * All.SofteningTable[i];

  All.MinGasHsml = All.MinGasHsmlFractional * All.ForceSoftening[0];
}


/*! This function is used as a comparison kernel in a sort routine. It is
 *  used to group particles in the communication buffer that are going to
 *  be sent to the same CPU.
    */
int data_index_compare(const void *a, const void *b)
{
  if(((struct data_index *) a)->Task < (((struct data_index *) b)->Task))
    return -1;

  if(((struct data_index *) a)->Task > (((struct data_index *) b)->Task))
    return +1;

  if(((struct data_index *) a)->Index < (((struct data_index *) b)->Index))
    return -1;

  if(((struct data_index *) a)->Index > (((struct data_index *) b)->Index))
    return +1;

  if(((struct data_index *) a)->IndexGet < (((struct data_index *) b)->IndexGet))
    return -1;

  if(((struct data_index *) a)->IndexGet > (((struct data_index *) b)->IndexGet))
    return +1;

  return 0;
}

static void msort_dataindex_with_tmp(struct data_index *b, size_t n, struct data_index *t)
{
  struct data_index *tmp;
  struct data_index *b1, *b2;
  size_t n1, n2;

  if(n <= 1)
    return;

  n1 = n / 2;
  n2 = n - n1;
  b1 = b;
  b2 = b + n1;

  msort_dataindex_with_tmp(b1, n1, t);
  msort_dataindex_with_tmp(b2, n2, t);

  tmp = t;

  while(n1 > 0 && n2 > 0)
    {
      if(b1->Task < b2->Task || (b1->Task == b2->Task && b1->Index <= b2->Index))
	{
	  --n1;
	  *tmp++ = *b1++;
	}
      else
	{
	  --n2;
	  *tmp++ = *b2++;
	}
    }

  if(n1 > 0)
    memcpy(tmp, b1, n1 * sizeof(struct data_index));

  memcpy(b, t, (n - n2) * sizeof(struct data_index));
}

void mysort_dataindex(void *b, size_t n, size_t s, int (*cmp) (const void *, const void *))
{
  const size_t size = n * s;

  struct data_index *tmp = (struct data_index *) mymalloc("struct data_index *tmp", size);

  msort_dataindex_with_tmp((struct data_index *) b, n, tmp);

  myfree(tmp);
}
