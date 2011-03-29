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

/* ----------------------------------------------------------------------
   Contributing authors: Mike Brown (ORNL), brownw@ornl.gov
------------------------------------------------------------------------- */

#ifndef PAIR_GPU_DEVICE_H
#define PAIR_GPU_DEVICE_H

#include "pair_gpu_atom.h"
#include "pair_gpu_ans.h"
#include "pair_gpu_nbor.h"
#include "pppm_gpu_memory.h"
#include "mpi.h"
#include <sstream>
#include "stdio.h"
#include <string>
#include <queue>

template <class numtyp, class acctyp, 
          class grdtyp, class grdtyp4> class PPPMGPUMemory;

template <class numtyp, class acctyp>
class PairGPUDevice {
 public:
  PairGPUDevice();
  ~PairGPUDevice(); 
 
  /// Initialize the device for use by this process
  /** Sets up a per-device MPI communicator for load balancing and initializes
    * the device (>=first_gpu and <=last_gpu) that this proc will be using **/
  bool init_device(MPI_Comm world, MPI_Comm replica, const int first_gpu, 
                   const int last_gpu, const int gpu_mode, 
                   const double particle_split, const int nthreads);

  /// Initialize the device for Atom and Neighbor storage
  /** \param rot True if quaternions need to be stored
    * \param nlocal Total number of local particles to allocate memory for
    * \param host_nlocal Initial number of host particles to allocate memory for
    * \param nall Total number of local+ghost particles
    * \param gpu_nbor True if neighboring is performed on device
    * \param gpu_host 0 if host will not perform force calculations,
    *                 1 if gpu_nbor is true, and host needs a half nbor list,
    *                 2 if gpu_nbor is true, and host needs a full nbor list
    * \param max_nbors Initial number of rows in the neighbor matrix
    * \param cell_size cutoff+skin 
    * \param pre_cut True if cutoff test will be performed in separate kernel
    *                than the force kernel **/
  bool init(PairGPUAns<numtyp,acctyp> &a, const bool charge, const bool rot,
            const int nlocal, const int host_nlocal, const int nall,
            PairGPUNbor *nbor, const int maxspecial, const int gpu_host,
            const int max_nbors, const double cell_size, const bool pre_cut);

  /// Initialize the device for Atom storage only
  /** \param nlocal Total number of local particles to allocate memory for
    * \param nall Total number of local+ghost particles **/
  bool init(PairGPUAns<numtyp,acctyp> &ans, const int nlocal, const int nall);

  /// Output a message for pair_style acceleration with device stats
  void init_message(FILE *screen, const char *name,
                    const int first_gpu, const int last_gpu);

  /// Perform charge assignment asynchronously for PPPM
	void set_single_precompute(PPPMGPUMemory<numtyp,acctyp,
	                                         float,_lgpu_float4> *pppm);

  /// Perform charge assignment asynchronously for PPPM
	void set_double_precompute(PPPMGPUMemory<numtyp,acctyp,
	                                         double,_lgpu_double4> *pppm);

  /// Esimate the overhead from GPU calls from multiple procs
  /** \param kernel_calls Number of kernel calls/timestep for timing estimated
    *                     overhead
    * \param gpu_overhead Estimated gpu overhead per timestep (sec)
    * \param driver_overhead Estimated overhead from driver per timestep (s) **/
  void estimate_gpu_overhead(const int kernel_calls, double &gpu_overhead,
                             double &gpu_driver_overhead);

  /// Returns true if double precision is supported on card
  inline bool double_precision() { return gpu->double_precision(); }
  
  /// Output a message with timing information
  void output_times(UCL_Timer &time_pair, PairGPUAns<numtyp,acctyp> &ans, 
                    PairGPUNbor &nbor, const double avg_split, 
                    const double max_bytes, const double gpu_overhead,
                    const double driver_overhead, FILE *screen);

  /// Output a message with timing information
  void output_kspace_times(UCL_Timer &time_in, UCL_Timer &time_out,
                           UCL_Timer & time_map, UCL_Timer & time_rho,
                           UCL_Timer &time_interp, 
                           PairGPUAns<numtyp,acctyp> &ans, 
                           const double max_bytes, const double cpu_time,
                           FILE *screen);

  /// Clear all memory on host and device associated with atom and nbor data
  void clear();
  
  /// Clear all memory on host and device
  void clear_device();

  /// Add an answer object for putting forces, energies, etc from GPU to LAMMPS
  inline void add_ans_object(PairGPUAns<numtyp,acctyp> *ans)
    { ans_queue.push(ans); }

  /// Add "answers" (force,energies,etc.) into LAMMPS structures
  inline double fix_gpu(double **f, double **tor, double *eatom,
                        double **vatom, double *virial, double &ecoul) {
    atom.data_unavail();
    if (ans_queue.empty()==false) {
      stop_host_timer();
      double evdw=0.0;
      while (ans_queue.empty()==false) {
        evdw+=ans_queue.front()->get_answers(f,tor,eatom,vatom,virial,ecoul);
        ans_queue.pop();
      }                                                 
      return evdw;
    }
    return 0.0;
  }

  /// Start timer on host
  inline void start_host_timer() 
    { _cpu_full=MPI_Wtime(); _host_timer_started=true; }
  
  /// Stop timer on host
  inline void stop_host_timer() { 
    if (_host_timer_started) {
      _cpu_full=MPI_Wtime()-_cpu_full; 
      _host_timer_started=false;
    }
  }
  
  /// Return host time
  inline double host_time() { return _cpu_full; }

  /// Return host memory usage in bytes
  double host_memory_usage() const;

  /// Return the number of procs sharing a device (size of device commincator)
  inline int procs_per_gpu() const { return _procs_per_gpu; }
  /// Return the number of threads per proc
  inline int num_threads() const { return _nthreads; }
  /// My rank within all processes
  inline int world_me() const { return _world_me; }
  /// Total number of processes
  inline int world_size() const { return _world_size; }
  /// MPI Barrier for world
  inline void world_barrier() { MPI_Barrier(_comm_world); }
  /// Return the replica MPI communicator
  inline MPI_Comm & replica() { return _comm_replica; }
  /// My rank within replica communicator
  inline int replica_me() const { return _replica_me; }
  /// Number of procs in replica communicator
  inline int replica_size() const { return _replica_size; }
  /// Return the per-GPU MPI communicator
  inline MPI_Comm & gpu_comm() { return _comm_gpu; }
  /// Return my rank in the device communicator
  inline int gpu_rank() const { return _gpu_rank; }
  /// MPI Barrier for gpu
  inline void gpu_barrier() { MPI_Barrier(_comm_gpu); }
  /// Return the 'mode' for acceleration: GPU_FORCE or GPU_NEIGH
  inline int gpu_mode() const { return _gpu_mode; }
  /// Index of first device used by a node
  inline int first_device() const { return _first_device; }
  /// Index of last device used by a node
  inline int last_device() const { return _last_device; }
  /// Particle split defined in fix
  inline double particle_split() const { return _particle_split; }
  /// Return the initialization count for the device
  inline int init_count() const { return _init_count; }

  // -------------------- SHARED DEVICE ROUTINES -------------------- 
  // Perform asynchronous zero of integer array 
  void zero(UCL_D_Vec<int> &mem, const int numel) {
    int num_blocks=static_cast<int>(ceil(static_cast<double>(numel)/
                                    _block_size));
    k_zero.set_size(num_blocks,_block_size);
    k_zero.run(&mem.begin(),&numel);
  }

  // -------------------------- DEVICE DATA ------------------------- 

  /// Geryon Device
  UCL_Device *gpu;

  enum{GPU_FORCE, GPU_NEIGH};

  // --------------------------- ATOM DATA -------------------------- 

  /// Atom Data
  PairGPUAtom<numtyp,acctyp> atom;

  // --------------------------- NBOR DATA ----------------------------
  
  /// Neighbor Data
  PairGPUNborShared _nbor_shared;

  // ------------------------ LONG RANGE DATA -------------------------
  
  // Long Range Data
  int _long_range_precompute;
  PPPMGPUMemory<numtyp,acctyp,float,_lgpu_float4> *pppm_single;
  PPPMGPUMemory<numtyp,acctyp,double,_lgpu_double4> *pppm_double;
  /// Precomputations for long range charge assignment (asynchronously)
//  inline void precompute(const int ago, const int nlocal, const int nall,
//                         double **host_x, int *host_type, bool &success,
//                         double *charge, double *boxlo, const double delxinv,
//                         const double delyinv, const double delzinv) {
                         


 private:
  std::queue<PairGPUAns<numtyp,acctyp> *> ans_queue;
  int _init_count;
  bool _device_init, _host_timer_started;
  MPI_Comm _comm_world, _comm_replica, _comm_gpu;
  int _procs_per_gpu, _gpu_rank, _world_me, _world_size, _replica_me, 
      _replica_size;
  int _gpu_mode, _first_device, _last_device, _nthreads;
  double _particle_split;
  double _cpu_full;

  int _block_size;
  UCL_Program *dev_program;
  UCL_Kernel k_zero;
  bool _compiled;
  void compile_kernels();

  int _data_in_estimate, _data_out_estimate;
  
  template <class t>
  inline std::string toa(const t& in) {
    std::ostringstream o;
    o.precision(2);
    o << in;
    return o.str();
  }

};

#endif
