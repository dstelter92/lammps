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

#ifdef COMMAND_CLASS

CommandStyle(temper/stmd,TemperStmd)

#else

#ifndef LMP_TEMPER_STMD_H
#define LMP_TEMPER_STMD_H

#include "pointers.h"

namespace LAMMPS_NS {

class TemperStmd : protected Pointers {
 public:
  TemperStmd(class LAMMPS *);
  ~TemperStmd();
  void command(int, char **);

 private:
  int me,me_universe;          // my proc ID in world and universe
  int iworld,nworlds;          // world info
  double boltz;                // copy from output->boltz
  MPI_Comm roots;              // MPI comm with 1 root proc from each world
  class RanPark *ranswap,*ranboltz;  // RNGs for swapping and Boltz factor
  int nevery;                  // # of timesteps between swaps
  int nswaps;                  // # of tempering swaps to perform
  int seed_swap;               // 0 = toggle swaps, n = RNG for swap direction
  int seed_boltz;              // seed for Boltz factor comparison
  int whichfix;                // index of temperature fix to use
  int fixstyle;                // what kind of temperature fix is used
  int bin, Emin, Emax, BinMin, BinMax;
  double T_me,T_partner;       // latest sampled temperture
  int current_STG;             // current STMD stage
  int EX_flag;                 // controls if swap is turned OFF/ON (0/1)

  int my_set_temp;             // which set temp I am simulating
  double *set_temp;            // static list of replica set kinetic temperatures
  double *local_values;        // list of Y2, Emin and Emax
  double *global_values;       // global list of all local_values
  int *temp2world;             // temp2world[i] = world simulating set temp i
  int *world2temp;             // world2temp[i] = temp simulated by world i
  int *world2root;             // world2root[i] = root proc of world i

  void print_status();

  class FixStmd * fix_stmd;
};

}

#endif
#endif

/* ERROR/WARNING messages:

E: Must have more than one processor partition to temper

Cannot use the temper command with only one processor partition.  Use
the -partition command-line option.

E: Temper command before simulation box is defined

The temper command cannot be used before a read_data, read_restart, or
create_box command.

E: Illegal ... command

Self-explanatory.  Check the input script syntax and compare to the
documentation for the command.  You can use -echo screen as a
command-line option when running LAMMPS to see the offending line.

E: Tempering fix ID is not defined

The fix ID specified by the temper command does not exist.

E: Kinetic temperatures not the same, use homogeneous temperature
control

This version uses homogeneous temperature control (HK) for the kinetic
temperature in each replica. Review http://dx.doi.org/10.1021/jp300366j 
for details.

E: Invalid frequency in temper command

Nevery must be > 0.

E: Non integer # of swaps in temper command

Swap frequency in temper command must evenly divide the total # of
timesteps.

E: Tempering temperature fix is not valid

The fix specified by the temper command is not one that controls
temperature (nvt or langevin).

E: Must use with fix STMD, fix is not valid

Self-explanatory.

E: Too many timesteps

The cummulative timesteps must fit in a 64-bit integer.

E: Tempering could not find thermo_pe compute

This compute is created by the thermo command.  It must have been
explicitly deleted by a uncompute command.

W: RESTMD still in STAGE1, ensure exchanges turned off

Replica exchange must be turned off during STG1, use EX_FLAG

*/
