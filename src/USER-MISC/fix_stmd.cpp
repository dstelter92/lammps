/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.

   Notes:
    The stmd.f::stmdcntrl() subroutine is translated in the FixSTMD constructor and init().
    The stmd.f::stmdinitrans() subroutine is translated in init().
    The stmd.f::stmdinitoutu() subroutine can be added to the bottom of init() if desired.
    If stmd.f::stmdAssignVel() is just velocity-rescaling, then this is not needed and a LAMMPS thermostat fix can be used.
    The stmd.f::stmddig() subroutine is translated to dig().
    ...
    The stmd.f::stmdMain() subroutine is translated to Main().

    The Verlet::run() function shows the flow of calculation at each
    MD step. The post_force() function is the current location for the
    STMD update via Main() and subsequent scaling of forces.

    In several spots, ".eq." was used in the Fortran code for testing reals. That is 
    repeated here with "==", but probably should be testing similarity against some tolerance.

    Many of the smaller subroutines/functions could probably be inlined.
    
------------------------------------------------------------------------- */

#include "string.h"
#include "stdlib.h"
#include "fix_stmd.h"
#include "atom.h"
#include "update.h"
#include "modify.h"
#include "domain.h"
#include "region.h"
#include "respa.h"
#include "input.h"
#include "variable.h"
#include "memory.h"
#include "error.h"
#include "force.h"
#include "comm.h"
#include "group.h"
#include "compute.h"
#include "output.h"
#include "universe.h"
#include <fstream>


using namespace LAMMPS_NS;
using namespace FixConst;

enum{NONE,CONSTANT,EQUAL,ATOM};

#define INVOKED_SCALAR 1

/* ---------------------------------------------------------------------- */

FixStmd::FixStmd(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg)
{
  if (narg < 16 || narg > 17) error->all(FLERR,"Illegal fix stmd command");

  scalar_flag = 1;
  vector_flag = 1;
  array_flag = 1;

  extscalar = 0;
  extvector = 0;
  extarray = 0;
  global_freq = 1;
  restart_file = 1;
 
  // This is the subset of variables explicitly given in the charmm.inp file
  // If the full set is expected to be modified by a user, then reading 
  // a stmd.inp file is probably the best mechanism for input.
  //
  // fix fxstmd all stmd RSTFRQ f_style init_f final_f Tlo Thi Elo Ehi binsize TSC1 TSC2 ST OREST

  // Probably a good idea to set this equal to restart value in input
  RSTFRQ = atoi(arg[3]);      

  // Setup type of f-reduction scheme
  f_flag = -1;
  if (strcmp(arg[4],"none") == 0)
    f_flag = 0;
  else if (strcmp(arg[4],"hchk") == 0) 
    f_flag = 1;
  else if (strcmp(arg[4],"sqrt") == 0)
    f_flag = 2;
  else if (strcmp(arg[4],"constant_f") == 0)
    f_flag = 3;
  else if (strcmp(arg[4],"constant_df") == 0)
    f_flag = 4;
  else
    error->all(FLERR,"STMD: invalid f-reduction scheme");
  if (f_flag == -1)
    error->all(FLERR,"STMD: invalid f-reduction scheme");
  
  // Only used initially, controlled by restart
  initf  = atof(arg[5]); // Delta F, init
  if (initf > 1.) 
    error->all(FLERR,"STMD: initial deltaF value too large");
  
  // Delta-f tolerances
  //dFval3 = 0.00010;
  dFval3 = atof(arg[6]); // Delta F, final
  if (dFval3 < 0.00001)
    error->all(FLERR,"STMD: final deltaF value too small");
  dFval4 = dFval3 / 10.;
  pfinFval = exp(dFval3 * 2 * bin);
  finFval = exp(dFval4 * 2 * bin);

  // Used once, controlled by restart file
  TL     = atof(arg[7]);
  TH     = atof(arg[8]);

  // Controlled by input file, not restart file
  Emin   = atof(arg[9]);
  Emax   = atof(arg[10]);
  bin    = atof(arg[11]);
  TSC1   = atoi(arg[12]);
  TSC2   = atoi(arg[13]);

  // This value should be consistent with target temperature of thermostat fix
  ST     = atof(arg[14]);
  
  // 0 for new run, 1 for restart
  OREST = -1;
  if (strcmp(arg[15],"yes") == 0)
    OREST = 1;
  else if (strcmp(arg[15],"no") == 0)
    OREST = 0;
  else 
    error->all(FLERR,"STMD: invalid restart option");
  if (OREST == -1)
    error->all(FLERR,"STMD: invalid restart option");

  // Make dir_output default to local dir
  if (narg == 17)
    strcpy(dir_output,arg[16]);
  else
    strcpy(dir_output,"./");

  
  Y1 = Y2 = Prob = NULL;
  Hist = Htot = PROH = NULL;

  stmd_logfile = stmd_debug = 0;
  if ((comm->me == 0) && (logfile)) stmd_logfile = 1;

  // DEBUG FLAG
  //stmd_debug = 1;

  fp_wtnm = fp_whnm = fp_whpnm = fp_orest = NULL;
  
  // Energy bin setup
  BinMin = round(Emin / bin);
  BinMax = round(Emax / bin);
  N = BinMax - BinMin + 1;
  
  size_vector = 8;
  size_array_cols = 4;
  size_array_rows = N;

}

/* ---------------------------------------------------------------------- */

FixStmd::~FixStmd()
{
  memory->destroy(Y1);
  memory->destroy(Y2);
  memory->destroy(Hist);
  memory->destroy(Htot);
  memory->destroy(PROH);
  memory->destroy(Prob);
}

/* ---------------------------------------------------------------------- */

int FixStmd::setmask()
{
  int mask = 0;
  mask |= POST_FORCE;
  mask |= MIN_POST_FORCE;
  mask |= END_OF_STEP;
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixStmd::init()
{
  // Get number of replicas (worlds) and walker number
  nworlds = universe->nworlds;
  iworld = universe->iworld;
  char walker[256]; 
  sprintf(walker,"%i",iworld);

  if (comm->me == 0) {
    char filename[256];
    if (!fp_wtnm) {
      strcpy(filename,dir_output);
      strcat(filename,"/WT.");
      strcat(filename,walker);
      strcat(filename,".d");
      strcpy(filename_wtnm,filename);
      fp_wtnm  = fopen(filename,"w");
    }
    if (!fp_whnm) {
      strcpy(filename,dir_output);
      strcat(filename,"/WH.");
      strcat(filename,walker);
      strcat(filename,".d");
      strcpy(filename_whnm,filename);
      fp_whnm  = fopen(filename,"w");
    }
    if (!fp_whpnm) {
      strcpy(filename,dir_output);
      strcat(filename,"/WHP.");
      strcat(filename,walker);
      strcat(filename,".d");
      strcpy(filename_whpnm,filename);
      fp_whpnm = fopen(filename,"w");
    }
    if ((!fp_orest) && (!OREST)) {
      strcpy(filename,dir_output);
      strcat(filename,"/oREST.");
      strcat(filename,walker);
      strcat(filename,".d");
      strcpy(filename_orest,filename);
      fp_orest = fopen(filename,"w");
    }
    if ((!fp_orest) && (OREST)) {
      strcpy(filename,dir_output);
      strcat(filename,"/oREST.");
      strcat(filename,walker);
      strcat(filename,".d");
      strcpy(filename_orest,filename);

      // Check if file exists
      fp_orest = fopen(filename,"r");
      if (!fp_orest) {
        if (stmd_logfile) {
          fprintf(screen,"Restart file: oREST.%s.d is empty\n",walker);
          fprintf(logfile,"Restart file: oREST.%s.d is empty\n",walker);
        }
        error->one(FLERR,"STMD: Restart file does not exist\n");
      }
    }
  }

  CutTmin  = 50.0;
  CutTmax  = 50.0;
  HCKtol   = 0.2;

  STG     = 1;
  SWf     = 1;
  SWfold  = 1;
  Gamma   = 1.0;
  Count   = 0;
  CountH  = 0;
  totC    = 0;
  totCi   = 0;
  SWchk   = 1;
  CountPH = 0;
  T = ST; // latest T_s at bin i

  /*
  // Delta-f tolerances
  dFval3 = 0.0001;
  dFval4 = 0.000002;
  */
  pfinFval = exp(dFval3 * 2 * bin);
  finFval = exp(dFval4 * 2 * bin);

  f = exp(initf * 2 * bin);
  df = log(f) * 0.5 / bin;

  T0 = ST;
  T1 = TL / ST;
  T2 = TH / ST;
  CTmin = (TL + CutTmin) / ST;
  CTmax = (TH - CutTmax) / ST;

  memory->grow(Y1, N, "FixSTMD:Y1");
  memory->grow(Y2, N, "FixSTMD:Y2");
  memory->grow(Hist, N, "FixSTMD:Hist");
  memory->grow(Htot, N, "FixSTMD:Htot");
  memory->grow(PROH, N, "FixSTMD:PROH");
  memory->grow(Prob, N, "FixSTMD:Prob");

  for (int i=0; i<N; i++) {
    Y1[i] = T2;
    Y2[i] = T2;
    Hist[i] = 0;
    Htot[i] = 0;
    PROH[i] = 0;
    Prob[i] = 0.0;
  }

  // Search for pe compute, otherwise create a new one
  pe_compute_id = -1;
  for (int i=0; i<modify->ncompute; i++) {
    if (strcmp(modify->compute[i]->style,"pe") == 0) {
      pe_compute_id = i;
      break;
    }
  }

  // Pretty sure a pe compute is always present, but check anyways.
  // Did we find a pe compute? If not, then create one.
  if (pe_compute_id < 0) {
    int n = strlen(id) + 4;
    id_pe = new char[n];
    strcpy(id_pe,id);
    strcat(id_pe,"_pe");

    char **newarg = new char*[3];
    newarg[0] = id_pe;
    newarg[1] = group->names[igroup];
    newarg[2] = (char *) "pe";

    modify->add_compute(3,newarg);
    delete [] newarg;

    pe_compute_id = modify->ncompute - 1;
  }
  
  if (OREST) { // Read oREST.d into variables
    if (comm->me == 0) {
      int k = 0;
      int numb = 13;
      int nsize = 3*N + numb;
      double *list;
      memory->create(list,nsize,"stmd:list");

      char filename[256];
      strcpy(filename,dir_output);
      strcat(filename,"/oREST.");
      strcat(filename,walker);
      strcat(filename,".d");
      std::ifstream file(filename);

      // Check if file empty before getting data
      file.seekg(0,file.end);
      int sz = file.tellg();
      file.seekg(0,file.beg);
      if (sz < (nsize*sizeof(double))) {
        if (stmd_logfile) {
          fprintf(screen,"Restart file: oREST.%s.d is an invalid format\n",walker);
          fprintf(logfile,"Restart file: oREST.%s.d is an invalid format\n",walker);
        }
        error->one(FLERR,"STMD: Restart file is empty/invalid\n");
      }

      for (int i=0; i<nsize; i++) 
        file >> list[i];

      STG = static_cast<int> (list[k++]);
      f = list[k++];
      CountH = static_cast<int> (list[k++]);
      SWf = static_cast<int> (list[k++]);
      SWfold = static_cast<int> (list[k++]);
      SWchk = static_cast<int> (list[k++]);
      Count = static_cast<int> (list[k++]);
      totCi = static_cast<int> (list[k++]);
      CountPH = static_cast<int> (list[k++]);
      //TSC1 = static_cast<int> (list[k++]);
      //TSC2 = static_cast<int> (list[k++]);
      T1 = list[k++];
      T2 = list[k++];
      CTmin = list[k++];
      CTmax = list[k++];

      for (int i=0; i<N; i++)
        Y2[i] = list[k++];
      for (int i=0; i<N; i++)
        Htot[i] = list[k++];
      for (int i=0; i<N; i++)
        PROH[i] = list[k++];

      memory->destroy(list);
    }
    df = log(f) * 0.5 / bin;
  }

  if ((stmd_logfile) && (nworlds > 1)) {
    fprintf(logfile,"RESTMD: #replicas= %i  walker= %i\n",nworlds,iworld);
    fprintf(screen,"RESTMD: #replicas= %i  walker= %i\n",nworlds,iworld);
    }
  if (stmd_logfile) {
    fprintf(logfile,"STMD: STAGE= %i, #bins= %i  binsize= %f\n",STG,N,bin); 
    fprintf(screen,"STMD: STAGE= %i, #bins= %i  binsize= %f\n",STG,N,bin);
    fprintf(logfile,"  Emin= %f Emax= %f f-value= %f df= %f\n",Emin,Emax,f,df); 
    fprintf(screen,"  Emin= %f Emax= %f f-value= %f df= %f\n",Emin,Emax,f,df); 
    fprintf(logfile,"  f-tolerances: STG3= %f STG4= %f\n",pfinFval,finFval);
    fprintf(screen,"  f-tolerances: STG3= %f STG4= %f\n",pfinFval,finFval);
  }

  // Write values of all paramters to logfile
  if ((stmd_logfile) && (stmd_debug)) {
    //fprintf(logfile,"STMD Yold(Y1)= ");
    //for (int i=0; i<N; i++) fprintf(logfile," %f",Y1[i]);
    //fprintf(logfile,"\n");
    fprintf(logfile,"STMD Temperature (Y2)= ");
    for (int i=0; i<N; i++) 
      fprintf(logfile," %f",Y2[i]);
    fprintf(logfile,"\n");
  }
}

/* ---------------------------------------------------------------------- */

void FixStmd::setup(int vflag)
{
  if (strstr(update->integrate_style,"verlet")) {
    post_force(vflag);

    // Force computation of energies
    modify->compute[pe_compute_id]->invoked_flag |= INVOKED_SCALAR;
    modify->addstep_compute(update->ntimestep + 1);
  } else
    error->all(FLERR,"Currently expecting run_style verlet");
}

/* ---------------------------------------------------------------------- */

void FixStmd::min_setup(int vflag)
{
  post_force(vflag);
}

/* ---------------------------------------------------------------------- */

void FixStmd::post_force(int vflag)
{
  double **f = atom->f;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  // Get current value of potential energy from compute/pe
  double potE = modify->compute[pe_compute_id]->compute_scalar();

  // Check if potE is outside of bounds before continuing
  if ((potE < Emin) || (potE > Emax))
    error->all(FLERR,"STMD: Sampled potential energy out of range\n");

  // Master rank will compute scaling factor and then Bcast to world
  MAIN(update->ntimestep,potE);

  // Gamma(U) = T_0 / T(U)
  MPI_Bcast(&Gamma, 1, MPI_DOUBLE, 0, world); 

  // Scale forces
  for (int i = 0; i < nlocal; i++)
    if (mask[i] & groupbit) {
      f[i][0]*= Gamma;
      f[i][1]*= Gamma;
      f[i][2]*= Gamma;
    }
}

/* ---------------------------------------------------------------------- */

void FixStmd::end_of_step()
{
  // Force computation of energies on next step
  modify->compute[pe_compute_id]->invoked_flag |= INVOKED_SCALAR;
  modify->addstep_compute(update->ntimestep + 1);

  // If stmd, write output, otherwise let temper/stmd handle it
  if (universe->nworlds == 1) {
    write_temperature();
    write_orest();
  }
}

/* ---------------------------------------------------------------------- */

void FixStmd::min_post_force(int vflag)
{
  post_force(vflag);
}

/* ----------------------------------------------------------------------
   memory usage of local atom-based array
------------------------------------------------------------------------- */

double FixStmd::memory_usage()
{
  double bytes = 0.0;
  bytes+= 7 * N * sizeof(double);
  return bytes;
}

/* ----------------------------------------------------------------------
   write temperature to external file
------------------------------------------------------------------------- */

void FixStmd::write_temperature()
{
  int m = (update->ntimestep) % RSTFRQ;
  if ((m == 0) && (comm->me == 0)) {
    for (int i=0; i<N; i++) 
      fprintf(fp_wtnm,"%i %f %f %f %i\n", i,(i*bin)+Emin,Y2[i]*ST,Y2[i],totCi);
    fprintf(fp_wtnm,"\n\n");
  }
}

/* ----------------------------------------------------------------------
   write external restart file
------------------------------------------------------------------------- */

void FixStmd::write_orest()
{
  // Write restart info to external file
  int m = (update->ntimestep) % RSTFRQ;
  if ((m == 0) && (comm->me == 0)) {
    int k = 0;
    int numb = 13;
    int nsize = 3*N + numb;
    double *list;
    memory->create(list,nsize,"stmd:list");

    list[k++] = STG;
    list[k++] = f;
    list[k++] = CountH;
    list[k++] = SWf;
    list[k++] = SWfold;
    list[k++] = SWchk;
    list[k++] = Count;
    list[k++] = totCi;
    list[k++] = CountPH;

    // Control these by LAMMPS input file
    //list[k++] = TSC1; 
    //list[k++] = TSC2;

    list[k++] = T1;
    list[k++] = T2;

    // only used for chk_hist
    list[k++] = CTmin;
    list[k++] = CTmax;

    for (int i=0; i<N; i++) 
      list[k++] = Y2[i];
    for (int i=0; i<N; i++) 
      list[k++] = Htot[i];
    for (int i=0; i<N; i++) 
      list[k++] = PROH[i];

    // wipe file contents...
    char filename[256];
    char walker[256];
    sprintf(walker,"%i",universe->iworld);
    strcpy(filename,dir_output);
    strcat(filename,"/oREST.");
    strcat(filename,walker);
    strcat(filename,".d");
    freopen(filename,"w",fp_orest);

    if (fp_orest == NULL) 
      error->all(FLERR,"Cannot open STMD restart file");

    if (stmd_debug && stmd_logfile) {
      fprintf(screen,"%d\n",STG);
      fprintf(screen,"%f\n",f);
      fprintf(logfile,"%d\n",STG);
      fprintf(logfile,"%f\n",f);
    }
    fprintf(fp_orest,"%d\n",STG);
    fprintf(fp_orest,"%f\n",f);
    for (int i=2; i<numb; i++) {
      if (stmd_debug && stmd_logfile) {
        fprintf(screen,"%f\n",list[i]);
        fprintf(logfile,"%f\n",list[i]);
      }
      fprintf(fp_orest,"%f\n",list[i]);
    }
    for (int i=numb; i<N+numb; i++) {
      if (stmd_debug && stmd_logfile) {
        fprintf(screen,"%f ",list[i]);
        fprintf(logfile,"%f ",list[i]);
      }
      fprintf(fp_orest,"%f ",list[i]);
    }
    if (stmd_debug && stmd_logfile) {
      fprintf(screen,"\n");
      fprintf(logfile,"\n");
    }
    fprintf(fp_orest,"\n");
    for (int i=N+numb; i<(2*N)+numb; i++) {
      if (stmd_debug && stmd_logfile) {
        fprintf(screen,"%f ",list[i]);
        fprintf(logfile,"%f ",list[i]);
      }
      fprintf(fp_orest,"%f ",list[i]);
    }
    if (stmd_debug && stmd_logfile) {
      fprintf(screen,"\n");
      fprintf(logfile,"\n");
    }
    fprintf(fp_orest,"\n");
    for (int i=(2*N)+numb; i<nsize; i++) {
      if (stmd_debug && stmd_logfile) {
        fprintf(screen,"%f ",list[i]);
        fprintf(logfile,"%f ",list[i]);
      }
      fprintf(fp_orest,"%f ",list[i]);
    }
    if (stmd_debug && stmd_logfile) {
      fprintf(screen,"\n");
      fprintf(logfile,"\n");
    }
    fprintf(fp_orest,"\n");

    memory->destroy(list);
  }
}

/* ----------------------------------------------------------------------
   Translation of stmd.f subroutines
------------------------------------------------------------------------- */

void FixStmd::dig()
{
  int nkeepmin = 0;
  double keepmin = Y2[nkeepmin];

  for (int i=0; i<N; i++) {
    if (Y2[i] <= keepmin) {
      keepmin = Y2[i];
      nkeepmin = i;
    }
  }

  for (int i=0; i<nkeepmin; i++) 
    Y2[i] = keepmin;
}

/* ---------------------------------------------------------------------- */

int FixStmd::Yval(double potE)
{
  curbin = round(potE / double(bin)) - BinMin + 1;
  int i = curbin;

  if ((i<1) || (i>N-1)) {
    if ((stmd_logfile) && (comm->me == 0)) {
      fprintf(screen,"Error in Yval: potE= %f  bin= %f  i= %i\n",potE,bin,i);
      fprintf(logfile,"Error in Yval: potE= %f  bin= %f  i= %i\n",potE,bin,i);
    }
    error->all(FLERR,"STMD: Histogram index out of range");
  }

  double Yhi = Y2[i+1];
  double Ylo = Y2[i-1];

  Y2[i+1] = Y2[i+1] / (1.0 - df * Y2[i+1]);
  Y2[i-1] = Y2[i-1] / (1.0 + df * Y2[i-1]);

  if (stmd_debug && stmd_logfile) {
    fprintf(screen,"  STMD T-UPDATE: potE= %f  sampledbin= %i  df=%f\n",potE,i,df);
    fprintf(logfile,"  STMD T-UPDATE: potE= %f  sampledbin= %i  df=%f\n",potE,i,df);
    fprintf(screen,"    bin %d+1: T'= %f  T=%f  delta= %f\n",i,Y2[i+1],Yhi,Y2[i+1]-Yhi);
    fprintf(logfile,"    bin %d+1: T'= %f  T=%f  delta= %f\n",i,Y2[i+1],Yhi,Y2[i+1]-Yhi);
    fprintf(screen,"    bin %d-1: T'=%f  T=%f  delta= %f\n",i,Y2[i-1],Ylo,Y2[i-1]-Ylo);
    fprintf(logfile,"    bin %d-1: T'=%f  T=%f  delta= %f\n",i,Y2[i-1],Ylo,Y2[i-1]-Ylo);
  }

  if (Y2[i-1] < T1) 
    Y2[i-1] = T1;
  if (Y2[i+1] > T2) 
    Y2[i+1] = T2;

  return i;
}

/* ---------------------------------------------------------------------- */

void FixStmd::GammaE(double potE, int indx)
{
  const int i  = indx;
  const int im = indx - 1;
  const int ip = indx + 1;

  const double e = potE - double( round(potE / double(bin)) * bin );

  //double T;  // made this public to share with RESTMD
  if (e > 0.0) {
    const double lam = (Y2[ip] - Y2[i]) / double(bin);
    T = Y2[i] + lam * e;
  } else if (e < 0.0) {
    const double lam = (Y2[i] - Y2[im]) / double(bin);
    T = Y2[i] + lam * e;
  } else T = Y2[i];

  Gamma = 1.0 / T;
}

/* ---------------------------------------------------------------------- */

void FixStmd::AddedEHis(int i)
{
  Hist[i] = Hist[i] + 1;
  Htot[i] = Htot[i] + 1;
}

/* ---------------------------------------------------------------------- */

void FixStmd::EPROB(int icycle)
{
  int sw, m;
  const int indx = icycle;
  m = indx % TSC1;
  if ((m == 0) && (indx != 0)) sw = 1;

  m = indx % TSC2;
  if ((m == 0) && (indx != 0)) sw = 2;

  if (sw == 1) 
    for (int i=0; i<N; i++) 
      Prob[i] = Prob[i] / double(TSC1);
  else if (sw == 2)
    for (int i=0; i<N; i++) 
      Prob[i] = Prob[i] / double(TSC2);
  sw = 0;
}

/* ---------------------------------------------------------------------- */

void FixStmd::ResetPH()
{
  for (int i=0; i<N; i++) Hist[i] = 0;
}

/* ---------------------------------------------------------------------- */

void FixStmd::TCHK()
{
  if ((stmd_logfile) && (stmd_debug)) {
    fprintf(logfile,"  STMD TCHK: T1= %f (%f K)  Y2[0]= %f (%f K)\n",T1,T1*ST,Y2[0],Y2[0]*ST);
    fprintf(screen,"  STMD TCHK: T1= %f (%f K)  Y2[0]= %f (%f K)\n",T1,T1*ST,Y2[0],Y2[0]*ST);
  }
  if (Y2[0] == T1) STG = 2;
}

/* ---------------------------------------------------------------------- */

void FixStmd::HCHK()
{
  SWfold = SWf;

  int ichk = 0;
  int icnt = 0;
  double aveH = 0.0;

  // check CTmin and CTmax
  // average histogram
  for (int i=0; i<N; i++) {
    if ((Y2[i] > CTmin) && (Y2[i] < CTmax)) {
      aveH = aveH + double(Hist[i]);
      icnt++;
    }
  }

  if ((stmd_logfile) && (stmd_debug)) {
    fprintf(logfile,"  STMD CHK HIST: icnt= %i  aveH= %f  N= %i\n",icnt,aveH,N);
    fprintf(screen,"  STMD CHK HIST: icnt= %i  aveH= %f  N= %i\n",icnt,aveH,N);
  }
  if (icnt==0) return;

  aveH = aveH / double(icnt);

  double eval;
  for (int i=0; i<N; i++) {
    if ((Y2[i] > CTmin) && (Y2[i] < CTmax) ) {
      eval = abs(double(Hist[i] - aveH) / aveH);
      if (eval > HCKtol) ichk++;
      if ((stmd_logfile) && (stmd_debug)) {
        fprintf(logfile,"  STMD CHK HIST: totCi= %i  i= %i  eval= %f  HCKtol= %f  " 
            "ichk= %i  Hist[i]= %i\n",totCi,i,eval,HCKtol,ichk,Hist[i]);
        fprintf(screen,"  STMD CHK HIST: totCi= %i  i= %i  eval= %f  HCKtol= %f  " 
            "ichk= %i  Hist[i]= %i\n",totCi,i,eval,HCKtol,ichk,Hist[i]);
      }
    }
  }

  if (ichk < 1) SWf = SWf + 1;
}

/* ---------------------------------------------------------------------- */

void FixStmd::MAIN(int istep, double potE)
{
  Count = istep;
  totCi++;

  if (STG >= 3) CountPH++;

  if ((stmd_logfile) && (stmd_debug)) {
    fprintf(logfile,"STMD DEBUG: STAGE %i\n",STG);
    fprintf(screen,"STMD DEBUG: STAGE %i\n",STG);
    fprintf(logfile,"  STMD: Count=%i, f=%f\n",Count,f);
    fprintf(screen,"  STMD: Count=%i, f=%f\n",Count,f);
  }

  // Statistical Temperature Update
  int stmdi = Yval(potE);

  // Gamma Update
  GammaE(potE,stmdi);

  if ((stmd_logfile) && (stmd_debug)) {
    fprintf(logfile,"  STMD: totCi= %i Gamma= %f Hist[%i]= %i "
        "T= %f\n",totCi,Gamma,stmdi,Hist[stmdi],T);
    fprintf(screen,"  STMD: totCi= %i Gamma= %f Hist[%i]= %i "
        "T= %f\n",totCi,Gamma,stmdi,Hist[stmdi],T);
  }

  // Histogram Update
  AddedEHis(stmdi);
  CountH++;

  // Add to Histogram for production run
  if (STG >= 3) {
    PROH[stmdi]++;
    CountPH++;
  }

  // Hist Output
  int o = istep % RSTFRQ;
  if ((o == 0) && (comm->me == 0)) {
    for (int i=0; i<N; i++) fprintf(fp_whnm,"%i %f %i %i %f %i %i %f"
        "\n",i,(i*bin)+Emin,Hist[i],Htot[i],Y2[i],CountH,totCi,f);
    fprintf(fp_whnm,"\n\n");
  }
  
  // Production Run if STG >= 3
  // STG3 START: Check histogram and further reduce f until cutoff
  if (STG >= 3) {
    int m = istep % TSC2;

    // STMD, reduce f based on histogram flatness (original form)
    if (m == 0) {
      if ((stmd_logfile) && (stmd_debug)) {
        fprintf(logfile,"  STMD: istep= %i  TSC2= %i\n",istep,TSC2);
        fprintf(screen,"  STMD: istep= %i  TSC2= %i\n",istep,TSC2);
      }

      // Reduction based on histogram flatness
      if (f_flag == 1) { 
        HCHK(); // Check flatness
        if ((stmd_logfile) && (stmd_debug)) {
          fprintf(logfile,"  STMD: SWfold= %i  SWf= %i\n",SWfold,SWf);
          fprintf(logfile,"  STMD: f= %f  SWchk= %i\n",f,SWchk);
          fprintf(screen,"  STMD: SWfold= %i  SWf= %i\n",SWfold,SWf);
          fprintf(screen,"  STMD: f= %f  SWchk= %i\n",f,SWchk);
        }
        if (SWfold != SWf) {
          if (STG == 3) // dont reduce if STG4
            f = sqrt(f); // reduce f
          df = log(f) * 0.5 / bin;
          if ((stmd_logfile) && (stmd_debug)) {
            fprintf(logfile,"  STMD f-UPDATE: f= %f  SWf= %i  df= %f\n",f,SWf,df);
            fprintf(screen,"  STMD f-UPDATE: f= %f  SWf= %i  df= %f\n",f,SWf,df);
          }
          SWchk = 1;
          // Histogram reset
          ResetPH();
          CountH = 0;
        } 
        else {
          SWchk++;
          if ((stmd_logfile) && (stmd_debug)) {
            fprintf(logfile,"  STMD: f= %f  Swchk= %i T= %f\n",f,SWchk,T);
            fprintf(screen,"  STMD: f= %f  Swchk= %i T= %f\n",f,SWchk,T);
          }
        }
      } // if (f_flag == 1)

      if (f_flag > 1) {
        if ((stmd_logfile) && (stmd_debug)) {
          fprintf(logfile,"  STMD: istep= %i  TSC2= %i\n",istep,TSC2);
          fprintf(screen,"  STMD: istep= %i  TSC2= %i\n",istep,TSC2);
        }
        if (STG == 3) // dont reduce if STG4
          f = sqrt(f);
        df = log(f) * 0.5 / bin;
        if ((stmd_logfile) && (stmd_debug)) {
          fprintf(logfile,"  STMD f-UPDATE: f= %f  df= %f\n",f,df);
          fprintf(screen,"  STMD f-UPDATE: f= %f  df= %f\n",f,df);
        }
      
        // Histogram reset
        ResetPH();
        CountH = 0;
      } // if (f_flag > 1)

      // Check stage 3
      if (f <= finFval) STG = 4;

      // Production run: Hist Output STMD
      if ((o == 0) && (comm->me == 0)) {
        for (int i=0; i<N; i++)
          fprintf(fp_whpnm,"%i %f %i %i %i %f %i %i %f\n", i, (i*bin)+Emin,\
              Hist[i],PROH[i],Htot[i],Y2[i],CountH,CountPH,f);
        fprintf(fp_whpnm,"\n\n");
      }
    } // if ((m == 0)
  } // if (STG >= 3)

  // STG2 START: Check histogram and modify f value on STG2
  // If STMD, run until histogram is flat, then reduce f value 
  // else if RESTMD, reduce every TSC2 steps
  if (STG == 2) {

    int m = istep % TSC2;
    if (m == 0) {
      if ((stmd_logfile) && (stmd_debug)) {
        fprintf(logfile,"  STMD: istep= %i  TSC2= %i\n",istep,TSC2);
        fprintf(screen,"  STMD: istep= %i  TSC2= %i\n",istep,TSC2);
      }

      // No f-reduction, simulate at initf only!
      if (f_flag == 0) { 
        ResetPH();
        CountH = 0;
      }

      // Standard f-reduction as HCHK() every m steps
      if (f_flag == 1) { 
        HCHK();
        if ((stmd_logfile) && (stmd_debug)) {
          fprintf(logfile,"  STMD: SWfold= %i SWf= %i\n",SWfold,SWf);
          fprintf(screen,"  STMD: SWfold= %i SWf= %i\n",SWfold,SWf);
        }
        // f value update
        if (SWfold != SWf) {
          f = sqrt(f);
          df = log(f) * 0.5 / bin;
          if ((stmd_logfile) && (stmd_debug)) {
            fprintf(logfile,"  STMD f-UPDATE: f= %f  SWf= %i  df= %f\n",f,SWf,df);
            fprintf(screen,"  STMD f-UPDATE: f= %f  SWf= %i  df= %f\n",f,SWf,df);
          }
          SWchk = 1;
          ResetPH();
          CountH = 0;
        }
        else SWchk++;

        if ((stmd_logfile) && (stmd_debug)) {
          fprintf(logfile,"  STMD RESULTS: totCi= %i  f= %f  SWf= %i  SWchk= %i  "
              "STG= %i\n",totCi,f,SWf,SWchk,STG);
          fprintf(screen,"  STMD RESULTS: totCi= %i  f= %f  SWf= %i  SWchk= %i  "
              "STG= %i\n",totCi,f,SWf,SWchk,STG);
        }
        if (f <= pfinFval) {
          STG = 3;
          CountPH = 0;
          SWchk = 1;
          ResetPH();
          CountH = 0;
        }
      } // if (f_flag == 1)

      if (f_flag == 2) { 
      // Reduce as sqrtf every m steps
        if (istep != 0) {
          f = sqrt(f);
          df = log(f) * 0.5 / bin;
        }

        ResetPH();
        CountH = 0;
      } // if (f_flag == 2)

      // Reduce f by constant every m steps
      // otherwise, reduce by sqrt(f) if too small
      if (f_flag == 3) { 
        double reduce_val = 0.1;
        if (istep != 0) {
          if (f > (1+(2*reduce_val)))
            f = f - (reduce_val*f);
          else f = sqrt(f);
        }
        df = log(f) * 0.5 / bin;
        ResetPH();
        CountH = 0;
      } // if (f_flag == 3)

      // Reduce df by constant every m steps
      if (f_flag == 4) {
        double reduce_val = 0.01; // 1% reduction
        if (istep != 0) {
          df = df - (df * reduce_val);
          f = exp(2 * bin * df);
        }
      } // if (f_flag == 4)

      if (f <= 1.0)
        error->all(FLERR,"f-value is less than unity");

      if ((stmd_logfile) && (stmd_debug) && (f_flag > 1)) {
	      fprintf(logfile,"  STMD f-UPDATE: f= %f  df= %f\n",f,df);
	      fprintf(screen,"  STMD f-UPDATE: f= %f  df= %f\n",f,df);
      }

      if ((f <= pfinFval) && (f_flag > 1)) {
        STG = 3;
        CountPH = 0;
      }

    } // if (m == 0)
  } // if (STG == 2)

  // STG1 START: Digging and chk stage on STG1
  // Run until lowest temperature sampled
  if (STG == 1) {
    int m = istep % TSC1;
    if (m == 0) {
      if (stmd_logfile) {
        fprintf(logfile,"  STMD DIG: istep= %i  TSC1= %i Tlow= %f\n",istep,TSC1,T);
        fprintf(screen,"  STMD DIG: istep= %i  TSC1= %i Tlow= %f\n",istep,TSC1,T);
      }

      dig();
      TCHK();

      // Histogram reset
      if (STG > 1) {
        ResetPH();
        CountH = 0;
      }
    } // if (m == 0) 
  } // if (STG == 1) 

  if ((stmd_logfile) && (stmd_debug)) {
    fprintf(logfile,"STMD NEXT STG= %i\n",STG);
    fprintf(screen,"STMD NEXT STG= %i\n",STG);
  }
}

/* ---------------------------------------------------------------------- */

double FixStmd::compute_scalar()
{
  return T*ST;
}

/* ---------------------------------------------------------------------- */

double FixStmd::compute_vector(int i)
{
  // Returns useful info for STMD
  double xx;
  if      (i == 0) xx = static_cast<double>(STG);             // Current STG
  else if (i == 1) xx = static_cast<double>(N);               // Number of bins
  else if (i == 2) xx = static_cast<double>(BinMin);          // Lower limit of energy: Emin
  else if (i == 3) xx = static_cast<double>(BinMax);          // Upper limit of energy: Emax
  else if (i == 4) xx = static_cast<double>(curbin);          // Last sampled bin
  else if (i == 5) xx = bin;                                  // Bin spacing of energy: \Delta
  else if (i == 6) xx = df;                                   // df-value
  else if (i == 7) xx = Gamma;                                // force scalling factor

  return xx;
}

/* ---------------------------------------------------------------------- */

double FixStmd::compute_array(int i, int j)
{
  // Returns data from arrays
  double xx;
  if      (i == 0) xx = (j*bin)+Emin;    // Binned Energies
  else if (i == 1) xx = Y2[j];           // Histogram of temperature 1/T*j
  else if (i == 2) xx = Hist[j];         // Histogram of energies*j
  else if (i == 3) xx = PROH[j];         // Production histogram*j

  return xx;
}

/* ---------------------------------------------------------------------- */
/* --- Trigger reinitialization of key arrays after modify_fix called --- */
/* ---------------------------------------------------------------------- */

void FixStmd::modify_fix(int which, double *values, char *notused)
{
  // Sets a specified variable to the input value(s)
  if      (which == 0) BinMin = static_cast<int>(values[0] + 0.5);
  else if (which == 1) BinMax = static_cast<int>(values[0] + 0.5);
  else if (which == 2) bin    = static_cast<int>(values[0] + 0.5);
  else if (which == 3) {
    for (int i=0; i<N; i++) Y2[i] = values[i];
  }
}

/* ---------------------------------------------------------------------- */
/*     Extract scale factor                                               */
/* ---------------------------------------------------------------------- */

void *FixStmd::extract(const char *str, int &dim)
{
  dim=0;
  if (strcmp(str,"scale_stmd") == 0) {
    return &Gamma;
  }
  return NULL;
}
