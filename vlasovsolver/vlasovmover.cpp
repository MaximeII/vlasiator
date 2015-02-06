/*
  This file is part of Vlasiator.

  Copyright 2010-2015 Finnish Meteorological Institute

*/

#include <cstdlib>
#include <iostream>
#include <vector>

#ifdef _OPENMP
   #include "omp.h"
#endif
#include <zoltan.h>

#include "../vlasovmover.h"
#include "phiprof.hpp"

#include "cpu_moments.h"
#include "cpu_acc_semilag.hpp"
#include "cpu_trans_map.hpp"

#include <stdint.h>
#include <dccrg.hpp>

#include "spatial_cell.hpp"
#include "../grid.h"
#include "../definitions.h"

using namespace std;
using namespace spatial_cell;

creal ZERO    = 0.0;
creal HALF    = 0.5;
creal FOURTH  = 1.0/4.0;
creal SIXTH   = 1.0/6.0;
creal ONE     = 1.0;
creal TWO     = 2.0;
creal EPSILON = 1.0e-25;


/*!
  
  Propagates the distribution function in spatial space. 
  
  Based on SLICE-3D algorithm: Zerroukat, M., and T. Allen. "A
  three‐dimensional monotone and conservative semi‐Lagrangian scheme
  (SLICE‐3D) for transport problems." Quarterly Journal of the Royal
  Meteorological Society 138.667 (2012): 1640-1651.

*/
void calculateSpatialTranslation(
        dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,creal dt,
        const std::vector<CellID>& local_propagated_cells,
        const std::vector<CellID>& local_target_cells,
        const std::vector<CellID>& remoteTargetCellsx,
        const std::vector<CellID>& remoteTargetCellsy,
        const std::vector<CellID>& remoteTargetCellsz,
        const int& popID) {
   
   int trans_timer;
   bool localTargetGridGenerated = false;
   
   // ------------- SLICE - map dist function in Z --------------- //
   if(P::zcells_ini > 1 ){
      trans_timer=phiprof::initializeTimer("transfer-stencil-data-z","MPI");
      phiprof::start(trans_timer);
      SpatialCell::set_mpi_transfer_type(Transfer::VEL_BLOCK_DATA);
      mpiGrid.start_remote_neighbor_copy_updates(VLASOV_SOLVER_Z_NEIGHBORHOOD_ID);
      phiprof::stop(trans_timer);
      
      /*generate target grid in the temporary arrays, same size as
       *   original one. We only need to create these in target cells*/
      createTargetGrid(mpiGrid,remoteTargetCellsz);

      if(!localTargetGridGenerated){ 
         createTargetGrid(mpiGrid,local_target_cells);
         localTargetGridGenerated=true;
      }
      
      phiprof::start(trans_timer);
      mpiGrid.wait_remote_neighbor_copy_update_receives(VLASOV_SOLVER_Z_NEIGHBORHOOD_ID);
      phiprof::stop(trans_timer);
      
      phiprof::start("compute-mapping-z");
      #pragma omp parallel
      {
         for (size_t c=0; c<local_propagated_cells.size(); ++c) {
            trans_map_1d(mpiGrid,local_propagated_cells[c], 2, dt,popID); // map along z//
         }
      }
      phiprof::stop("compute-mapping-z");

      phiprof::start(trans_timer);
      mpiGrid.wait_remote_neighbor_copy_update_sends();
      phiprof::stop(trans_timer);
      
      trans_timer=phiprof::initializeTimer("update_remote-z","MPI");
      phiprof::start("update_remote-z");
      update_remote_mapping_contribution(mpiGrid, 2,+1,popID);
      update_remote_mapping_contribution(mpiGrid, 2,-1,popID);
      phiprof::stop("update_remote-z");

      clearTargetGrid(mpiGrid,remoteTargetCellsz);
      swapTargetSourceGrid(mpiGrid, local_target_cells,popID);
      zeroTargetGrid(mpiGrid, local_target_cells);
   }

   // ------------- SLICE - map dist function in X --------------- //
   if(P::xcells_ini > 1 ){
      trans_timer=phiprof::initializeTimer("transfer-stencil-data-x","MPI");
      phiprof::start(trans_timer);
      SpatialCell::set_mpi_transfer_type(Transfer::VEL_BLOCK_DATA);
      mpiGrid.start_remote_neighbor_copy_updates(VLASOV_SOLVER_X_NEIGHBORHOOD_ID);
      phiprof::stop(trans_timer);
      
      createTargetGrid(mpiGrid,remoteTargetCellsx);
       if(!localTargetGridGenerated){ 
         createTargetGrid(mpiGrid,local_target_cells);
         localTargetGridGenerated=true;
      }
       
      phiprof::start(trans_timer);
      mpiGrid.wait_remote_neighbor_copy_update_receives(VLASOV_SOLVER_X_NEIGHBORHOOD_ID);
      phiprof::stop(trans_timer);

      phiprof::start("compute-mapping-x");
      #pragma omp parallel
      {
         for (size_t c=0; c<local_propagated_cells.size(); ++c) {
            trans_map_1d(mpiGrid,local_propagated_cells[c], 0, dt,popID); // map along x//
         }
      }
      phiprof::stop("compute-mapping-x");

      phiprof::start(trans_timer);
      mpiGrid.wait_remote_neighbor_copy_update_sends();
      phiprof::stop(trans_timer);
      
      trans_timer=phiprof::initializeTimer("update_remote-x","MPI");
      phiprof::start("update_remote-x");
      update_remote_mapping_contribution(mpiGrid, 0,+1,popID);
      update_remote_mapping_contribution(mpiGrid, 0,-1,popID);
      phiprof::stop("update_remote-x");
      clearTargetGrid(mpiGrid,remoteTargetCellsx);
      swapTargetSourceGrid(mpiGrid, local_target_cells,popID);
      zeroTargetGrid(mpiGrid, local_target_cells);
   }
   
   // ------------- SLICE - map dist function in Y --------------- //
   if(P::ycells_ini > 1 ){
      trans_timer=phiprof::initializeTimer("transfer-stencil-data-y","MPI");
      phiprof::start(trans_timer);
      SpatialCell::set_mpi_transfer_type(Transfer::VEL_BLOCK_DATA);
      mpiGrid.start_remote_neighbor_copy_updates(VLASOV_SOLVER_Y_NEIGHBORHOOD_ID);
      phiprof::stop(trans_timer);
      
      createTargetGrid(mpiGrid,remoteTargetCellsy);
      if(!localTargetGridGenerated){ 
         createTargetGrid(mpiGrid,local_target_cells);
         localTargetGridGenerated=true;
      }
      
      phiprof::start(trans_timer);
      mpiGrid.wait_remote_neighbor_copy_update_receives(VLASOV_SOLVER_Y_NEIGHBORHOOD_ID);
      phiprof::stop(trans_timer);

      phiprof::start("compute-mapping-y");
      #pragma omp parallel
      {
         for (size_t c=0; c<local_propagated_cells.size(); ++c) {
            trans_map_1d(mpiGrid,local_propagated_cells[c], 1, dt,popID); // map along y//
         }
      }
      
      phiprof::stop("compute-mapping-y");

      phiprof::start(trans_timer);
      mpiGrid.wait_remote_neighbor_copy_update_sends();
      phiprof::stop(trans_timer);
      
      trans_timer=phiprof::initializeTimer("update_remote-y","MPI");
      phiprof::start("update_remote-y");
      update_remote_mapping_contribution(mpiGrid, 1,+1,popID);
      update_remote_mapping_contribution(mpiGrid, 1,-1,popID);
      phiprof::stop("update_remote-y");
      clearTargetGrid(mpiGrid,remoteTargetCellsy);
      swapTargetSourceGrid(mpiGrid, local_target_cells,popID);
   }

   clearTargetGrid(mpiGrid,local_target_cells);
}
   
void calculateSpatialTranslation(
   dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
   creal dt
) {
   const size_t popID = 0;
   typedef Parameters P;

   cerr << "spat trans" << endl;
   phiprof::start("semilag-trans");

   // Calculate propagated cells, these are the same for all particle populations
   phiprof::start("compute_cell_lists");
   const vector<CellID> localCells = mpiGrid.get_cells();
   const vector<CellID> remoteTargetCellsx = mpiGrid.get_remote_cells_on_process_boundary(VLASOV_SOLVER_TARGET_X_NEIGHBORHOOD_ID);
   const vector<CellID> remoteTargetCellsy = mpiGrid.get_remote_cells_on_process_boundary(VLASOV_SOLVER_TARGET_Y_NEIGHBORHOOD_ID);
   const vector<CellID> remoteTargetCellsz = mpiGrid.get_remote_cells_on_process_boundary(VLASOV_SOLVER_TARGET_Z_NEIGHBORHOOD_ID);

   vector<CellID> local_propagated_cells;
   vector<CellID> local_target_cells;
   for (size_t c=0; c<localCells.size(); ++c) {
      if(do_translate_cell(mpiGrid[localCells[c]])){
         local_propagated_cells.push_back(localCells[c]);
      }
   }
   for (size_t c=0; c<localCells.size(); ++c) {
      if(mpiGrid[localCells[c]]->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY) {
         local_target_cells.push_back(localCells[c]);
      }
   }
   phiprof::stop("compute_cell_lists");

   // Propagate all particle species
   for (size_t p=0; p<getObjectWrapper().particleSpecies.size(); ++p) {
      SpatialCell::setCommunicatedSpecies(p);
      calculateSpatialTranslation(mpiGrid,dt,local_propagated_cells,local_target_cells,
              remoteTargetCellsx,remoteTargetCellsy,remoteTargetCellsz,p);
      
   }

   // Mapping complete, update moments //
   phiprof::start("compute-moments-n-maxdt");
   
   cerr << "moments" << endl;
   // Note: Parallelization over blocks is not thread-safe
   #pragma omp  parallel for
   for (size_t c=0; c<localCells.size(); ++c) {
      SpatialCell* SC=mpiGrid[localCells[c]];
      Real* cellParams  = SC->get_cell_parameters();
      
      // Clear old moments
      const Real dx=SC->parameters[CellParams::DX];
      const Real dy=SC->parameters[CellParams::DY];
      const Real dz=SC->parameters[CellParams::DZ];
      SC->parameters[CellParams::RHO_R  ] = 0.0;
      SC->parameters[CellParams::RHOVX_R] = 0.0;
      SC->parameters[CellParams::RHOVY_R] = 0.0;
      SC->parameters[CellParams::RHOVZ_R] = 0.0;
      SC->parameters[CellParams::P_11_R ] = 0.0;
      SC->parameters[CellParams::P_22_R ] = 0.0;
      SC->parameters[CellParams::P_33_R ] = 0.0;

      // Reset spatial max DT
      cellParams[CellParams::MAXRDT]=numeric_limits<Real>::max();
      
      for (int popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
         // This prevents get_block_parameters from segfaulting if there're no blocks
         if (SC->get_number_of_velocity_blocks(popID) == 0) continue;         
         
         // Get pointer to velocity block data (for this species)
         const Realf* data       = SC->get_data(popID);
         const Real* blockParams = SC->get_block_parameters(popID);
         
         // Array for storing temporary moment values
         Real array[4]; 
         for (int i=0; i<4; ++i) array[i] = 0;
         
         for (vmesh::LocalID blockLID=0; blockLID<SC->get_number_of_velocity_blocks(popID); ++blockLID) {
            // Compute maximum dt. Algorithm has a CFL condition, since it
            // is written only for the case where we have a stencil
            // supporting max translation of one cell.
            for (unsigned int i=0; i<WID; i+=(WID-1)) {
               const Real Vx = blockParams[BlockParams::VXCRD] + (i+HALF)*blockParams[BlockParams::DVX];
               const Real Vy = blockParams[BlockParams::VYCRD] + (i+HALF)*blockParams[BlockParams::DVY];
               const Real Vz = blockParams[BlockParams::VZCRD] + (i+HALF)*blockParams[BlockParams::DVZ];
            
               if (fabs(Vx) != ZERO) cellParams[CellParams::MAXRDT]=min(dx/fabs(Vx),cellParams[CellParams::MAXRDT]);
               if (fabs(Vy) != ZERO) cellParams[CellParams::MAXRDT]=min(dy/fabs(Vy),cellParams[CellParams::MAXRDT]);
               if (fabs(Vz) != ZERO) cellParams[CellParams::MAXRDT]=min(dz/fabs(Vz),cellParams[CellParams::MAXRDT]);
            }
            
            // Compute first moments for this block, 
            // moments stored to array indices 0-3.
            if (SC->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY) {
               blockVelocityFirstMoments(
                       data,
                       blockParams,
                       array
               );
            }

            data        += SIZE_VELBLOCK;
            blockParams += BlockParams::N_VELOCITY_BLOCK_PARAMS;
         } // for-loop over blocks

         // Accumulate the contribution of this species to velocity 
         // moments, taking the mass correctly into account.
         const Real massRatio = getObjectWrapper().particleSpecies[popID].mass / physicalconstants::MASS_PROTON;
         cellParams[CellParams::RHO_R  ] += array[0]*massRatio;
         cellParams[CellParams::RHOVX_R] += array[1]*massRatio;
         cellParams[CellParams::RHOVY_R] += array[2]*massRatio;
         cellParams[CellParams::RHOVZ_R] += array[3]*massRatio;
      } // for-loop over particle species
      cerr << "first moments" << endl;
      
      // Compute the second velocity moments (pressure) for this cell
      for (int popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
         // This prevents get_block_parameters from segfaulting if there're no blocks
         if (SC->get_number_of_velocity_blocks(popID) == 0) continue;         

         // Get pointer to velocity block data (for this species)
         const Realf* data       = SC->get_data(popID);
         const Real* blockParams = SC->get_block_parameters(popID);
         
         // Array for storing temporary moment values
         Real array[3]; 
         for (int i=0; i<3; ++i) array[i] = 0;
         
         for (vmesh::LocalID blockLID=0; blockLID<SC->get_number_of_velocity_blocks(popID); ++blockLID) {
            if (SC->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY) {
               blockVelocitySecondMoments(
                  data,
                  blockParams,
                  cellParams,
                  CellParams::RHO_R,
                  CellParams::RHOVX_R,
                  CellParams::RHOVY_R,
                  CellParams::RHOVZ_R,
                  array
               );
            }
         } // for-loop over velocity blocks

         // Accumulate the contribution of this species to velocity 
         // moments, taking the mass correctly into account.
         const Real mass = getObjectWrapper().particleSpecies[popID].mass;
         cellParams[CellParams::P_11_R] += array[0]*mass;
         cellParams[CellParams::P_22_R] += array[1]*mass;
         cellParams[CellParams::P_33_R] += array[2]*mass;         
      } // for-loop over particle species
      cerr << "second moments" << endl;
   } // for-loop over spatial cells

   phiprof::stop("compute-moments-n-maxdt");
   phiprof::stop("semilag-trans");
}

/*
  --------------------------------------------------
  Acceleration (velocity space propagation)
  --------------------------------------------------
*/

int getAccerelationSubcycles(SpatialCell* sc, Real dt){
   return max(convert<int>(ceil(dt / sc->parameters[CellParams::MAXVDT])),1);
}

void calculateAcceleration(const int& popID,const int& globalMaxSubcycles,const uint& step,
        dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
        const std::vector<CellID>& propagatedCells,
        const Real& dt) {

   // Set active population
   SpatialCell::setCommunicatedSpecies(popID);

   //Semilagrangian acceleration for those cells which are subcycled
   #pragma omp parallel for schedule(dynamic,1)
   for (size_t c=0; c<propagatedCells.size(); ++c) {
      const CellID cellID = propagatedCells[c];
      const Real maxVdt = mpiGrid[cellID]->parameters[CellParams::MAXVDT]; 

      //compute subcycle dt. The length is maVdt on all steps
      //except the last one. This is to keep the neighboring
      //spatial cells in sync, so that two neighboring cells with
      //different number of subcycles have similar timesteps,
      //except that one takes an additional short step. This keeps
      //spatial block neighbors as much in sync as possible for
      //adjust blocks.
      Real subcycleDt;
      if( (step + 1) * maxVdt > dt) {
         subcycleDt = dt - step * maxVdt;
      } else{
         subcycleDt = maxVdt;
      }

      //generate pseudo-random order which is always the same irrespective of parallelization, restarts, etc
      char rngStateBuffer[256];
      random_data rngDataBuffer;

      // set seed, initialise generator and get value
      memset(&(rngDataBuffer), 0, sizeof(rngDataBuffer));
      #ifdef _AIX
         initstate_r(P::tstep + cellID, &(rngStateBuffer[0]), 256, NULL, &(rngDataBuffer));
         int64_t rndInt;
         random_r(&rndInt, &rngDataBuffer);
      #else
         initstate_r(P::tstep + cellID, &(rngStateBuffer[0]), 256, &(rngDataBuffer));
         int32_t rndInt;
         random_r(&rngDataBuffer, &rndInt);
      #endif
         
      uint map_order=rndInt%3;
      phiprof::start("cell-semilag-acc");
      cpu_accelerate_cell(mpiGrid[cellID],map_order,subcycleDt,popID);
      phiprof::stop("cell-semilag-acc");
   }
      
   //global adjust after each subcycle to keep number of blocks managable. Even the ones not
   //accelerating anyore participate. It is important to keep
   //the spatial dimension to make sure that we do not loose
   //stuff streaming in from other cells, perhaps not connected
   //to the existing distribution function in the cell.
   //- All cells update and communicate their lists of content blocks
   //- Only cells which were accerelated on this step need to be adjusted (blocks removed or added).
   //- Not done here on last step (done after loop)
   if(step < (globalMaxSubcycles - 1)) adjustVelocityBlocks(mpiGrid, propagatedCells, false, popID);
}

void calculateAcceleration(
   dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
   Real dt
) {
   typedef Parameters P;
   const vector<CellID> cells = mpiGrid.get_cells();
//    if(dt > 0)  // FIXME this has to be deactivated to support regular projects but it breaks test_trans support most likely, with this on dt stays 0
   phiprof::start("semilag-acc");

   // Iterate through all local cells and collect cells to propagate.
   // Ghost cells (spatial cells at the boundary of the simulation 
   // volume) do not need to be propagated:
   vector<CellID> propagatedCells;
   for (size_t c=0; c<cells.size(); ++c) {
      SpatialCell* SC = mpiGrid[cells[c]];
      //disregard boundary cells
      //do not integrate cells with no blocks  (well, do not computes in practice)
      #warning In principle this is different for different species
      if (SC->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY &&
          SC->get_number_of_all_velocity_blocks() != 0) {
         propagatedCells.push_back(cells[c]);
      }
   }

   //Compute global maximum for number of subcycles (collective operation)
   int maxSubcycles=0;
   int globalMaxSubcycles;
   for (size_t c=0; c<propagatedCells.size(); ++c) {
      const CellID cellID = propagatedCells[c];
      int subcycles = getAccerelationSubcycles(mpiGrid[cellID], dt);
      mpiGrid[cellID]->parameters[CellParams::ACCSUBCYCLES] = subcycles;
      maxSubcycles=maxSubcycles < subcycles ? subcycles:maxSubcycles;
   }
   MPI_Allreduce(&maxSubcycles, &globalMaxSubcycles, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

   // Substep global max times
   for(uint step = 0;step < globalMaxSubcycles; step ++) {
      //prune list of cells to propagate to only contained those which are now subcycled
      vector<CellID> temp;
      for (size_t c=0; c<propagatedCells.size(); ++c) {
         if(step < getAccerelationSubcycles(mpiGrid[propagatedCells[c]], dt)) {
            temp.push_back(propagatedCells[c]);
         }
      }
      propagatedCells.swap(temp);

      // Substep each population
      for (int popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
         calculateAcceleration(popID,globalMaxSubcycles,step,mpiGrid,propagatedCells,dt);
      }
   }
   //final adjust for all cells, also fixing remote cells.
   for (int popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
      adjustVelocityBlocks(mpiGrid, cells, true, popID);
   }
   phiprof::stop("semilag-acc");   

   // compute moments after acceleration
   phiprof::start("Compute moments");
   
   // Loop over particle populations, must be done before looping over 
   // spatial cells because setActivePopulation is static
   for (int popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
      #pragma omp parallel for
      for (size_t c=0; c<cells.size(); ++c) {
         const CellID cellID = cells[c];

         // clear old moments
         if (popID == 0) {
            mpiGrid[cellID]->parameters[CellParams::RHO_V  ] = 0.0;
            mpiGrid[cellID]->parameters[CellParams::RHOVX_V] = 0.0;
            mpiGrid[cellID]->parameters[CellParams::RHOVY_V] = 0.0;
            mpiGrid[cellID]->parameters[CellParams::RHOVZ_V] = 0.0;
            mpiGrid[cellID]->parameters[CellParams::P_11_V] = 0.0;
            mpiGrid[cellID]->parameters[CellParams::P_22_V] = 0.0;
            mpiGrid[cellID]->parameters[CellParams::P_33_V] = 0.0;
         }
      
         for (vmesh::LocalID block_i=0; block_i<mpiGrid[cellID]->get_number_of_velocity_blocks(popID); ++block_i) {
            cpu_calcVelocityFirstMoments(
               mpiGrid[cellID],
               block_i,
               CellParams::RHO_V,
               CellParams::RHOVX_V,
               CellParams::RHOVY_V,
               CellParams::RHOVZ_V,
               popID
            );   //set first moments after acceleration
         }
      }
   }

   // Second iteration needed as rho has to be already computed when computing pressure
   for (int popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
      #pragma omp parallel for
      for (size_t c=0; c<cells.size(); ++c) {
         const CellID cellID = cells[c];

         for (vmesh::LocalID block_i=0; block_i<mpiGrid[cellID]->get_number_of_velocity_blocks(popID); ++block_i) {
            cpu_calcVelocitySecondMoments(
               mpiGrid[cellID],
               block_i,
               CellParams::RHO_V,
               CellParams::RHOVX_V,
               CellParams::RHOVY_V,
               CellParams::RHOVZ_V,
               CellParams::P_11_V,
               CellParams::P_22_V,
               CellParams::P_33_V,
               popID
            );   //set second moments after acceleration
         }
      } // for-loop over spatial cells
   } // for-loop over species
   phiprof::stop("Compute moments");
}

/*--------------------------------------------------
  Functions for computing moments
  --------------------------------------------------*/

void calculateInterpolatedVelocityMoments(
   dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
   const int cp_rho,
   const int cp_rhovx,
   const int cp_rhovy,
   const int cp_rhovz,
   const int cp_p11,
   const int cp_p22,
   const int cp_p33
) {
   vector<CellID> cells;
   cells=mpiGrid.get_cells();
   
   //Iterate through all local cells (excl. system boundary cells):
#pragma omp parallel for
   for (size_t c=0; c<cells.size(); ++c) {
      const CellID cellID = cells[c];
      SpatialCell* SC = mpiGrid[cellID];
      if(SC->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY) {
         SC->parameters[cp_rho  ] = 0.5* ( SC->parameters[CellParams::RHO_R] + SC->parameters[CellParams::RHO_V] );
         SC->parameters[cp_rhovx] = 0.5* ( SC->parameters[CellParams::RHOVX_R] + SC->parameters[CellParams::RHOVX_V] );
         SC->parameters[cp_rhovy] = 0.5* ( SC->parameters[CellParams::RHOVY_R] + SC->parameters[CellParams::RHOVY_V] );
         SC->parameters[cp_rhovz] = 0.5* ( SC->parameters[CellParams::RHOVZ_R] + SC->parameters[CellParams::RHOVZ_V] );
         SC->parameters[cp_p11]   = 0.5* ( SC->parameters[CellParams::P_11_R] + SC->parameters[CellParams::P_11_V] );
         SC->parameters[cp_p22]   = 0.5* ( SC->parameters[CellParams::P_22_R] + SC->parameters[CellParams::P_22_V] );
         SC->parameters[cp_p33]   = 0.5* ( SC->parameters[CellParams::P_33_R] + SC->parameters[CellParams::P_33_V] );
      }
   }
}

void calculateCellVelocityMoments(
   SpatialCell* SC,
   bool doNotSkip // default: false
) {
   // if doNotSkip == true then the first clause is false and we will never return, i.e. always compute
   // otherwise we skip DO_NOT_COMPUTE cells
   // or boundary cells of layer larger than 1
   if (!doNotSkip &&
       (SC->sysBoundaryFlag == sysboundarytype::DO_NOT_COMPUTE ||
       (SC->sysBoundaryLayer != 1  &&
       SC->sysBoundaryFlag != sysboundarytype::NOT_SYSBOUNDARY))
      ) return;

   // Clear old moment values
   SC->parameters[CellParams::RHO  ] = 0.0;
   SC->parameters[CellParams::RHOVX] = 0.0;
   SC->parameters[CellParams::RHOVY] = 0.0;
   SC->parameters[CellParams::RHOVZ] = 0.0;
   SC->parameters[CellParams::P_11 ] = 0.0;  
   SC->parameters[CellParams::P_22 ] = 0.0;  
   SC->parameters[CellParams::P_33 ] = 0.0;  

   // Iterate over all populations and calculate the zeroth and first velocity moments
   for (int popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
      for (vmesh::LocalID blockLID=0; blockLID<SC->get_number_of_velocity_blocks(popID); ++blockLID) {
         cpu_blockVelocityFirstMoments(
            SC->get_data(blockLID,popID),
            SC->get_block_parameters(blockLID,popID),
            SC->parameters,
            CellParams::RHO,
            CellParams::RHOVX,
            CellParams::RHOVY,
            CellParams::RHOVZ
         );
      }
   }

   // Second iteration needed as rho has to be already computed when computing pressure
   for (int popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {      
      for (vmesh::LocalID blockLID=0; blockLID<SC->get_number_of_velocity_blocks(popID); ++blockLID) {
         cpu_blockVelocitySecondMoments(
            SC->get_data(blockLID,popID),
            SC->get_block_parameters(blockLID,popID),
            SC->parameters,
            CellParams::RHO,
            CellParams::RHOVX,
            CellParams::RHOVY,
            CellParams::RHOVZ,
            CellParams::P_11,
            CellParams::P_22,
            CellParams::P_33
         );
      }
   } // for-loop over populations
}

void calculateInitialVelocityMoments(dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid) {
   vector<CellID> cells;
   cells=mpiGrid.get_cells();
   phiprof::start("Calculate moments");
 
   // Iterate through all local cells (incl. system boundary cells):
   #pragma omp parallel for
   for (size_t c=0; c<cells.size(); ++c) {
      const CellID cellID = cells[c];
      SpatialCell* SC = mpiGrid[cellID];
      calculateCellVelocityMoments(SC);
      
      // WARNING the following is sane as this function is only called by initializeGrid.
      // We need initialized _DT2 values for the dt=0 field propagation done in the beginning.
      // Later these will be set properly.
      SC->parameters[CellParams::RHO_DT2] = SC->parameters[CellParams::RHO];
      SC->parameters[CellParams::RHOVX_DT2] = SC->parameters[CellParams::RHOVX];
      SC->parameters[CellParams::RHOVY_DT2] = SC->parameters[CellParams::RHOVY];
      SC->parameters[CellParams::RHOVZ_DT2] = SC->parameters[CellParams::RHOVZ];
      SC->parameters[CellParams::P_11_DT2] = SC->parameters[CellParams::P_11];
      SC->parameters[CellParams::P_22_DT2] = SC->parameters[CellParams::P_22];
      SC->parameters[CellParams::P_33_DT2] = SC->parameters[CellParams::P_33];
   } // for-loop over spatial cells
   phiprof::stop("Calculate moments"); 
}
