// ***************************************************************
// On the divergence-free condition in Godunov-type schemes for
// ideal magnetohydrodynamics: the upwind constrained transport method,
// P. Londrillo and L. Del Zanna, J. Comp. Phys., 195, 2004.
// http://dx.doi.org/10.1016/j.jcp.2003.09.016
//
// Reconstructions taken from:
// Efficient, high accuracy ADER-WENO schemes for hydrodynamics and
// divergence-free magnetohydrodynamics, D. S. Balsaraa, T. Rumpfa,
// M. Dumbserb, C.-D. Munzc, J. Comp. Phys, 228, 2480-2516, 2009.
// http://dx.doi.org/10.1016/j.jcp.2008.12.003
// and
// Divergence-free reconstruction of magnetic fields and WENO
// schemes for magnetohydrodynamics, D. S. Balsara, J. Comp. Phys.,
// 228, 5040-5056, 2009.
// http://dx.doi.org/10.1016/j.jcp.2009.03.038
// *****                                                     *****
// *****  NOTATION USED FOR VARIABLES FOLLOWS THE ONES USED  *****
// *****      IN THE ABOVEMENTIONED PUBLICATION(S)           *****
// ***************************************************************

#include <cstdlib>
#include <iostream>
#include <cmath>
#include <vector>
#include <map>
#include <list>
#include <set>

#include "../common.h"
#include "../parameters.h"
#include "../fieldsolver.h"
#include "../priorityqueue.h"
#include "../transferstencil.h"
#include "limiters.h"
#include "../project.h"

using namespace std;

#ifdef PARGRID
   typedef uint CellID;
#else
   #include <stdint.h>
   typedef uint64_t CellID;
#endif

static creal EPS = 1.0e-30;

static set<CellID> ghostCells;
static PriorityQueue<CellID> readyCells;      // Priority queue containing cell IDs that are ready to be computed

static map<CellID,uint> boundaryFlags;        // Boundary status flags for all cells on this process. Here "boundary cell" 
                                              // means that the cell is at the physical boundary of the simulation volume, 
					      // in some cases this condition means that this cell is a "ghost cell". However, 
					      // this is algorithm-dependent, so one must be careful with ghost cell definition.
					      // 
					      // Consider a cell and its immediate neighbours (26 in total), i.e. a 3x3 cube 
					      // of cells at base grid level. Considered cell is at the center of the cube. 
					      // Number the cells with the usual C array numbering, k*9+j*3+i, where i,j,k 
					      // are the cell indices in x,y,z directions. Each existing cell within the 
					      // 3x3 cube has its bit (calculated with C array indexing) flipped to value 1.
					      // The bit 13 is always set to unit value (considered cell always exists).
					      // 
					      // These boundary flags can be used to determine whether a numerical algorithm 
					      // should be applied to a cell, for example, to calculate an edge electric field.
					      // The boundary status can be checked with a single bitwise operation instead of 
					      // N if-statements.
					      // 
					      // Note that this definition works with mesh refinement. The boundary flag 
					      // should only change for a cell if some of its neighbours are deleted or 
					      // created during the simulation.

static TransferStencil<CellID> stencil1(INVALID_CELLID);     // MPI-stencil used to receive data for derivatives & edge-E calculation
static TransferStencil<CellID> stencil2(INVALID_CELLID);     // MPI-stencil used to receive data for propagation of B
static TransferStencil<CellID> stencil3(INVALID_CELLID);

static uint CALCULATE_DX; /**< Bit mask determining if x-derivatives can be calculated on a cell.*/
static uint CALCULATE_DY; /**< Bit mask determining if y-derivatives can be calculated on a cell.*/
static uint CALCULATE_DZ; /**< Bit mask determining if z-derivatives can be calculated on a cell.*/
static uint CALCULATE_EX; /**< Bit mask determining if edge Ex can be calculated on a cell.*/
static uint CALCULATE_EY; /**< Bit mask determining if edge Ey can be calculated on a cell.*/
static uint CALCULATE_EZ; /**< Bit mask determining if edge Ez can be calculated on a cell.*/
static uint PROPAGATE_BX; /**< Bit mask determining if face Bx is propagated on a cell.*/
static uint PROPAGATE_BY; /**< Bit mask determining if face By is propagated on a cell.*/
static uint PROPAGATE_BZ; /**< Bit mask determining if face Bz is propagated on a cell.*/

// Constants: not needed as such, but if field solver is implemented on GPUs 
// these force CPU to use float accuracy, which in turn helps to compare 
// CPU and GPU results.
creal HALF    = 0.5;
creal MINUS   = -1.0;
creal PLUS    = +1.0;
creal EIGTH   = 1.0/8.0;
creal FOURTH  = 1.0/4.0;
creal SIXTH   = 1.0/6.0;
creal TWELWTH = 1.0/12.0;
creal TWO     = 2.0;
creal ZERO    = 0.0;

#ifdef PARGRID
void calculateDerivativesSimple(ParGrid<SpatialCell>& mpiGrid,const vector<CellID>& localCells);
void calculateUpwindedElectricFieldSimple(ParGrid<SpatialCell>& mpiGrid,const vector<CellID>& localCells);
//void calculateFaceAveragedFields(ParGrid<SpatialCell>& mpiGrid);
#else

#endif

/** Calculate the neighbour number. For the inspected cell the (i,j,k) are (1,1,1). Add or 
 * reduce one from an index to get the "neighbour number" for the neighbour in that direction. 
 * For example, neighbour number for i-1,j-1,k neighbour is calculated with calcNbrNumber(1-1,1-1,1+0).
 * Thus, the cell in question has a "neighbour number" 13.
 * The purpose of this function (and neighbour numbers) is to indicate whether a cell has 
 * existing neighbours on a given direction. The neighbour existence status can be stored in 
 * a single 32bit word and tested with bitwise operations.
 */
inline uchar calcNbrNumber(const uchar& i,const uchar& j,const uchar& k) {return k*9+j*3+i;}

inline uchar calcNbrTypeID(const uchar& i,const uchar& j,const uchar& k) {return k*25+j*5+i;}

Real limiter(creal& left,creal& cent,creal& rght) {
   //return minmod(left,cent,rght);
   return MClimiter(left,cent,rght);
   //return vanLeer(left,cent,rght);
}

#ifdef PARGRID

CellID getNeighbourID(ParGrid<SpatialCell>& mpiGrid,const CellID& cellID,const uchar& i,const uchar& j,const uchar& k) {
   const uchar nbrTypeID = calcNbrTypeID(i,j,k);
   return mpiGrid.getNeighbour(cellID,nbrTypeID);
}

static void calculateBoundaryFlags(ParGrid<SpatialCell>& mpiGrid,const vector<CellID>& localCells) {
   for (size_t cell=0; cell<localCells.size(); ++cell) {
      const CellID cellID = localCells[cell];
      
      // Raise the bit for each existing cell within a 3x3 cube of 
      // spatial cells. This cell sits at the center of the cube.
      uint boundaryFlag = (1 << calcNbrNumber(1,1,1)); // The cell itself exists (bit 13 set to 1)
      
      for (int k=-1; k<2; ++k) for (int j=-1; j<2; ++j) for (int i=-1; i<2; ++i) {
	 if (i == 0 && (j == 0 && k == 0)) continue;
	 const CellID nbr = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+i,2+j,2+k));
	 if (nbr == INVALID_CELLID) continue;
	 boundaryFlag = boundaryFlag | (1 << calcNbrNumber(1+i,1+j,1+k));
      }
      boundaryFlags[cellID] = boundaryFlag;
   }
}

static void calculateDerivatives(const CellID& cellID,ParGrid<SpatialCell>& mpiGrid) {
   namespace cp = CellParams;
   namespace fs = fieldsolver;
   Real* const array       = mpiGrid[cellID]->cpu_derivatives;
   Real* const derivatives = array;
   
   // Get boundary flag for the cell:
   #ifndef NDEBUG
      map<CellID,uint>::const_iterator it = boundaryFlags.find(cellID);
      if (it == boundaryFlags.end()) {cerr << "ERROR Could not find boundary flag for cell #" << cellID << endl; exit(1);}
      cuint existingCells = it->second;
   #else
      cuint existingCells = boundaryFlags[cellID];
   #endif
   cuint nonExistingCells = (existingCells ^ numeric_limits<uint>::max());
   
   CellID leftNbrID,rghtNbrID;
   creal* left = NULL;
   creal* cent = mpiGrid[cellID   ]->cpu_cellParams;
   creal* rght = NULL;
   // Calculate x-derivatives (is not TVD for AMR mesh):
   if ((existingCells & CALCULATE_DX) == CALCULATE_DX) {
      leftNbrID = getNeighbourID(mpiGrid,cellID,2-1,2  ,2  );
      rghtNbrID = getNeighbourID(mpiGrid,cellID,2+1,2  ,2  );
      left = mpiGrid[leftNbrID]->cpu_cellParams;
      rght = mpiGrid[rghtNbrID]->cpu_cellParams;
      array[fs::drhodx] = limiter(left[cp::RHO],cent[cp::RHO],rght[cp::RHO]);
      array[fs::dBydx]  = limiter(left[cp::BY],cent[cp::BY],rght[cp::BY]);
      array[fs::dBzdx]  = limiter(left[cp::BZ],cent[cp::BZ],rght[cp::BZ]);
      array[fs::dVxdx]  = limiter(left[cp::RHOVX]/left[cp::RHO],cent[cp::RHOVX]/cent[cp::RHO],rght[cp::RHOVX]/rght[cp::RHO]);
      array[fs::dVydx]  = limiter(left[cp::RHOVY]/left[cp::RHO],cent[cp::RHOVY]/cent[cp::RHO],rght[cp::RHOVY]/rght[cp::RHO]);
      array[fs::dVzdx]  = limiter(left[cp::RHOVZ]/left[cp::RHO],cent[cp::RHOVZ]/cent[cp::RHO],rght[cp::RHOVZ]/rght[cp::RHO]);
   } else {
      fieldSolverBoundaryCondDerivX(cellID,array,existingCells,nonExistingCells,derivatives,mpiGrid);
   }
   
   // Calculate y-derivatives (is not TVD for AMR mesh):
   if ((existingCells & CALCULATE_DY) == CALCULATE_DY) {
      leftNbrID = getNeighbourID(mpiGrid,cellID,2  ,2-1,2  );
      rghtNbrID = getNeighbourID(mpiGrid,cellID,2  ,2+1,2  );
      left = mpiGrid[leftNbrID]->cpu_cellParams;
      rght = mpiGrid[rghtNbrID]->cpu_cellParams;
      array[fs::drhody] = limiter(left[cp::RHO],cent[cp::RHO],rght[cp::RHO]);
      array[fs::dBxdy]  = limiter(left[cp::BX],cent[cp::BX],rght[cp::BX]);
      array[fs::dBzdy]  = limiter(left[cp::BZ],cent[cp::BZ],rght[cp::BZ]);
      array[fs::dVxdy]  = limiter(left[cp::RHOVX]/left[cp::RHO],cent[cp::RHOVX]/cent[cp::RHO],rght[cp::RHOVX]/rght[cp::RHO]);
      array[fs::dVydy]  = limiter(left[cp::RHOVY]/left[cp::RHO],cent[cp::RHOVY]/cent[cp::RHO],rght[cp::RHOVY]/rght[cp::RHO]);
      array[fs::dVzdy]  = limiter(left[cp::RHOVZ]/left[cp::RHO],cent[cp::RHOVZ]/cent[cp::RHO],rght[cp::RHOVZ]/rght[cp::RHO]);
   } else {
      fieldSolverBoundaryCondDerivY(cellID,array,existingCells,nonExistingCells,derivatives,mpiGrid);
   }
   
   // Calculate z-derivatives (is not TVD for AMR mesh):
   if ((existingCells & CALCULATE_DZ) == CALCULATE_DZ) {
      leftNbrID = getNeighbourID(mpiGrid,cellID,2  ,2  ,2-1);
      rghtNbrID = getNeighbourID(mpiGrid,cellID,2  ,2  ,2+1);
      left = mpiGrid[leftNbrID]->cpu_cellParams;
      rght = mpiGrid[rghtNbrID]->cpu_cellParams;
      array[fs::drhodz] = limiter(left[cp::RHO],cent[cp::RHO],rght[cp::RHO]);
      array[fs::dBxdz]  = limiter(left[cp::BX],cent[cp::BX],rght[cp::BX]);
      array[fs::dBydz]  = limiter(left[cp::BY],cent[cp::BY],rght[cp::BY]);
      array[fs::dVxdz]  = limiter(left[cp::RHOVX]/left[cp::RHO],cent[cp::RHOVX]/cent[cp::RHO],rght[cp::RHOVX]/rght[cp::RHO]);
      array[fs::dVydz]  = limiter(left[cp::RHOVY]/left[cp::RHO],cent[cp::RHOVY]/cent[cp::RHO],rght[cp::RHOVY]/rght[cp::RHO]);
      array[fs::dVzdz]  = limiter(left[cp::RHOVZ]/left[cp::RHO],cent[cp::RHOVZ]/cent[cp::RHO],rght[cp::RHOVZ]/rght[cp::RHO]);
   } else {
      fieldSolverBoundaryCondDerivZ(cellID,array,existingCells,nonExistingCells,derivatives,mpiGrid);
   }
}

template<typename REAL> REAL calculateFastMSspeedYZ(const REAL* cp,const REAL* derivs,const REAL* nbr_cp,const REAL* nbr_derivs,
						    const REAL& By,const REAL& Bz,
						    const REAL& dBydx,const REAL& dBydz,const REAL& dBzdx,const REAL& dBzdy,
						    const REAL& ydir,const REAL& zdir) {
   namespace fs = fieldsolver;
   namespace pc = physicalconstants;

   const REAL A_0  = HALF*(nbr_cp[CellParams::BX] + cp[CellParams::BX]);
   const REAL A_X  = nbr_cp[CellParams::BX] - cp[CellParams::BX];
   const REAL A_Y  = nbr_derivs[fs::dBxdy]  + derivs[fs::dBxdy];
   const REAL A_XY = nbr_derivs[fs::dBxdy]  - derivs[fs::dBxdy];
   const REAL A_Z  = nbr_derivs[fs::dBxdz]  + derivs[fs::dBxdz];
   const REAL A_XZ = nbr_derivs[fs::dBxdz]  - derivs[fs::dBxdz];
   
   const REAL Bx2  = (A_0 + ydir*HALF*A_Y + zdir*HALF*A_Z)*(A_0 + ydir*HALF*A_Y + zdir*HALF*A_Z)
     + TWELWTH*(A_X + ydir*HALF*A_XY + zdir*HALF*A_XZ)*(A_X + ydir*HALF*A_XY + zdir*HALF*A_XZ); // OK
   const REAL By2  = (By + zdir*HALF*dBydz)*(By + zdir*HALF*dBydz) + TWELWTH*dBydx*dBydx; // OK
   const REAL Bz2  = (Bz + ydir*HALF*dBzdy)*(Bz + ydir*HALF*dBzdy) + TWELWTH*dBzdx*dBzdx; // OK
   
   const REAL rho = Parameters::m*(cp[CellParams::RHO] + ydir*HALF*derivs[fs::drhody] + zdir*HALF*derivs[fs::drhodz]);   
   return sqrt((Bx2+By2+Bz2) / (pc::MU_0 * rho));
}

template<typename REAL> REAL calculateFastMSspeedXZ(const REAL* cp,const REAL* derivs,const REAL* nbr_cp,const REAL* nbr_derivs,
						    const REAL& Bx,const REAL& Bz,
						    const REAL& dBxdy,const REAL& dBxdz,const REAL& dBzdx,const REAL& dBzdy,
						    const REAL& xdir,const REAL& zdir) {
   namespace fs = fieldsolver;
   namespace pc = physicalconstants;
   
   const REAL B_0  = HALF*(nbr_cp[CellParams::BY] + cp[CellParams::BY]);
   const REAL B_Y  = nbr_cp[CellParams::BY] - cp[CellParams::BY];
   const REAL B_X  = nbr_derivs[fs::dBydx]  + derivs[fs::dBydx];
   const REAL B_XY = nbr_derivs[fs::dBydx]  - derivs[fs::dBydx];
   const REAL B_Z  = nbr_derivs[fs::dBydz]  + derivs[fs::dBydz];
   const REAL B_YZ = nbr_derivs[fs::dBydz]  - derivs[fs::dBydz];
   
   const REAL By2  = (B_0 + xdir*HALF*B_X + zdir*HALF*B_Z)*(B_0 + xdir*HALF*B_X + zdir*HALF*B_Z)
     + TWELWTH*(B_Y + xdir*HALF*B_XY + zdir*HALF*B_YZ)*(B_Y + xdir*HALF*B_XY + zdir*HALF*B_YZ); // OK
   const REAL Bx2  = (Bx + zdir*HALF*dBxdz)*(Bx + zdir*HALF*dBxdz) + TWELWTH*dBxdy*dBxdy; // OK
   const REAL Bz2  = (Bz + xdir*HALF*dBzdx)*(Bz + xdir*HALF*dBzdx) + TWELWTH*dBzdy*dBzdy; // OK
   
   const REAL rho = Parameters::m*(cp[CellParams::RHO] + xdir*HALF*derivs[fs::drhodx] + zdir*HALF*derivs[fs::drhodz]);
   return sqrt((Bx2+By2+Bz2) / (pc::MU_0 * rho));
}

template<typename REAL> REAL calculateFastMSspeedXY(const REAL* cp,const REAL* derivs,const REAL* nbr_cp,const REAL* nbr_derivs,
						    const REAL& Bx,const REAL& By,
						    const REAL& dBxdy,const REAL& dBxdz,const REAL& dBydx,const REAL& dBydz,
						    const REAL& xdir,const REAL& ydir) {
   namespace fs = fieldsolver;
   namespace pc = physicalconstants;
   
   const REAL C_0  = HALF*(nbr_cp[CellParams::BZ] + cp[CellParams::BZ]);
   const REAL C_Z  = nbr_cp[CellParams::BZ] - cp[CellParams::BZ];
   const REAL C_X  = nbr_derivs[fs::dBzdx]  + derivs[fs::dBzdx];
   const REAL C_XZ = nbr_derivs[fs::dBzdx]  - derivs[fs::dBzdx];
   const REAL C_Y  = nbr_derivs[fs::dBzdy]  + derivs[fs::dBzdy];
   const REAL C_YZ = nbr_derivs[fs::dBzdy]  - derivs[fs::dBzdy];
   
   const REAL Bz2  = (C_0 + xdir*HALF*C_X + ydir*HALF*C_Y)*(C_0 + xdir*HALF*C_X + ydir*HALF*C_Y)
     + TWELWTH*(C_Z + xdir*HALF*C_XZ + ydir*HALF*C_YZ)*(C_Z + xdir*HALF*C_XZ + ydir*HALF*C_YZ);
   const REAL Bx2  = (Bx + ydir*HALF*dBxdy)*(Bx + ydir*HALF*dBxdy) + TWELWTH*dBxdz*dBxdz;
   const REAL By2  = (By + xdir*HALF*dBydx)*(By + xdir*HALF*dBydx) + TWELWTH*dBydz*dBydz;
   
   const REAL rho = Parameters::m*(cp[CellParams::RHO] + xdir*HALF*derivs[fs::drhodx] + ydir*HALF*derivs[fs::drhody]);
   
   return sqrt((Bx2+By2+Bz2) / (pc::MU_0 * rho));
}

static void calculateEdgeElectricFieldX(const CellID& cellID,ParGrid<SpatialCell>& mpiGrid) {
   namespace fs = fieldsolver;
   
   // An edge has four neighbouring spatial cells. Calculate
   // electric field in each of the four cells per edge.   
   Real ay_pos,ay_neg;              // Max. characteristic velocities to x-direction
   Real az_pos,az_neg;              // Max. characteristic velocities to y-direction
   Real Vy0,Vz0;                    // Reconstructed V
   Real c_y, c_z;                   // Wave speeds to yz-directions

   // Get read-only pointers to NE,NW,SE,SW states (SW is rw, result is written there):
   const CellID nbr_SE = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2-1,2  ));
   const CellID nbr_NE = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2-1,2-1));
   const CellID nbr_NW = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2  ,2-1));
   #ifndef NDEBUG
      if (nbr_SE == INVALID_CELLID) {cerr << "ERROR: Could not find SE neighbour!" << endl; exit(1);}
      if (nbr_NE == INVALID_CELLID) {cerr << "ERROR: Could not find NE neighbour!" << endl; exit(1);}
      if (nbr_NW == INVALID_CELLID) {cerr << "ERROR: Could not find NW neighbour!" << endl; exit(1);}
   #endif
   
   Real*  const cp_SW = mpiGrid[cellID]->cpu_cellParams;
   creal* const cp_SE = mpiGrid[nbr_SE]->cpu_cellParams;
   creal* const cp_NE = mpiGrid[nbr_NE]->cpu_cellParams;
   creal* const cp_NW = mpiGrid[nbr_NW]->cpu_cellParams;
   
   creal* const derivs_SW = mpiGrid[cellID]->cpu_derivatives;
   creal* const derivs_SE = mpiGrid[nbr_SE]->cpu_derivatives;
   creal* const derivs_NE = mpiGrid[nbr_NE]->cpu_derivatives;
   creal* const derivs_NW = mpiGrid[nbr_NW]->cpu_derivatives;
   
   creal By_S = cp_SW[CellParams::BY];
   creal Bz_W = cp_SW[CellParams::BZ];
   creal Bz_E = cp_SE[CellParams::BZ];
   creal By_N = cp_NW[CellParams::BY];
   
   creal dBydx_S = derivs_SW[fs::dBydx];
   creal dBydz_S = derivs_SW[fs::dBydz];
   creal dBzdx_W = derivs_SW[fs::dBzdx];
   creal dBzdy_W = derivs_SW[fs::dBzdy];
   creal dBzdx_E = derivs_SE[fs::dBzdx];
   creal dBzdy_E = derivs_SE[fs::dBzdy];
   creal dBydx_N = derivs_NW[fs::dBydx];
   creal dBydz_N = derivs_NW[fs::dBydz];
   
   // Ex and characteristic speeds on this cell:
   Vy0  = cp_SW[CellParams::RHOVY]/cp_SW[CellParams::RHO];
   Vz0  = cp_SW[CellParams::RHOVZ]/cp_SW[CellParams::RHO];

   // 1st order terms:
   Real Ex_SW = By_S*Vz0 - Bz_W*Vy0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ex_SW += +HALF*((By_S - HALF*dBydz_S)*(-derivs_SW[fs::dVzdy] - derivs_SW[fs::dVzdz]) - dBydz_S*Vz0 + SIXTH*dBydx_S*derivs_SW[fs::dVzdx]);
      Ex_SW += -HALF*((Bz_W - HALF*dBzdy_W)*(-derivs_SW[fs::dVydy] - derivs_SW[fs::dVydz]) - dBzdy_W*Vy0 + SIXTH*dBzdx_W*derivs_SW[fs::dVydx]);
   #endif

   const CellID nbrID_SW      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2  ,2  ));
   creal* const nbr_cp_SW     = mpiGrid[nbrID_SW]->cpu_cellParams;
   creal* const nbr_derivs_SW = mpiGrid[nbrID_SW]->cpu_derivatives;
   c_y = calculateFastMSspeedYZ(cp_SW,derivs_SW,nbr_cp_SW,nbr_derivs_SW,By_S,Bz_W,dBydx_S,dBydz_S,dBzdx_W,dBzdy_W,MINUS,MINUS);
   c_z = c_y;
   ay_neg   = max(ZERO,-Vy0 + c_y);
   ay_pos   = max(ZERO,+Vy0 + c_y);
   az_neg   = max(ZERO,-Vz0 + c_z);
   az_pos   = max(ZERO,+Vz0 + c_z);
   
   // Ex and characteristic speeds on j-1 neighbour:
   Vy0  = cp_SE[CellParams::RHOVY]/cp_SE[CellParams::RHO];
   Vz0  = cp_SE[CellParams::RHOVZ]/cp_SE[CellParams::RHO];

   // 1st order terms:
   Real Ex_SE = By_S*Vz0 - Bz_E*Vy0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ex_SE += +HALF*((By_S - HALF*dBydz_S)*(+derivs_SE[fs::dVzdy] - derivs_SE[fs::dVzdz]) - dBydz_S*Vz0 + SIXTH*dBydx_S*derivs_SE[fs::dVzdx]);
      Ex_SE += -HALF*((Bz_E + HALF*dBzdy_E)*(+derivs_SE[fs::dVydy] - derivs_SE[fs::dVydz]) + dBzdy_E*Vy0 + SIXTH*dBzdx_E*derivs_SE[fs::dVydx]);
   #endif
   
   const CellID nbrID_SE      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2-1,2  ));
   creal* const nbr_cp_SE     = mpiGrid[nbrID_SE]->cpu_cellParams;
   creal* const nbr_derivs_SE = mpiGrid[nbrID_SE]->cpu_derivatives;
   c_y = calculateFastMSspeedYZ(cp_SE,derivs_SE,nbr_cp_SE,nbr_derivs_SE,By_S,Bz_E,dBydx_S,dBydz_S,dBzdx_E,dBzdy_E,PLUS,MINUS);
   c_z = c_y;
   ay_neg   = max(ay_neg,-Vy0 + c_y);
   ay_pos   = max(ay_pos,+Vy0 + c_y);
   az_neg   = max(az_neg,-Vz0 + c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);
   
   // Ex and characteristic speeds on k-1 neighbour:
   Vy0  = cp_NW[CellParams::RHOVY]/cp_NW[CellParams::RHO];
   Vz0  = cp_NW[CellParams::RHOVZ]/cp_NW[CellParams::RHO];
   
   // 1st order terms:
   Real Ex_NW    = By_N*Vz0 - Bz_W*Vy0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ex_NW += +HALF*((By_N + HALF*dBydz_N)*(-derivs_NW[fs::dVzdy] + derivs_NW[fs::dVzdz]) + dBydz_N*Vz0 + SIXTH*dBydx_N*derivs_NW[fs::dVzdx]);
      Ex_NW += -HALF*((Bz_W - HALF*dBzdy_W)*(-derivs_NW[fs::dVydy] + derivs_NW[fs::dVydz]) - dBzdy_W*Vy0 + SIXTH*dBzdx_W*derivs_NW[fs::dVydx]);
   #endif
   
   const CellID nbrID_NW      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2  ,2-1));
   creal* const nbr_cp_NW     = mpiGrid[nbrID_NW]->cpu_cellParams;
   creal* const nbr_derivs_NW = mpiGrid[nbrID_NW]->cpu_derivatives;
   c_y = calculateFastMSspeedYZ(cp_NW,derivs_NW,nbr_cp_NW,nbr_derivs_NW,By_N,Bz_W,dBydx_N,dBydz_N,dBzdx_W,dBzdy_W,MINUS,PLUS);
   c_z = c_y;
   ay_neg   = max(ay_neg,-Vy0 + c_y);
   ay_pos   = max(ay_pos,+Vy0 + c_y);
   az_neg   = max(az_neg,-Vz0 + c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);
   
   // Ex and characteristic speeds on j-1,k-1 neighbour:
   Vy0 = cp_NE[CellParams::RHOVY]/cp_NE[CellParams::RHO];
   Vz0 = cp_NE[CellParams::RHOVZ]/cp_NE[CellParams::RHO];
   
   // 1st order terms:
   Real Ex_NE    = By_N*Vz0 - Bz_E*Vy0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ex_NE += +HALF*((By_N + HALF*dBydz_N)*(+derivs_NE[fs::dVzdy] + derivs_NE[fs::dVzdz]) + dBydz_N*Vz0 + SIXTH*dBydx_N*derivs_NE[fs::dVzdx]);
      Ex_NE += -HALF*((Bz_E + HALF*dBzdy_E)*(+derivs_NE[fs::dVydy] + derivs_NE[fs::dVydz]) + dBzdy_E*Vy0 + SIXTH*dBzdx_E*derivs_NE[fs::dVydx]);
   #endif
   
   const CellID nbrID_NE      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2-1,2-1));
   creal* const nbr_cp_NE     = mpiGrid[nbrID_NE]->cpu_cellParams;
   creal* const nbr_derivs_NE = mpiGrid[nbrID_NE]->cpu_derivatives;
   c_y = calculateFastMSspeedYZ(cp_NE,derivs_NE,nbr_cp_NE,nbr_derivs_NE,By_N,Bz_E,dBydx_N,dBydz_N,dBzdx_E,dBzdy_E,PLUS,PLUS);
   c_z = c_y;
   ay_neg   = max(ay_neg,-Vy0 + c_y);
   ay_pos   = max(ay_pos,+Vy0 + c_y);
   az_neg   = max(az_neg,-Vz0 + c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);

   // Calculate properly upwinded edge-averaged Ex:
   cp_SW[CellParams::EX]  = ay_pos*az_pos*Ex_NE + ay_pos*az_neg*Ex_SE + ay_neg*az_pos*Ex_NW + ay_neg*az_neg*Ex_SW;
   cp_SW[CellParams::EX] /= ((ay_pos+ay_neg)*(az_pos+az_neg)+EPS);
   #ifdef FS_1ST_ORDER
      // 1st order diffusive terms:
      cp_SW[CellParams::EX] -= az_pos*az_neg/(az_pos+az_neg+EPS)*(By_S-By_N);
      cp_SW[CellParams::EX] += ay_pos*ay_neg/(ay_pos+ay_neg+EPS)*(Bz_W-Bz_E);
   #else
      // 2nd order diffusive terms
      cp_SW[CellParams::EX] -= az_pos*az_neg/(az_pos+az_neg+EPS)*((By_S-HALF*dBydz_S) - (By_N+HALF*dBydz_N));
      cp_SW[CellParams::EX] += ay_pos*ay_neg/(ay_pos+ay_neg+EPS)*((Bz_W-HALF*dBzdy_W) - (Bz_E+HALF*dBzdy_E));
   #endif
}

static void calculateEdgeElectricFieldY(const CellID& cellID,ParGrid<SpatialCell>& mpiGrid) {
   // An edge has four neighbouring spatial cells. Calculate
   // electric field in each of the four cells per edge. 
   namespace fs = fieldsolver;
   
   Real ax_pos,ax_neg;              // Max. characteristic velocities to x-direction
   Real az_pos,az_neg;              // Max. characteristic velocities to y-direction
   Real Vx0,Vz0;                    // Reconstructed V
   Real c_x,c_z;                    // Wave speeds to xz-directions
   
   // Get read-only pointers to NE,NW,SE,SW states (SW is rw, result is written there):
   const CellID nbr_SE = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2  ,2-1));
   const CellID nbr_NW = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2  ,2  ));
   const CellID nbr_NE = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2  ,2-1));
   #ifndef NDEBUG
      if (nbr_SE == INVALID_CELLID) {cerr << "ERROR: Could not find SE neighbour!" << endl; exit(1);}
      if (nbr_NE == INVALID_CELLID) {cerr << "ERROR: Could not find NE neighbour!" << endl; exit(1);}
      if (nbr_NW == INVALID_CELLID) {cerr << "ERROR: Could not find NW neighbour!" << endl; exit(1);}
   #endif
   
   Real* const  cp_SW = mpiGrid[cellID]->cpu_cellParams;
   creal* const cp_SE = mpiGrid[nbr_SE]->cpu_cellParams;
   creal* const cp_NE = mpiGrid[nbr_NE]->cpu_cellParams;
   creal* const cp_NW = mpiGrid[nbr_NW]->cpu_cellParams;
   
   creal* const derivs_SW = mpiGrid[cellID]->cpu_derivatives;
   creal* const derivs_SE = mpiGrid[nbr_SE]->cpu_derivatives;
   creal* const derivs_NE = mpiGrid[nbr_NE]->cpu_derivatives;
   creal* const derivs_NW = mpiGrid[nbr_NW]->cpu_derivatives;
   
   // Fetch required plasma parameters:
   creal Bz_S = cp_SW[CellParams::BZ];
   creal Bx_W = cp_SW[CellParams::BX];
   creal Bx_E = cp_SE[CellParams::BX];
   creal Bz_N = cp_NW[CellParams::BZ];
   
   creal dBxdy_W = derivs_SW[fs::dBxdy];
   creal dBxdz_W = derivs_SW[fs::dBxdz];
   creal dBzdx_S = derivs_SW[fs::dBzdx];
   creal dBzdy_S = derivs_SW[fs::dBzdy];
   creal dBxdy_E = derivs_SE[fs::dBxdy];
   creal dBxdz_E = derivs_SE[fs::dBxdz];
   creal dBzdx_N = derivs_NW[fs::dBzdx];
   creal dBzdy_N = derivs_NW[fs::dBzdy];
   
   // Ey and characteristic speeds on this cell:
   Vx0  = cp_SW[CellParams::RHOVX]/cp_SW[CellParams::RHO];
   Vz0  = cp_SW[CellParams::RHOVZ]/cp_SW[CellParams::RHO];
   
   // 1st order terms:
   Real Ey_SW  = Bz_S*Vx0 - Bx_W*Vz0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms
      Ey_SW += +HALF*((Bz_S - HALF*dBzdx_S)*(-derivs_SW[fs::dVxdx] - derivs_SW[fs::dVxdz]) - dBzdx_S*Vx0 + SIXTH*dBzdy_S*derivs_SW[fs::dVxdy]);
      Ey_SW += -HALF*((Bx_W - HALF*dBxdz_W)*(-derivs_SW[fs::dVzdx] - derivs_SW[fs::dVzdz]) - dBxdz_W*Vz0 + SIXTH*dBxdy_W*derivs_SW[fs::dVzdy]);
   #endif
   
   const CellID nbrID_SW      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2+1,2  ));
   creal* const nbr_cp_SW     = mpiGrid[nbrID_SW]->cpu_cellParams;
   creal* const nbr_derivs_SW = mpiGrid[nbrID_SW]->cpu_derivatives;
   c_z = calculateFastMSspeedXZ(cp_SW,derivs_SW,nbr_cp_SW,nbr_derivs_SW,Bx_W,Bz_S,dBxdy_W,dBxdz_W,dBzdx_S,dBzdy_S,MINUS,MINUS);
   c_x = c_z;
   az_neg   = max(ZERO,-Vz0 + c_z);
   az_pos   = max(ZERO,+Vz0 + c_z);
   ax_neg   = max(ZERO,-Vx0 + c_x);
   ax_pos   = max(ZERO,+Vx0 + c_x);
   
   // Ey and characteristic speeds on k-1 neighbour:
   Vx0  = cp_SE[CellParams::RHOVX]/cp_SE[CellParams::RHO];
   Vz0  = cp_SE[CellParams::RHOVZ]/cp_SE[CellParams::RHO];

   // 1st order terms:
   Real Ey_SE    = Bz_S*Vx0 - Bx_E*Vz0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ey_SE += +HALF*((Bz_S - HALF*dBzdx_S)*(-derivs_SE[fs::dVxdx] + derivs_SE[fs::dVxdz]) - dBzdx_S*Vx0 + SIXTH*dBzdy_S*derivs_SE[fs::dVxdy]);
      Ey_SE += -HALF*((Bx_E + HALF*dBxdz_E)*(-derivs_SE[fs::dVzdx] + derivs_SE[fs::dVzdz]) + dBxdz_E*Vz0 + SIXTH*dBxdy_E*derivs_SE[fs::dVzdy]);
   #endif

   const CellID nbrID_SE      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2+1,2-1));
   creal* const nbr_cp_SE     = mpiGrid[nbrID_SE]->cpu_cellParams;
   creal* const nbr_derivs_SE = mpiGrid[nbrID_SE]->cpu_derivatives;
   c_z = calculateFastMSspeedXZ(cp_SE,derivs_SE,nbr_cp_SE,nbr_derivs_SE,Bx_E,Bz_S,dBxdy_E,dBxdz_E,dBzdx_S,dBzdy_S,MINUS,PLUS);
   c_x = c_z;
   az_neg   = max(az_neg,-Vz0 - c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);
   ax_neg   = max(ax_neg,-Vx0 - c_x);
   ax_pos   = max(ax_pos,+Vx0 + c_x);
   
   // Ey and characteristic speeds on i-1 neighbour:
   Vz0  = cp_NW[CellParams::RHOVZ]/cp_NW[CellParams::RHO];
   Vx0  = cp_NW[CellParams::RHOVX]/cp_NW[CellParams::RHO];
   
   // 1st order terms:
   Real Ey_NW    = Bz_N*Vx0 - Bx_W*Vz0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ey_NW += +HALF*((Bz_N + HALF*dBzdx_N)*(+derivs_NW[fs::dVxdx] - derivs_NW[fs::dVxdz]) + dBzdx_N*Vx0 + SIXTH*dBzdy_N*derivs_NW[fs::dVxdy]);
      Ey_NW += -HALF*((Bx_W - HALF*dBxdz_W)*(+derivs_NW[fs::dVzdx] - derivs_NW[fs::dVzdz]) - dBxdz_W*Vz0 + SIXTH*dBxdy_W*derivs_NW[fs::dVzdy]);
   #endif
   
   const CellID nbrID_NW      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2+1,2  ));
   creal* const nbr_cp_NW     = mpiGrid[nbrID_NW]->cpu_cellParams;
   creal* const nbr_derivs_NW = mpiGrid[nbrID_NW]->cpu_derivatives;
   c_z = calculateFastMSspeedXZ(cp_NW,derivs_NW,nbr_cp_NW,nbr_derivs_NW,Bx_W,Bz_N,dBxdy_W,dBxdz_W,dBzdx_N,dBzdy_N,PLUS,MINUS);
   c_x = c_z;
   az_neg   = max(az_neg,-Vz0 + c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);
   ax_neg   = max(ax_neg,-Vx0 + c_x);
   ax_pos   = max(ax_pos,+Vx0 + c_x);
   
   // Ey and characteristic speeds on i-1,k-1 neighbour:
   Vz0 = cp_NE[CellParams::RHOVZ]/cp_NE[CellParams::RHO];
   Vx0 = cp_NE[CellParams::RHOVX]/cp_NE[CellParams::RHO];

   // 1st order terms:
   Real Ey_NE    = Bz_N*Vx0 - Bx_E*Vz0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ey_NE += +HALF*((Bz_N + HALF*dBzdx_N)*(+derivs_NE[fs::dVxdx] + derivs_NE[fs::dVxdz]) + dBzdx_N*Vx0 + SIXTH*dBzdy_N*derivs_NE[fs::dVxdy]);
      Ey_NE += -HALF*((Bx_E + HALF*dBxdz_E)*(+derivs_NE[fs::dVzdx] + derivs_NE[fs::dVzdz]) + dBxdz_E*Vz0 + SIXTH*dBxdy_E*derivs_NE[fs::dVzdy]);
   #endif
   
   const CellID nbrID_NE      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2+1,2-1));
   creal* const nbr_cp_NE     = mpiGrid[nbrID_NE]->cpu_cellParams;
   creal* const nbr_derivs_NE = mpiGrid[nbrID_NE]->cpu_derivatives;
   c_z = calculateFastMSspeedXZ(cp_NE,derivs_NE,nbr_cp_NE,nbr_derivs_NE,Bx_E,Bz_N,dBxdy_E,dBxdz_E,dBzdx_N,dBzdy_N,PLUS,PLUS);
   c_x = c_z;
   az_neg   = max(az_neg,-Vz0 + c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);
   ax_neg   = max(ax_neg,-Vx0 + c_x);
   ax_pos   = max(ax_pos,+Vx0 + c_x);
   
   // Calculate properly upwinded edge-averaged Ey:
   cp_SW[CellParams::EY]  = az_pos*ax_pos*Ey_NE + az_pos*ax_neg*Ey_SE + az_neg*ax_pos*Ey_NW + az_neg*ax_neg*Ey_SW;
   cp_SW[CellParams::EY] /= ((az_pos+az_neg)*(ax_pos+ax_neg)+EPS);
   #ifdef FS_1ST_ORDER
      cp_SW[CellParams::EY] -= ax_pos*ax_neg/(ax_pos+ax_neg+EPS)*(Bz_S-Bz_N);
      cp_SW[CellParams::EY] += az_pos*az_neg/(az_pos+az_neg+EPS)*(Bx_W-Bx_E);
   #else
      cp_SW[CellParams::EY] -= ax_pos*ax_neg/(ax_pos+ax_neg+EPS)*((Bz_S-HALF*dBzdx_S) - (Bz_N+HALF*dBzdx_N));
      cp_SW[CellParams::EY] += az_pos*az_neg/(az_pos+az_neg+EPS)*((Bx_W-HALF*dBxdz_W) - (Bx_E+HALF*dBxdz_E));
   #endif
}

static void calculateEdgeElectricFieldZ(const CellID& cellID,ParGrid<SpatialCell>& mpiGrid) {
   namespace fs = fieldsolver;
   namespace pc = physicalconstants;
   
   // An edge has four neighbouring spatial cells. Calculate 
   // electric field in each of the four cells per edge.
   Real ax_pos,ax_neg;              // Max. characteristic velocities to x-direction
   Real ay_pos,ay_neg;              // Max. characteristic velocities to y-direction
   Real Vx0,Vy0;                    // Reconstructed V
   Real c_x,c_y;                    // Characteristic speeds to xy-directions

   // Get read-only pointers to NE,NW,SE,SW states (SW is rw, result is written there):
   const CellID nbr_SE = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2  ,2  ));
   const CellID nbr_NE = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2-1,2  ));
   const CellID nbr_NW = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2-1,2  ));
   #ifndef NDEBUG
      if (nbr_SE == INVALID_CELLID) {cerr << "ERROR: Could not find SE neighbour!" << endl; exit(1);}
      if (nbr_NE == INVALID_CELLID) {cerr << "ERROR: Could not find NE neighbour!" << endl; exit(1);}
      if (nbr_NW == INVALID_CELLID) {cerr << "ERROR: Could not find NW neighbour!" << endl; exit(1);}
   #endif
   
   Real* const cp_SW  = mpiGrid[cellID]->cpu_cellParams;
   creal* const cp_SE = mpiGrid[nbr_SE]->cpu_cellParams;
   creal* const cp_NE = mpiGrid[nbr_NE]->cpu_cellParams;
   creal* const cp_NW = mpiGrid[nbr_NW]->cpu_cellParams;
   
   creal* const derivs_SW = mpiGrid[cellID]->cpu_derivatives;
   creal* const derivs_SE = mpiGrid[nbr_SE]->cpu_derivatives;
   creal* const derivs_NE = mpiGrid[nbr_NE]->cpu_derivatives;
   creal* const derivs_NW = mpiGrid[nbr_NW]->cpu_derivatives;
   
   // Fetch needed plasma parameters/derivatives from the four cells:
   creal Bx_S    = cp_SW[CellParams::BX];
   creal By_W    = cp_SW[CellParams::BY];
   creal By_E    = cp_SE[CellParams::BY];
   creal Bx_N    = cp_NW[CellParams::BX];
   creal dBxdy_S = derivs_SW[fs::dBxdy];
   creal dBxdz_S = derivs_SW[fs::dBxdz];
   creal dBydx_W = derivs_SW[fs::dBydx];
   creal dBydz_W = derivs_SW[fs::dBydz];
   creal dBydx_E = derivs_SE[fs::dBydx];
   creal dBydz_E = derivs_SE[fs::dBydz];
   creal dBxdy_N = derivs_NW[fs::dBxdy];
   creal dBxdz_N = derivs_NW[fs::dBxdz];
   
   // Ez and characteristic speeds on SW cell:
   Vx0  = cp_SW[CellParams::RHOVX]/cp_SW[CellParams::RHO];
   Vy0  = cp_SW[CellParams::RHOVY]/cp_SW[CellParams::RHO];

   // 1st order terms:
   Real Ez_SW = Bx_S*Vy0 - By_W*Vx0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ez_SW  += +HALF*((Bx_S - HALF*dBxdy_S)*(-derivs_SW[fs::dVydx] - derivs_SW[fs::dVydy]) - dBxdy_S*Vy0 + SIXTH*dBxdz_S*derivs_SW[fs::dVydz]);
      Ez_SW  += -HALF*((By_W - HALF*dBydx_W)*(-derivs_SW[fs::dVxdx] - derivs_SW[fs::dVxdy]) - dBydx_W*Vx0 + SIXTH*dBydz_W*derivs_SW[fs::dVxdz]);
   #endif

   // Calculate maximum wave speed (fast magnetosonic speed) on SW cell. In order 
   // to get Alfven speed we need to calculate some reconstruction coeff. for Bz:
   Real Bx2,By2,Bz2;
   const CellID nbrID_SW      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2  ,2+1));
   creal* const nbr_cp_SW     = mpiGrid[nbrID_SW]->cpu_cellParams;
   creal* const nbr_derivs_SW = mpiGrid[nbrID_SW]->cpu_derivatives;
   c_x = calculateFastMSspeedXY(cp_SW,derivs_SW,nbr_cp_SW,nbr_derivs_SW,Bx_S,By_W,dBxdy_S,dBxdz_S,dBydx_W,dBydz_W,MINUS,MINUS);
   c_y = c_x;
   ax_neg   = max(ZERO,-Vx0 + c_x);
   ax_pos   = max(ZERO,+Vx0 + c_x);
   ay_neg   = max(ZERO,-Vy0 + c_y);
   ay_pos   = max(ZERO,+Vy0 + c_y);
   
   // Ez and characteristic speeds on SE (i-1) cell:
   Vx0  = cp_SE[CellParams::RHOVX]/cp_SE[CellParams::RHO];
   Vy0  = cp_SE[CellParams::RHOVY]/cp_SE[CellParams::RHO];
   
   // 1st order terms:
   Real Ez_SE = Bx_S*Vy0 - By_E*Vx0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ez_SE  += +HALF*((Bx_S - HALF*dBxdy_S)*(+derivs_SE[fs::dVydx] - derivs_SE[fs::dVydy]) - dBxdy_S*Vy0 + SIXTH*dBxdz_S*derivs_SE[fs::dVydz]);
      Ez_SE  += -HALF*((By_E + HALF*dBydx_E)*(+derivs_SE[fs::dVxdx] - derivs_SE[fs::dVxdy]) + dBydx_E*Vx0 + SIXTH*dBydz_E*derivs_SE[fs::dVxdz]);
   #endif

   const CellID nbrID_SE      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2  ,2+1));
   creal* const nbr_cp_SE     = mpiGrid[nbrID_SE]->cpu_cellParams;
   creal* const nbr_derivs_SE = mpiGrid[nbrID_SE]->cpu_derivatives;
   c_x = calculateFastMSspeedXY(cp_SE,derivs_SE,nbr_cp_SE,nbr_derivs_SE,Bx_S,By_E,dBxdy_S,dBxdz_S,dBydx_E,dBydz_E,PLUS,MINUS);
   c_y = c_x;   
   ax_neg = max(ax_neg,-Vx0 + c_x);
   ax_pos = max(ax_pos,+Vx0 + c_x);
   ay_neg = max(ay_neg,-Vy0 + c_y);
   ay_pos = max(ay_pos,+Vy0 + c_y);

   // Ez and characteristic speeds on NW (j-1) cell:
   Vx0  = cp_NW[CellParams::RHOVX]/cp_NW[CellParams::RHO];
   Vy0  = cp_NW[CellParams::RHOVY]/cp_NW[CellParams::RHO];
   
   // 1st order terms:
   Real Ez_NW = Bx_N*Vy0 - By_W*Vx0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ez_NW  += +HALF*((Bx_N + HALF*dBxdy_N)*(-derivs_NW[fs::dVydx] + derivs_NW[fs::dVydy]) + dBxdy_N*Vy0 + SIXTH*dBxdz_N*derivs_NW[fs::dVydz]);
      Ez_NW  += -HALF*((By_W - HALF*dBydx_W)*(-derivs_NW[fs::dVxdx] + derivs_NW[fs::dVxdy]) - dBydx_W*Vx0 + SIXTH*dBydz_W*derivs_NW[fs::dVxdz]);
   #endif

   const CellID nbrID_NW      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2-1,2+1));
   creal* const nbr_cp_NW     = mpiGrid[nbrID_NW]->cpu_cellParams;
   creal* const nbr_derivs_NW = mpiGrid[nbrID_NW]->cpu_derivatives;
   c_x = calculateFastMSspeedXY(cp_NW,derivs_NW,nbr_cp_NW,nbr_derivs_NW,Bx_N,By_W,dBxdy_N,dBxdz_N,dBydx_W,dBydz_W,MINUS,PLUS);
   c_y = c_x;
   ax_neg = max(ax_neg,-Vx0 + c_x); 
   ax_pos = max(ax_pos,+Vx0 + c_x);
   ay_neg = max(ay_neg,-Vy0 + c_y);
   ay_pos = max(ay_pos,+Vy0 + c_y);
   
   // Ez and characteristic speeds on NE (i-1,j-1) cell:
   Vx0  = cp_NE[CellParams::RHOVX]/cp_NE[CellParams::RHO];
   Vy0  = cp_NE[CellParams::RHOVY]/cp_NE[CellParams::RHO];
   
   // 1st order terms:
   Real Ez_NE = Bx_N*Vy0 - By_E*Vx0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ez_NE  += +HALF*((Bx_N + HALF*dBxdy_N)*(+derivs_NE[fs::dVydx] + derivs_NE[fs::dVydy]) + dBxdy_N*Vy0 + SIXTH*dBxdz_N*derivs_NE[fs::dVydz]);
      Ez_NE  += -HALF*((By_E + HALF*dBydx_E)*(+derivs_NE[fs::dVxdx] + derivs_NE[fs::dVxdy]) + dBydx_E*Vx0 + SIXTH*dBydz_E*derivs_NE[fs::dVxdz]);
   #endif
   
   const CellID nbrID_NE      = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2-1,2+1));
   creal* const nbr_cp_NE     = mpiGrid[nbrID_NW]->cpu_cellParams;
   creal* const nbr_derivs_NE = mpiGrid[nbrID_NW]->cpu_derivatives;
   c_x = calculateFastMSspeedXY(cp_NE,derivs_NE,nbr_cp_NE,nbr_derivs_NE,Bx_N,By_E,dBxdy_N,dBxdz_N,dBydx_E,dBydz_E,PLUS,PLUS);
   c_y = c_x;
   ax_neg = max(ax_neg,-Vx0 + c_x);
   ax_pos = max(ax_pos,+Vx0 + c_x);
   ay_neg = max(ay_neg,-Vy0 + c_y);
   ay_pos = max(ay_pos,+Vy0 + c_y);
   
   // Calculate properly upwinded edge-averaged Ez:
   cp_SW[CellParams::EZ] = ax_pos*ay_pos*Ez_NE + ax_pos*ay_neg*Ez_SE + ax_neg*ay_pos*Ez_NW + ax_neg*ay_neg*Ez_SW;
   cp_SW[CellParams::EZ] /= ((ax_pos+ax_neg)*(ay_pos+ay_neg)+EPS);
   #ifdef FS_1ST_ORDER
      cp_SW[CellParams::EZ] -= ay_pos*ay_neg/(ay_pos+ay_neg+EPS)*(Bx_S-Bx_N);
      cp_SW[CellParams::EZ] += ax_pos*ax_neg/(ax_pos+ax_neg+EPS)*(By_W-By_E);
   #else
      cp_SW[CellParams::EZ] -= ay_pos*ay_neg/(ay_pos+ay_neg+EPS)*((Bx_S-HALF*dBxdy_S) - (Bx_N+HALF*dBxdy_N));
      cp_SW[CellParams::EZ] += ax_pos*ax_neg/(ax_pos+ax_neg+EPS)*((By_W-HALF*dBydx_W) - (By_E+HALF*dBydx_E));
   #endif
}

static void propagateMagneticField(const CellID& cellID,ParGrid<SpatialCell>& mpiGrid,creal& dt) {
   CellID nbrID;
   Real* const cp0 = mpiGrid[cellID]->cpu_cellParams;
   creal* cp1;
   creal* cp2;
   creal dx = cp0[CellParams::DX];
   creal dy = cp0[CellParams::DY];
   creal dz = cp0[CellParams::DZ];
   
   #ifndef NDEBUG
      map<CellID,uint>::const_iterator it = boundaryFlags.find(cellID);
      if (it == boundaryFlags.end()) {cerr << "Could not find boundary flags for cell #" << cellID << endl; exit(1);}
      cuint boundaryFlag = it->second;
   #else
      cuint boundaryFlag = boundaryFlags[cellID];
   #endif
   
   // Propagate face-averaged Bx:
   if ((boundaryFlag & PROPAGATE_BX) == PROPAGATE_BX) {
      nbrID = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2+1,2  ));
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif
      cp1 = mpiGrid[nbrID]->cpu_cellParams;
      
      nbrID = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2  ,2+1));
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif
      cp2 = mpiGrid[nbrID]->cpu_cellParams;
      
      cp0[CellParams::BX] += dt/dz*(cp2[CellParams::EY] - cp0[CellParams::EY]) + dt/dy*(cp0[CellParams::EZ] - cp1[CellParams::EZ]);
   }
   
   // Propagate face-averaged By:
   if ((boundaryFlag & PROPAGATE_BY) == PROPAGATE_BY) {
      nbrID = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2  ,2+1)); 
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif
      cp1 = mpiGrid[nbrID]->cpu_cellParams;
      
      nbrID = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2  ,2  ));
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif
      cp2 = mpiGrid[nbrID]->cpu_cellParams;
      
      cp0[CellParams::BY] += dt/dx*(cp2[CellParams::EZ] - cp0[CellParams::EZ]) + dt/dz*(cp0[CellParams::EX] - cp1[CellParams::EX]);
   }
      
   // Propagate face-averaged Bz:
   if ((boundaryFlag & PROPAGATE_BZ) == PROPAGATE_BZ) {
      nbrID = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2  ,2  ));
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif
      cp1 = mpiGrid[nbrID]->cpu_cellParams;
      
      nbrID = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2+1,2  ));
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif
      cp2 = mpiGrid[nbrID]->cpu_cellParams;
      
      cp0[CellParams::BZ] += dt/dy*(cp2[CellParams::EX] - cp0[CellParams::EX]) + dt/dx*(cp0[CellParams::EY] - cp1[CellParams::EY]);
   }
}

static void calculateTransferStencil1(ParGrid<SpatialCell>& mpiGrid,const vector<CellID>& localCells,
				      TransferStencil<CellID>& stencil) {
   CellID cellID;

   // Flag neighbour bits for each existing neighbour
   // this cell has within stencil size 1 (i-1,j-1,k-1 neighbour is
   // within the stencil, but i-2,j-2,k-2 is not).
   for (size_t cell=0; cell<localCells.size(); ++cell) {
      cellID = localCells[cell];
      uint boundaryFlag = (1 << calcNbrNumber(1,1,1)); // The cell itself exists (bit 13 set to 1)
      for (int k=-1; k<2; ++k) for (int j=-1; j<2; ++j) for (int i=-1; i<2; ++i) {
	 if (i == 0 && (j == 0 && k == 0)) continue;
	 if (mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+i,2+j,2+k)) == INVALID_CELLID) continue;	 
	 boundaryFlag = boundaryFlag | (1 << calcNbrNumber(1+i,1+j,1+k));
      }
      boundaryFlags[cellID] = boundaryFlag;
   }
      
   // Calculate receive list (18/26 = 69% of neighbours). It is actually 
   // easier to check if a neighbour should not be stored to the 
   // transfer list (continue statements below):
   vector<uchar> nbrIDs;
   for (int k=-1; k<2; ++k) for (int j=-1; j<2; ++j) for (int i=-1; i<2; ++i) {
      if (i ==  0 && (j ==  0 && k ==  0)) continue;
      if (i ==  0 && (j ==  1 && k ==  1)) continue;
      if (i == -1 && (j ==  1 && k ==  1)) continue;
      if (i == -1 && (j == -1 && k == -1)) continue;
      if (i ==  1 && (j ==  0 && k ==  1)) continue;
      if (i ==  1 && (j ==  1 && k ==  0)) continue;
      if (i ==  1 && (j ==  1 && k ==  1)) continue;
      if (i ==  1 && (j == -1 && k ==  1)) continue;
      if (i ==  1 && (j ==  1 && k == -1)) continue;
      nbrIDs.push_back(calcNbrTypeID(2+i,2+j,2+k));
   }
   stencil.addReceives(mpiGrid,nbrIDs);
   
   // Calculate send list (18/26 = 69% of neighbours). It is actually easier 
   // here to check if a neighbour should not be stored to the 
   // transfer list (continue statements below):
   nbrIDs.clear();
   for (int k=-1; k<2; ++k) for (int j=-1; j<2; ++j) for (int i=-1; i<2; ++i) {
      if (i ==  0 && (j ==  0 && k ==  0)) continue;
      if (i ==  0 && (j == -1 && k == -1)) continue;
      if (i == +1 && (j == -1 && k == -1)) continue;
      if (i == +1 && (j == +1 && k == +1)) continue;
      if (i == -1 && (j ==  0 && k == -1)) continue;
      if (i == -1 && (j == -1 && k ==  0)) continue;
      if (i == -1 && (j == -1 && k == -1)) continue;
      if (i == -1 && (j == +1 && k == -1)) continue;
      if (i == -1 && (j == -1 && k == +1)) continue;
      nbrIDs.push_back(calcNbrTypeID(2+i,2+j,2+k));
   }
   stencil.addSends(mpiGrid,nbrIDs);
}

static void calculateTransferStencil2(ParGrid<SpatialCell>& mpiGrid,const vector<CellID>& localCells,
				      TransferStencil<CellID>& stencil) {
   // ***** RECV STENCIL *****
   vector<uchar> nbrTypeIDs;
   nbrTypeIDs.push_back(calcNbrTypeID(2+1,2  ,2  ));
   nbrTypeIDs.push_back(calcNbrTypeID(2  ,2+1,2  ));
   nbrTypeIDs.push_back(calcNbrTypeID(2  ,2  ,2+1));
   nbrTypeIDs.push_back(calcNbrTypeID(2+1,2+1,2  ));
   nbrTypeIDs.push_back(calcNbrTypeID(2+1,2  ,2+1));
   nbrTypeIDs.push_back(calcNbrTypeID(2  ,2+1,2+1));
   stencil.addReceives(mpiGrid,nbrTypeIDs);

   // ***** SEND STENCIL *****
   nbrTypeIDs.clear();
   nbrTypeIDs.push_back(calcNbrTypeID(2-1,2  ,2  ));
   nbrTypeIDs.push_back(calcNbrTypeID(2  ,2-1,2  ));
   nbrTypeIDs.push_back(calcNbrTypeID(2  ,2  ,2-1));
   nbrTypeIDs.push_back(calcNbrTypeID(2-1,2-1,2  ));
   nbrTypeIDs.push_back(calcNbrTypeID(2-1,2  ,2-1));
   nbrTypeIDs.push_back(calcNbrTypeID(2  ,2-1,2-1));
   stencil.addSends(mpiGrid,nbrTypeIDs);
}

// Test, exchanges data with all 26 neighbours
static void calculateTransferStencil3(ParGrid<SpatialCell>& mpiGrid,const vector<CellID>& localCells,
				      TransferStencil<CellID>& stencil) {
   vector<uchar> nbrIDs;
   for (int k=-1; k<2; ++k) for (int j=-1; j<2; ++j) for (int i=-1; i<2; ++i) {
      if (i == 0 && (j == 0 && k == 0)) continue;
      nbrIDs.push_back(calcNbrTypeID(2+i,2+j,2+k));
   }
   stencil.addReceives(mpiGrid,nbrIDs);
   stencil.addSends(mpiGrid,nbrIDs);
}

bool initializeFieldPropagator(ParGrid<SpatialCell>& mpiGrid) {
   vector<uint> cells;
   mpiGrid.getCells(cells);

   calculateBoundaryFlags(mpiGrid,cells);
   calculateTransferStencil1(mpiGrid,cells,stencil1);
   calculateTransferStencil2(mpiGrid,cells,stencil2);
   calculateTransferStencil3(mpiGrid,cells,stencil3);
   
   // Calculate bit masks used for if-statements by field propagator. 
   // These are used to test whether or not certain combination of 
   // neighbours exists for a cell. These can be replaced by honest 
   // if-statements, but you will just end up needing very many of them 
   // as each bit mask tests the existence of several neighbours at once.
   // Existence of neighbours would also need to be queried from the 
   // parallel grid, i.e. using if-statements is likely to be much 
   // slower.
   
   // x-derivatives are calculated if x-face neighbours exist:
   CALCULATE_DX = 0;
   CALCULATE_DX = CALCULATE_DX | (1 << calcNbrNumber(0,1,1));
   CALCULATE_DX = CALCULATE_DX | (1 << calcNbrNumber(2,1,1));
   
   // y-derivatives are calculated if y-face neighbours exist:
   CALCULATE_DY = 0;
   CALCULATE_DY = CALCULATE_DY | (1 << calcNbrNumber(1,0,1));
   CALCULATE_DY = CALCULATE_DY | (1 << calcNbrNumber(1,2,1));
   
   // z-derivatives are calculated if z-face neighbours exist:
   CALCULATE_DZ = 0;
   CALCULATE_DZ = CALCULATE_DZ | (1 << calcNbrNumber(1,1,0));
   CALCULATE_DZ = CALCULATE_DZ | (1 << calcNbrNumber(1,1,2));
   
   // Edge Ex is calculated if -y,-z,+/-x neighbours exist:
   CALCULATE_EX = 0;
   CALCULATE_EX = CALCULATE_EX | (1 << calcNbrNumber(1,0,1)); // -y nbr
   CALCULATE_EX = CALCULATE_EX | (1 << calcNbrNumber(1,1,0)); // -z nbr
   CALCULATE_EX = CALCULATE_EX | (1 << calcNbrNumber(0,1,1)); // -x nbr
   CALCULATE_EX = CALCULATE_EX | (1 << calcNbrNumber(2,1,1)); // +x nbr
   
   // Edge Ey is calculated if -x,-z,+/-y neighbours exist:
   CALCULATE_EY = 0;
   CALCULATE_EY = CALCULATE_EY | (1 << calcNbrNumber(0,1,1)); // -x nbr
   CALCULATE_EY = CALCULATE_EY | (1 << calcNbrNumber(1,1,0)); // -z nbr
   CALCULATE_EY = CALCULATE_EY | (1 << calcNbrNumber(1,0,1)); // -y nbr
   CALCULATE_EY = CALCULATE_EY | (1 << calcNbrNumber(1,2,1)); // +y nbr
   
   // Edge Ez is calculated if -x,-y,+/-z neighbours exist:
   CALCULATE_EZ = 0;
   CALCULATE_EZ = CALCULATE_EZ | (1 << calcNbrNumber(0,1,1)); // -x nbr
   CALCULATE_EZ = CALCULATE_EZ | (1 << calcNbrNumber(1,0,1)); // -y nbr
   CALCULATE_EZ = CALCULATE_EZ | (1 << calcNbrNumber(1,1,0)); // -z nbr
   CALCULATE_EZ = CALCULATE_EZ | (1 << calcNbrNumber(1,1,2)); // +z nbr
   
   // Bx is propagated if -x,+/-y,+/-z neighbours exist:
   PROPAGATE_BX = 0;
   PROPAGATE_BX = PROPAGATE_BX | (1 << calcNbrNumber(0,1,1)); // -x nbr
   PROPAGATE_BX = PROPAGATE_BX | (1 << calcNbrNumber(1,0,1)); // -y nbr
   PROPAGATE_BX = PROPAGATE_BX | (1 << calcNbrNumber(1,2,1)); // +y nbr
   PROPAGATE_BX = PROPAGATE_BX | (1 << calcNbrNumber(1,1,0)); // -z nbr
   PROPAGATE_BX = PROPAGATE_BX | (1 << calcNbrNumber(1,1,2)); // +z nbr
   
   // By is propagated if -y,+/-x,+/-z neighbours exist:
   PROPAGATE_BY = 0;
   PROPAGATE_BY = PROPAGATE_BY | (1 << calcNbrNumber(1,0,1)); // -y nbr
   PROPAGATE_BY = PROPAGATE_BY | (1 << calcNbrNumber(0,1,1)); // -x nbr
   PROPAGATE_BY = PROPAGATE_BY | (1 << calcNbrNumber(2,1,1)); // +x nbr
   PROPAGATE_BY = PROPAGATE_BY | (1 << calcNbrNumber(1,1,0)); // -z nbr
   PROPAGATE_BY = PROPAGATE_BY | (1 << calcNbrNumber(1,1,2)); // +z nbr
   
   // Bz is propagated if -z,+/-x,+/-y neighbours exist:
   PROPAGATE_BZ = 0;
   PROPAGATE_BZ = PROPAGATE_BZ | (1 << calcNbrNumber(1,1,0)); // -z nbr
   PROPAGATE_BZ = PROPAGATE_BZ | (1 << calcNbrNumber(0,1,1)); // -x nbr
   PROPAGATE_BZ = PROPAGATE_BZ | (1 << calcNbrNumber(2,1,1)); // +x nbr
   PROPAGATE_BZ = PROPAGATE_BZ | (1 << calcNbrNumber(1,0,1)); // -y nbr
   PROPAGATE_BZ = PROPAGATE_BZ | (1 << calcNbrNumber(1,2,1)); // +y nbr
   
   // Calculate derivatives and upwinded edge-E. Exchange derivatives 
   // and edge-E:s between neighbouring processes and calculate 
   // face-averaged E,B fields. Note that calculateUpwindedElectricField 
   // does not exchange edge-E:
   vector<CellID> localCells;
   mpiGrid.getCells(localCells);
   calculateDerivativesSimple(mpiGrid,localCells);
   calculateUpwindedElectricFieldSimple(mpiGrid,localCells);
   calculateFaceAveragedFields(mpiGrid);
   return true;
}

bool finalizeFieldPropagator(ParGrid<SpatialCell>& mpiGrid) {
   return true;
}

static void propagateMagneticField(const CellID& cellID,ParGrid<SpatialCell>& mpiGrid,creal& dt);
/*
void calculateDerivativesTaskQueue(ParGrid<SpatialCell>& mpiGrid,creal& dt,const vector<CellID>& localCells) {
   bool allTasksCompleted = true;
   CellID cellID = numeric_limits<CellID>::max();
   uint priority = 0;
   namespace fs = fieldsolver;
   #ifndef NDEBUG
      int myrank;
      MPI_Comm_rank(MPI_COMM_WORLD,&myrank);
   #endif
   
   // Exchange cellParams with neighbours (2nd order accuracy). Post receives:
   mpiGrid.startSingleMode();
   for (map<pair<int,int>,CellID>::const_iterator it=stencil1.recvs.begin(); it!=stencil1.recvs.end(); ++it) {
      const int host     = it->first.first;
      const int tag      = it->first.second;
      const int bytes    = 7*sizeof(Real); // BX,BY,BZ,RHO,RHOVX,RHOVY,RHOVZ
      const CellID nbrID = it->second;
      char* buffer       = reinterpret_cast<char*>(mpiGrid[nbrID]->cpu_cellParams + CellParams::BX);
      mpiGrid.singleReceive(nbrID,tag,bytes,buffer,nbrID);
   }
   // Post sends for cellParams:
   for (multimap<CellID,pair<int,int> >::const_iterator it=stencil1.sends.begin(); it!=stencil1.sends.end(); ++it) {
      const int host       = it->second.first;
      const int tag        = it->second.second;
      const int bytes      = 7*sizeof(Real);
      const CellID localID = it->first;
      char* buffer       = reinterpret_cast<char*>(mpiGrid[localID]->cpu_cellParams + CellParams::BX);
      mpiGrid.singleSend(host,tag,bytes,buffer,localID);
   }
   // Derivatives are calculated during the first pass over local cells and then exchanged
   // with remote processes. Post receives for derivatives:
   mpiGrid.startSingleMode2();
   for (map<pair<int,int>,CellID>::const_iterator it=stencil1.recvs.begin(); it!=stencil1.recvs.end(); ++it) {
      const int host     = it->first.first;
      const int tag      = it->first.second;
      const CellID nbrID = it->second;
      //const uint bytes   = (fs::dVzdz+1)*sizeof(Real);
      const int bytes    = SIZE_DERIVATIVES*sizeof(Real);
      char* buffer       = reinterpret_cast<char*>(mpiGrid[nbrID]->cpu_derivatives);
      mpiGrid.singleReceive2(nbrID,tag,bytes,buffer,nbrID);
   }
   
   // Push all inner local cell IDs into priority queue. Number of sends in derivatives
   // stencil is used as cell's priority:
   readyCells.clear();
   for (set<CellID>::const_iterator it=stencil1.innerCells.begin(); it!=stencil1.innerCells.end(); ++it) {
      const CellID cellID = *it;
      const size_t N_sends = stencil1.sends.count(cellID);
      readyCells.insert(cellID,N_sends);
   }
   
   // Clear the number of received remote neighbours:
   for (map<CellID,pair<uint,uint> >::iterator it=stencil1.neighbours.begin(); it!=stencil1.neighbours.end(); ++it) it->second.second = 0;
   for (map<CellID,pair<uint,uint> >::iterator it=stencil2.neighbours.begin(); it!=stencil2.neighbours.end(); ++it) it->second.second = 0;
   
   size_t calculatedCells = 0;
   size_t cellsToCalculate = stencil1.innerCells.size() + stencil1.boundaryCells.size();
   do {
      allTasksCompleted = true; // Will be set to false if any incomplete tasks are detected
   
      // Check if data has been received from remote processes. Flag
      // local cells as ready to be computed if all required remote neighbour
      // data has been received:
      mpiGrid.singleModeWaitSome();
      while (mpiGrid.getReadyCell(cellID) == true) {
	 // Increase counter on all local cells requiring remote cell with ID cellID.
	 // If all required data has arrived, push the local cell into readyCells.
	 // Number of remote neighbours the local cell has in the derivatives
	 // stencil is used as its computation priority:
	 for (multimap<CellID,CellID>::const_iterator it=stencil1.remoteToLocalMap.lower_bound(cellID); it!=stencil1.remoteToLocalMap.upper_bound(cellID); ++it) {
	    map<CellID,pair<uint,uint> >::iterator localCell = stencil1.neighbours.find(it->second);
            #ifndef NDEBUG
	    if (localCell == stencil1.neighbours.end()) {
	       cerr << "ERROR could not find cell #" << it->second << " from stencil1.neighbours!" << endl;
	       exit(1);
	    }
            #endif
	    ++(localCell->second.second);
	    if (localCell->second.second == localCell->second.first) {
	       readyCells.insert(localCell->first,stencil1.sends.count(localCell->first));
	    }
	 }
	 allTasksCompleted = false;
      }
      
      // Check if a local cell can be computed:
      if (readyCells.empty() == false) {
	 readyCells.pop(cellID,priority);
	 calculateDerivatives(cellID,mpiGrid);
	 ++calculatedCells;
	 
	 // Send the derivatives to remote neighbours (if necessary):
	 for (multimap<CellID,pair<int,int> >::const_iterator it=stencil1.sends.lower_bound(cellID); it!=stencil1.sends.upper_bound(cellID); ++it) {
	    const int host       = it->second.first;
	    const int tag        = it->second.second;
	    //const int bytes      = (fs::dVzdz+1)*sizeof(Real);
	    const int bytes      = SIZE_DERIVATIVES*sizeof(Real);
	    const CellID localID = it->first;
	    char* buffer         = reinterpret_cast<char*>(mpiGrid[localID]->cpu_derivatives);
	    mpiGrid.singleSend2(host,tag,bytes,buffer,localID);
	 }
      }
	 
      if (calculatedCells != cellsToCalculate) allTasksCompleted = false;
      //if (calculatedCells != localCells.size()) allTasksCompleted = false;      
   } while (allTasksCompleted == false);
   
   // Clear the number of received cells in stencilCellParams:
   for (map<CellID,pair<uint,uint> >::iterator it=stencil1.neighbours.begin(); it!=stencil1.neighbours.end(); ++it) it->second.second = 0;
   
   // Wait for all cellParams sends to complete:
   cerr << "P#" << mpiGrid.rank() << " wait sends" << endl;
   mpiGrid.singleModeWaitAllSends();   
}

void calculateUpwindedElectricFieldTaskQueue(ParGrid<SpatialCell>& mpiGrid,creal& dt,const vector<CellID>& localCells) {
   bool allTasksCompleted = true;
   CellID cellID = numeric_limits<CellID>::max();
   uint priority = 0;
   
   // Post receives for cellParams (required for field propagation), upwinded 
   // electric field is exchanged:
   mpiGrid.startSingleMode();
   for (map<pair<int,int>,CellID>::const_iterator it=stencil2.recvs.begin(); it!=stencil2.recvs.end(); ++it) {
      const int host     = it->first.first;
      const int tag      = it->first.second;
      const CellID nbrID = it->second;
      const int bytes    = 3*sizeof(Real); // EX,EY,EZ
      char* buffer       = reinterpret_cast<char*>(mpiGrid[nbrID]->cpu_cellParams + CellParams::EX);
      mpiGrid.singleReceive(nbrID,tag,bytes,buffer,nbrID);
   }
   
   // Push all inner cells of derivatives stencil into readyCells:
   readyCells.clear();
   for (set<CellID>::const_iterator it=stencil1.innerCells.begin(); it!=stencil1.innerCells.end(); ++it) {
      const CellID cellID = *it;
      const size_t N_sends = stencil2.sends.count(cellID);
      readyCells.insert(cellID,N_sends);
   }   
   
   size_t calculatedCells = 0;
   size_t cellsToCalculate = stencil1.innerCells.size() + stencil1.boundaryCells.size();
   do {
      allTasksCompleted = true; // Will be set to false if any incomplete tasks are detected
      
      // Check if data has been received from remote processes. Flag local cells that have all the
      // required data on this process as ready:
      mpiGrid.singleModeWaitSome2();
      while (mpiGrid.getReadyCell2(cellID) == true) {
	 for (multimap<CellID,CellID>::const_iterator it=stencil1.remoteToLocalMap.lower_bound(cellID); it!=stencil1.remoteToLocalMap.upper_bound(cellID); ++it) {
	    map<CellID,pair<uint,uint> >::iterator localCell = stencil1.neighbours.find(it->second);
            #ifndef NDEBUG
	    if (localCell == stencil1.neighbours.end()) {
	       cerr << "ERROR could not find cell #" << it->second << " from stencil1.neighbours!" << endl;
	       exit(1);
	    }
            #endif
	    ++(localCell->second.second);
	    if (localCell->second.second == localCell->second.first) {
	       readyCells.insert(localCell->first,stencil2.sends.count(localCell->first));
	    }
	 }
	 allTasksCompleted = false;
      }
      
      // Check if a local cell can be computed:
      if (readyCells.empty() == false) {
	 readyCells.pop(cellID,priority);
	 cuint boundaryFlag = boundaryFlags[cellID];
	 if ((boundaryFlag & CALCULATE_EX) == CALCULATE_EX) calculateEdgeElectricFieldX(cellID,mpiGrid);
	 if ((boundaryFlag & CALCULATE_EY) == CALCULATE_EY) calculateEdgeElectricFieldY(cellID,mpiGrid);
	 if ((boundaryFlag & CALCULATE_EZ) == CALCULATE_EZ) calculateEdgeElectricFieldZ(cellID,mpiGrid);
	 ++calculatedCells;
	 
	 // Send the calculated electric field to remote neighbours (if necessary):
	 for (multimap<CellID,pair<int,int> >::const_iterator it=stencil2.sends.lower_bound(cellID); it!=stencil2.sends.upper_bound(cellID); ++it) {
	    const int host       = it->second.first;
	    const int tag        = it->second.second;
	    const int bytes      = 3*sizeof(Real);
	    const CellID localID = it->first;
	    char* buffer         = reinterpret_cast<char*>(mpiGrid[localID]->cpu_cellParams + CellParams::EX);
	    mpiGrid.singleSend(host,tag,bytes,buffer,localID);
	 }
      }
      
      if (calculatedCells != cellsToCalculate) allTasksCompleted = false;
      //if (calculatedCells != localCells.size()) allTasksCompleted = false;
   } while (allTasksCompleted == false);
   
   // Clear the number of received cells:
   for (map<CellID,pair<uint,uint> >::iterator it=stencil2.neighbours.begin(); it!=stencil2.neighbours.end(); ++it) it->second.second = 0;
   
   // Wait for all derivative sends to complete:
   mpiGrid.singleModeWaitAllSends2();
}   

void propagateMagneticFieldTaskQueue(ParGrid<SpatialCell>& mpiGrid,creal& dt,const vector<CellID>& localCells) {
   bool allTasksCompleted = false;
   CellID cellID = numeric_limits<CellID>::max();
   uint priority = 0;
   
   // Post receives for new magnetic field components:
   mpiGrid.startSingleMode2();
   for (map<pair<int,int>,CellID>::const_iterator it=stencil2.recvs.begin(); it!=stencil2.recvs.end(); ++it) {
      const int host     = it->first.first;
      const int tag      = it->first.second;
      const uint bytes   = 3*sizeof(Real);
      const CellID nbrID = it->second;
      char* const buffer = reinterpret_cast<char*>(mpiGrid[nbrID]->cpu_cellParams + CellParams::BX);
      mpiGrid.singleReceive2(nbrID,tag,bytes,buffer,nbrID);
   }
   // Push all inner cells of cellParams stencil into readyCells:
   readyCells.clear();
   for (set<CellID>::const_iterator it=stencil2.innerCells.begin(); it!=stencil2.innerCells.end(); ++it) {
      const CellID cellID = *it;
      const size_t N_sends = stencil2.sends.count(cellID);
      readyCells.insert(cellID,N_sends);
   }
   
   size_t calculatedCells = 0;
   size_t cellsToCalculate = stencil2.innerCells.size() + stencil2.boundaryCells.size();
   do {
      allTasksCompleted = true; // Will be set to false if any incomplete tasks are detected
      
      // Check if data has been received from remote processes. Increase
      // counter on all local cells that need the arrived data. If a
      // local has all required neighbour data insert it into readyCells.
      mpiGrid.singleModeWaitSome();
      while (mpiGrid.getReadyCell(cellID) == true) {
	 for (multimap<CellID,CellID>::const_iterator it=stencil2.remoteToLocalMap.lower_bound(cellID); it!=stencil2.remoteToLocalMap.upper_bound(cellID); ++it) {
	    map<CellID,pair<uint,uint> >::iterator localCell = stencil2.neighbours.find(it->second);
            #ifndef NDEBUG
	    if (localCell == stencil2.neighbours.end()) {
	       cerr << "ERROR could not find cell #" << it->second << " from stencil2.neighbours!" << endl;
	       exit(1);
	    }
            #endif
	    ++(localCell->second.second);
	    if (localCell->second.second == localCell->second.first) {
	       readyCells.insert(localCell->first,stencil2.sends.count(localCell->first));
	    }
	 }
	 allTasksCompleted = false;
      }
   
      // Check if a local cell can be computed:
      if (readyCells.empty() == false) {
	 readyCells.pop(cellID,priority);
	 
	 // Propagate magnetic field forward in time.
	 // Note: this function does not propagate faces that are outside the simulation volume -
	 // those faces need to be calculated using boundary conditions. External faces are needed
	 // if Hall term is included.
	 propagateMagneticField(cellID,mpiGrid,dt);
	 ++calculatedCells;
	 
	 // Send the new magnetic field to remote neighbours (if necessary):
	 for (multimap<CellID,pair<int,int> >::const_iterator it=stencil2.sends.lower_bound(cellID); it!=stencil2.sends.upper_bound(cellID); ++it) {
	    const int host       = it->second.first;
	    const int tag        = it->second.second;
	    const int bytes      = 3*sizeof(Real);
	    const CellID localID = it->first;
	    char* buffer         = reinterpret_cast<char*>(mpiGrid[localID]->cpu_cellParams + CellParams::BX);
	    mpiGrid.singleSend2(host,tag,bytes,buffer,localID);
	 }
      }

      if (calculatedCells != cellsToCalculate) allTasksCompleted = false;
      //if (calculatedCells != localCells.size()) allTasksCompleted = false;
   } while (allTasksCompleted == false);
   
   // Wait for cellParams sends to complete (edge E values):
   mpiGrid.singleModeWaitAllSends();
   
   // Wait for cellParams recvs + sends to complete (new B values):
   while (mpiGrid.getRemainingReceives2() > 0) mpiGrid.singleModeWaitSome2();
   mpiGrid.singleModeWaitAllSends2();

   // Calculate new B on faces outside the simulation domain using boundary conditions.
   // Vector localCells contains all cells (local + remote neighbours) stored on this
   // process, so boundary conditions are correctly calculated for remote ghost cells as well.
   for (size_t cell=0; cell<localCells.size(); ++cell) { 
      const CellID cellID = localCells[cell];
      #ifndef NDEBUG
         const map<CellID,uint>::const_iterator it = boundaryFlags.find(cellID);
         if (it == boundaryFlags.end()) {cerr << "ERROR Could not find boundary flag for cell #" << cellID << endl; exit(1);}
         cuint existingCells = it->second;
      #else
         cuint existingCells = boundaryFlags[cellID];
      #endif
      cuint nonExistingCells = (existingCells ^ numeric_limits<uint>::max());
      if ((existingCells & PROPAGATE_BX) != PROPAGATE_BX)
	mpiGrid[cellID]->cpu_cellParams[CellParams::BX] = fieldSolverBoundaryCondBx<CellID,uint,Real>(cellID,existingCells,nonExistingCells,mpiGrid);
      if ((existingCells & PROPAGATE_BY) != PROPAGATE_BY)
	mpiGrid[cellID]->cpu_cellParams[CellParams::BY] = fieldSolverBoundaryCondBy<CellID,uint,Real>(cellID,existingCells,nonExistingCells,mpiGrid);
      if ((existingCells & PROPAGATE_BZ) != PROPAGATE_BZ)
	mpiGrid[cellID]->cpu_cellParams[CellParams::BZ] = fieldSolverBoundaryCondBz<CellID,uint,Real>(cellID,existingCells,nonExistingCells,mpiGrid);
   }
}
*/
void calculateDerivativesSimple(ParGrid<SpatialCell>& mpiGrid,const vector<CellID>& localCells) {
   bool allTasksCompleted = true;
   CellID cellID = numeric_limits<CellID>::max();
   uint priority = 0;
   namespace fs = fieldsolver;
   
   // Exchange cellParams with neighbours (2nd order accuracy). Post receives:
   mpiGrid.startSingleMode();
   //for (map<pair<int,int>,CellID>::const_iterator it=stencil1.recvs.begin(); it!=stencil1.recvs.end(); ++it) {
   for (map<pair<int,int>,CellID>::const_iterator it=stencil3.recvs.begin(); it!=stencil3.recvs.end(); ++it) {
      const int host     = it->first.first;
      const int tag      = it->first.second;
      const CellID nbrID = it->second;
      const int bytes    = 7*sizeof(Real); // BX,BY,BZ,RHO,RHOVX,RHOVY,RHOVZ
      char* buffer       = reinterpret_cast<char*>(mpiGrid[nbrID]->cpu_cellParams + CellParams::BX);
      mpiGrid.singleReceive(nbrID,tag,bytes,buffer,nbrID);
   }
   // Post sends for cellParams:
   //for (multimap<CellID,pair<int,int> >::const_iterator it=stencil1.sends.begin(); it!=stencil1.sends.end(); ++it) {
   for (multimap<CellID,pair<int,int> >::const_iterator it=stencil3.sends.begin(); it!=stencil3.sends.end(); ++it) {
      const int host       = it->second.first;
      const int tag        = it->second.second;
      const CellID localID = it->first;
      const int bytes    = 7*sizeof(Real);
      char* buffer       = reinterpret_cast<char*>(mpiGrid[localID]->cpu_cellParams + CellParams::BX);
      mpiGrid.singleSend(host,tag,bytes,buffer,localID);
   }
   
   // Calculate derivatives on inner cells:
   //for (set<CellID>::const_iterator it=stencil1.innerCells.begin(); it!=stencil1.innerCells.end(); ++it) {
   for (set<CellID>::const_iterator it=stencil3.innerCells.begin(); it!=stencil3.innerCells.end(); ++it) {
      const CellID cellID = *it;
      calculateDerivatives(cellID,mpiGrid);
   }
   // Wait for all neighbour data:
   mpiGrid.waitAllReceives();
   
   // Calculate derivatives on boundary cells:
   //for (set<CellID>::const_iterator it=stencil1.boundaryCells.begin(); it!=stencil1.boundaryCells.end(); ++it) {
   for (set<CellID>::const_iterator it=stencil3.boundaryCells.begin(); it!=stencil3.boundaryCells.end(); ++it) {
      const CellID cellID = *it;
      calculateDerivatives(cellID,mpiGrid);
   }
   // Wait for all sends to complete:
   mpiGrid.waitAllSends();
}

void calculateUpwindedElectricFieldSimple(ParGrid<SpatialCell>& mpiGrid,const vector<CellID>& localCells) {
   namespace fs = fieldsolver;
   #ifndef NDEBUG
      int myrank;
      MPI_Comm_rank(MPI_COMM_WORLD,&myrank);
   #endif
   
   // Derivatives are calculated during the first pass over local cells and then exchanged
   // with remote processes. Post receives for derivatives:
   mpiGrid.startSingleMode();
   //for (map<pair<int,int>,CellID>::const_iterator it=stencil1.recvs.begin(); it!=stencil1.recvs.end(); ++it) {
   for (map<pair<int,int>,CellID>::const_iterator it=stencil3.recvs.begin(); it!=stencil3.recvs.end(); ++it) {
      const int host     = it->first.first;
      const int tag      = it->first.second;
      const CellID nbrID = it->second;
      const uint bytes   = SIZE_DERIVATIVES*sizeof(Real);
      char* buffer       = reinterpret_cast<char*>(mpiGrid[nbrID]->cpu_derivatives);
      mpiGrid.singleReceive(nbrID,tag,bytes,buffer,nbrID);
   }
   // Post sends for derivatives:
   //for (multimap<CellID,pair<int,int> >::const_iterator it=stencil1.sends.begin(); it!=stencil1.sends.end(); ++it) {
   for (multimap<CellID,pair<int,int> >::const_iterator it=stencil3.sends.begin(); it!=stencil3.sends.end(); ++it) {
      const int host       = it->second.first;
      const int tag        = it->second.second;
      const int bytes      = SIZE_DERIVATIVES*sizeof(Real);
      const CellID localID = it->first;
      char* buffer         = reinterpret_cast<char*>(mpiGrid[localID]->cpu_derivatives);
      mpiGrid.singleSend(host,tag,bytes,buffer,localID);      
   }
   
   // Calculate upwinded electric field on inner cells:
   //for (set<CellID>::const_iterator it=stencil1.innerCells.begin(); it!=stencil1.innerCells.end(); ++it) {
   for (set<CellID>::const_iterator it=stencil3.innerCells.begin(); it!=stencil3.innerCells.end(); ++it) {
      const CellID cellID = *it;
      cuint boundaryFlag = boundaryFlags[cellID];
      if ((boundaryFlag & CALCULATE_EX) == CALCULATE_EX) calculateEdgeElectricFieldX(cellID,mpiGrid);
      if ((boundaryFlag & CALCULATE_EY) == CALCULATE_EY) calculateEdgeElectricFieldY(cellID,mpiGrid);
      if ((boundaryFlag & CALCULATE_EZ) == CALCULATE_EZ) calculateEdgeElectricFieldZ(cellID,mpiGrid);
   }
   
   // Wait for all derivative receives:
   mpiGrid.waitAllReceives();
   
   // Calculate upwinded electric field on boundary cells:
   //for (set<CellID>::const_iterator it=stencil1.boundaryCells.begin(); it!=stencil1.boundaryCells.end(); ++it) {
   for (set<CellID>::const_iterator it=stencil3.boundaryCells.begin(); it!=stencil3.boundaryCells.end(); ++it) {
      const CellID cellID = *it;
      cuint boundaryFlag = boundaryFlags[cellID];
      if ((boundaryFlag & CALCULATE_EX) == CALCULATE_EX) calculateEdgeElectricFieldX(cellID,mpiGrid);
      if ((boundaryFlag & CALCULATE_EY) == CALCULATE_EY) calculateEdgeElectricFieldY(cellID,mpiGrid);
      if ((boundaryFlag & CALCULATE_EZ) == CALCULATE_EZ) calculateEdgeElectricFieldZ(cellID,mpiGrid);
   }
   
   // Wait for all derivative sends to complete:
   mpiGrid.waitAllSends();
   
   // Exchange electric field with neighbouring processes:
   mpiGrid.startSingleMode();
   //for (map<pair<int,int>,CellID>::const_iterator it=stencil2.recvs.begin(); it!=stencil2.recvs.end(); ++it) {
   for (map<pair<int,int>,CellID>::const_iterator it=stencil3.recvs.begin(); it!=stencil3.recvs.end(); ++it) {
      const int host     = it->first.first;
      const int tag      = it->first.second;
      const int bytes    = 3*sizeof(Real);
      const CellID nbrID = it->second;
      char* buffer       = reinterpret_cast<char*>(mpiGrid[nbrID]->cpu_cellParams + CellParams::EX);
      mpiGrid.singleReceive(nbrID,tag,bytes,buffer,nbrID);
   }
   // Post sends for cellParams:
   //for (multimap<CellID,pair<int,int> >::const_iterator it=stencil2.sends.begin(); it!=stencil2.sends.end(); ++it) {
   for (multimap<CellID,pair<int,int> >::const_iterator it=stencil3.sends.begin(); it!=stencil3.sends.end(); ++it) {
      const int host       = it->second.first;
      const int tag        = it->second.second;
      const int bytes      = 3*sizeof(Real);
      const CellID localID = it->first;
      char* buffer         = reinterpret_cast<char*>(mpiGrid[localID]->cpu_cellParams + CellParams::EX);
      mpiGrid.singleSend(host,tag,bytes,buffer,localID);
   }
   mpiGrid.waitAllReceives();
   mpiGrid.waitAllSends();
}

static void propagateMagneticFieldSimple(ParGrid<SpatialCell>& mpiGrid,creal& dt,const vector<CellID>& localCells) {
   // Propagate B on all local cells:
   for (size_t cell=0; cell<localCells.size(); ++cell) {
      const CellID cellID = localCells[cell];
      propagateMagneticField(cellID,mpiGrid,dt);
   }
   
   // Calculate new B on faces outside the simulation domain using boundary conditions.
   // Vector localCells contains all cells (local + remote neighbours) stored on this 
   // process, so boundary conditions are correctly calculated for remote ghost cells as well.
   for (size_t cell=0; cell<localCells.size(); ++cell) {
      const CellID cellID = localCells[cell];
      #ifndef NDEBUG
         const map<CellID,uint>::const_iterator it = boundaryFlags.find(cellID);
         if (it == boundaryFlags.end()) {cerr << "ERROR Could not find boundary flag for cell #" << cellID << endl; exit(1);}
         cuint existingCells = it->second;
      #else
         cuint existingCells = boundaryFlags[cellID];
      #endif
      cuint nonExistingCells = (existingCells ^ numeric_limits<uint>::max());
      if ((existingCells & PROPAGATE_BX) != PROPAGATE_BX)
	mpiGrid[cellID]->cpu_cellParams[CellParams::BX] = fieldSolverBoundaryCondBx<CellID,uint,Real>(cellID,existingCells,nonExistingCells,mpiGrid);
      if ((existingCells & PROPAGATE_BY) != PROPAGATE_BY)
	mpiGrid[cellID]->cpu_cellParams[CellParams::BY] = fieldSolverBoundaryCondBy<CellID,uint,Real>(cellID,existingCells,nonExistingCells,mpiGrid);
      if ((existingCells & PROPAGATE_BZ) != PROPAGATE_BZ)
	mpiGrid[cellID]->cpu_cellParams[CellParams::BZ] = fieldSolverBoundaryCondBz<CellID,uint,Real>(cellID,existingCells,nonExistingCells,mpiGrid);
   }
}
/*
bool calculateEdgeElectricField(ParGrid<SpatialCell>& mpiGrid) {
   bool success = true;
   
   vector<CellID> localCells;
   mpiGrid.getCells(localCells);

   // Check if MPI transfer stencils need to be recalculated:
   if (Parameters::recalculateStencils == true) {
      mpiGrid.getCells(localCells);
      calculateTransferStencil1(mpiGrid,localCells,stencil1);
      calculateTransferStencil2(mpiGrid,localCells,stencil2);
      calculateTransferStencil2(mpiGrid,localCells,stencil3);
      mpiGrid.getAllCells(localCells);
      Parameters::recalculateStencils = false;
   }

   // Calculate upwinded edge-averaged electric field for all cells. 
   // Calculated E is not exchanged with remote neighbours.
   //creal dt = Parameters::dt;
   calculateDerivativesSimple(mpiGrid,localCells);
   calculateUpwindedElectricFieldSimple(mpiGrid,localCells);   
   return success;
}
*/
bool propagateFields(ParGrid<SpatialCell>& mpiGrid,creal& dt) {
   typedef Parameters P;

   // Reserve memory for derivatives for all cells on this process:
   vector<CellID> localCells;
   mpiGrid.getCells(localCells);

   // Check if MPI transfer stencils need to be recalculated:
   if (P::recalculateStencils == true) {
      calculateTransferStencil1(mpiGrid,localCells,stencil1);
      calculateTransferStencil2(mpiGrid,localCells,stencil2);
      calculateTransferStencil2(mpiGrid,localCells,stencil3);
      P::recalculateStencils = false;
   }

   propagateMagneticFieldSimple(mpiGrid,dt,localCells);
   calculateDerivativesSimple(mpiGrid,localCells);
   calculateUpwindedElectricFieldSimple(mpiGrid,localCells);
   calculateFaceAveragedFields(mpiGrid);
   return true;
}

enum Rec {a_0,a_x,a_y,a_z,a_xx,a_xy,a_xz,
     b_0,b_x,b_y,b_z,b_yx,b_yy,b_yz,
     c_0,c_x,c_y,c_z,c_zx,c_zy,c_zz
};

void reconstructionCoefficients(const CellID& cellID,const CellID& nbr_i2j1k1,const CellID& nbr_i1j2k1,const CellID& nbr_i1j1k2,
				ParGrid<SpatialCell>& mpiGrid,Real* result) {
   // Do not calculate values for non-existing cells:
   if (cellID == INVALID_CELLID) {
      for (int i=0; i<Rec::c_zz+1; ++i) result[i] = 0.0;
      return;
   }
   
   namespace fs = fieldsolver;
   namespace cp = CellParams;
   
   Real* const cep_i1j1k1 = mpiGrid[cellID]->cpu_cellParams;
   
   // Create a dummy array for containing zero values for cellParams on non-existing cells:
   Real dummyCellParams[SIZE_CELLPARAMS];
   for (int i=0; i<SIZE_CELLPARAMS; ++i) dummyCellParams[i] = 0.0;
   
   Real* cep_i2j1k1 = NULL;
   Real* cep_i1j2k1 = NULL;
   Real* cep_i1j1k2 = NULL;
   if (nbr_i2j1k1 == INVALID_CELLID) cep_i2j1k1 = dummyCellParams;
   else cep_i2j1k1 = mpiGrid[nbr_i2j1k1]->cpu_cellParams;
   if (nbr_i1j2k1 == INVALID_CELLID) cep_i1j2k1 = dummyCellParams;
   else cep_i1j2k1 = mpiGrid[nbr_i1j2k1]->cpu_cellParams;
   if (nbr_i1j1k2 == INVALID_CELLID) cep_i1j1k2 = dummyCellParams;
   else cep_i1j1k2 = mpiGrid[nbr_i1j1k2]->cpu_cellParams;

   #ifndef FS_1ST_ORDER
      creal* const der_i1j1k1 = mpiGrid[cellID]->cpu_derivatives;

      // Create a dummy array for containing zero values for derivatives on non-existing cells:
      Real dummyDerivatives[SIZE_DERIVATIVES];
      for (int i=0; i<SIZE_DERIVATIVES; ++i) dummyDerivatives[i] = 0.0;
   
      // Fetch neighbour cell derivatives, or in case the neighbour does not 
      // exist, use dummyDerivatives array:
      Real* der_i2j1k1 = NULL;
      Real* der_i1j2k1 = NULL;
      Real* der_i1j1k2 = NULL;
      if (nbr_i2j1k1 == INVALID_CELLID) der_i2j1k1 = dummyDerivatives;
      else der_i2j1k1 = mpiGrid[nbr_i2j1k1]->cpu_derivatives;
      if (nbr_i1j2k1 == INVALID_CELLID) der_i1j2k1 = dummyDerivatives;
      else der_i1j2k1 = mpiGrid[nbr_i1j2k1]->cpu_derivatives;
      if (nbr_i1j1k2 == INVALID_CELLID) der_i1j1k2 = dummyDerivatives;
      else der_i1j1k2 = mpiGrid[nbr_i1j1k2]->cpu_derivatives;

      // Calculate 2nd order reconstruction coefficients:
      result[Rec::a_xy] = der_i2j1k1[fs::dBxdy] - der_i1j1k1[fs::dBxdy];
      result[Rec::a_xz] = der_i2j1k1[fs::dBxdz] - der_i1j1k1[fs::dBxdz];
      result[Rec::a_x ] = cep_i2j1k1[cp::BX] - cep_i1j1k1[cp::BX];
      result[Rec::a_y ] = HALF*(der_i2j1k1[fs::dBxdy] + der_i1j1k1[fs::dBxdy]);
      result[Rec::a_z ] = HALF*(der_i2j1k1[fs::dBxdz] + der_i1j1k1[fs::dBxdz]);
   
      result[Rec::b_yx] = der_i1j2k1[fs::dBydx] - der_i1j1k1[fs::dBydx];
      result[Rec::b_yz] = der_i1j2k1[fs::dBydz] - der_i1j1k1[fs::dBydz];
      result[Rec::b_x ] = HALF*(der_i1j2k1[fs::dBydx] + der_i1j1k1[fs::dBydx]);
      result[Rec::b_y ] = cep_i1j2k1[cp::BY] - cep_i1j1k1[cp::BY];
      result[Rec::b_z ] = HALF*(der_i1j2k1[fs::dBydz] + der_i1j1k1[fs::dBydz]);
   
      result[Rec::c_zx] = der_i1j1k2[fs::dBzdx] - der_i1j1k1[fs::dBzdx];
      result[Rec::c_zy] = der_i1j1k2[fs::dBzdy] - der_i1j1k1[fs::dBzdy];
      result[Rec::c_x ] = HALF*(der_i1j1k2[fs::dBzdx] + der_i1j1k1[fs::dBzdx]);
      result[Rec::c_y ] = HALF*(der_i1j1k2[fs::dBzdy] + der_i1j1k1[fs::dBzdy]);
      result[Rec::c_z ] = cep_i1j1k2[cp::BZ] - cep_i1j1k1[cp::BZ];
   
      result[Rec::a_xx] = -HALF*(result[Rec::b_yx] + result[Rec::c_zx]);
      result[Rec::b_yy] = -HALF*(result[Rec::a_xy] + result[Rec::c_zy]);
      result[Rec::c_zz] = -HALF*(result[Rec::a_xz] + result[Rec::b_yz]);
   #else
      for (int i=0; i<Rec::c_zz+1; ++i) result[i] = 0.0;
   #endif
   
   // Calculate 1st order reconstruction coefficients:
   result[Rec::a_0 ] = HALF*(cep_i2j1k1[cp::BX] + cep_i1j1k1[cp::BX]) - SIXTH*result[Rec::a_xx];
   result[Rec::b_0 ] = HALF*(cep_i1j2k1[cp::BY] + cep_i1j1k1[cp::BY]) - SIXTH*result[Rec::b_yy];
   result[Rec::c_0 ] = HALF*(cep_i1j1k2[cp::BZ] + cep_i1j1k1[cp::BZ]) - SIXTH*result[Rec::c_zz];
}

void averageFaceXElectricField(const CellID& cellID,const CellID& nbr_i1j2k1,const CellID& nbr_i1j1k2,const CellID& nbr_i1j2k2,
			       const CellID& nbr_i0j1k1,const CellID& nbr_i0j2k1,const CellID& nbr_i0j1k2,const CellID& nbr_i0j2k2,
			       cuint& existingCells,ParGrid<SpatialCell>& mpiGrid,Real* result) {
   namespace fs = fieldsolver;
   namespace cp = CellParams;
   
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }
   
   cuint REQUIRED_CELLS = (1 << calcNbrNumber(1,1,1)) | (1 << calcNbrNumber(1,2,1)) | (1 << calcNbrNumber(1,1,2)) | (1 << calcNbrNumber(1,2,2))
     | (1 << calcNbrNumber(0,1,1)) | (1 << calcNbrNumber(0,2,1)) | (1 << calcNbrNumber(0,1,2)) | (1 << calcNbrNumber(0,2,2));

   // If all required neighbour data exists, calculate E vector on 
   // x-face. NEEDS IMPROVEMENT!
   if ((existingCells & REQUIRED_CELLS) == REQUIRED_CELLS) {
      creal* const cep_i1j1k1 = mpiGrid[cellID]->cpu_cellParams;
      creal* const cep_i1j2k1 = mpiGrid[nbr_i1j2k1]->cpu_cellParams;
      creal* const cep_i1j1k2 = mpiGrid[nbr_i1j1k2]->cpu_cellParams;
      creal* const cep_i1j2k2 = mpiGrid[nbr_i1j2k2]->cpu_cellParams;
      creal* const cep_i0j1k1 = mpiGrid[nbr_i0j1k1]->cpu_cellParams;
      creal* const cep_i0j2k1 = mpiGrid[nbr_i0j2k1]->cpu_cellParams;
      creal* const cep_i0j1k2 = mpiGrid[nbr_i0j1k2]->cpu_cellParams;
      creal* const cep_i0j2k2 = mpiGrid[nbr_i0j2k2]->cpu_cellParams;
   
      result[0] = EIGTH*(cep_i1j1k1[cp::EX] + cep_i1j2k1[cp::EX] + cep_i1j1k2[cp::EX] + cep_i1j2k2[cp::EX]
			 + cep_i0j1k1[cp::EX] + cep_i0j2k1[cp::EX] + cep_i0j1k2[cp::EX] + cep_i0j2k2[cp::EX]);
      result[1] = HALF*(cep_i1j1k1[cp::EY] + cep_i1j1k2[cp::EY]);
      result[2] = HALF*(cep_i1j1k1[cp::EZ] + cep_i1j2k1[cp::EZ]);
   } else {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
   }
}

void averageFaceYElectricField(const CellID& cellID,const CellID& nbr_i2j1k1,const CellID& nbr_i1j1k2,const CellID& nbr_i2j1k2,
			       const CellID& nbr_i1j0k1,const CellID& nbr_i2j0k1,const CellID& nbr_i1j0k2,const CellID& nbr_i2j0k2,
			       cuint& existingCells,ParGrid<SpatialCell>& mpiGrid,Real* result) {
   namespace fs = fieldsolver;
   namespace cp = CellParams;
   
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }

   cuint REQUIRED_CELLS = (1 << calcNbrNumber(1,1,1)) | (1 << calcNbrNumber(2,1,1)) | (1 << calcNbrNumber(1,1,2)) | (1 << calcNbrNumber(2,1,2))
                        | (1 << calcNbrNumber(1,0,1)) | (1 << calcNbrNumber(2,0,1)) | (1 << calcNbrNumber(1,0,2)) | (1 << calcNbrNumber(2,0,2));
   
   // If all required neighbour data exists, calculate E vector on
   // y-face. NEEDS IMPROVEMENT!
   if ((existingCells & REQUIRED_CELLS) == REQUIRED_CELLS) {
      creal* const cep_i1j1k1 = mpiGrid[cellID]->cpu_cellParams;
      creal* const cep_i2j1k1 = mpiGrid[nbr_i2j1k1]->cpu_cellParams;
      creal* const cep_i1j1k2 = mpiGrid[nbr_i1j1k2]->cpu_cellParams;
      creal* const cep_i2j1k2 = mpiGrid[nbr_i2j1k2]->cpu_cellParams;
      creal* const cep_i1j0k1 = mpiGrid[nbr_i1j0k1]->cpu_cellParams;
      creal* const cep_i2j0k1 = mpiGrid[nbr_i2j0k1]->cpu_cellParams;
      creal* const cep_i1j0k2 = mpiGrid[nbr_i1j0k2]->cpu_cellParams;
      creal* const cep_i2j0k2 = mpiGrid[nbr_i2j0k2]->cpu_cellParams;
      
      result[0] = HALF*(cep_i1j1k1[cp::EX] + cep_i1j1k2[cp::EX]);
      result[1] = EIGTH*(cep_i1j1k1[cp::EY] + cep_i2j1k1[cp::EY] + cep_i1j1k2[cp::EY] + cep_i2j1k2[cp::EY]
		       + cep_i1j0k1[cp::EY] + cep_i2j0k1[cp::EY] + cep_i1j0k2[cp::EY] + cep_i2j0k2[cp::EY]);
      result[2] = HALF*(cep_i1j1k1[cp::EZ] + cep_i2j1k1[cp::EZ]);
   } else {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
   }
}

void averageFaceZElectricField(const CellID& cellID,const CellID& nbr_i2j1k1,const CellID& nbr_i1j2k1,const CellID& nbr_i2j2k1,
			       const CellID& nbr_i1j1k0,const CellID& nbr_i2j1k0,const CellID& nbr_i1j2k0,const CellID& nbr_i2j2k0,
			       cuint& existingCells,ParGrid<SpatialCell>& mpiGrid,Real* result) {
   namespace fs = fieldsolver;
   namespace cp = CellParams;
   
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }

   cuint REQUIRED_CELLS = (1 << calcNbrNumber(1,1,1)) | (1 << calcNbrNumber(2,1,1)) | (1 << calcNbrNumber(1,2,1)) | (1 << calcNbrNumber(2,2,1))
     | (1 << calcNbrNumber(1,1,0)) | (1 << calcNbrNumber(2,1,0)) | (1 << calcNbrNumber(1,2,0)) | (1 << calcNbrNumber(2,2,0));
   
   // If all required neighbour data exists, calculate E vector on
   // y-face. NEEDS IMPROVEMENT!
   if ((existingCells & REQUIRED_CELLS) == REQUIRED_CELLS) {
      creal* const cep_i1j1k1 = mpiGrid[cellID]->cpu_cellParams;
      creal* const cep_i2j1k1 = mpiGrid[nbr_i2j1k1]->cpu_cellParams;
      creal* const cep_i1j2k1 = mpiGrid[nbr_i1j2k1]->cpu_cellParams;
      creal* const cep_i2j2k1 = mpiGrid[nbr_i2j2k1]->cpu_cellParams;
      creal* const cep_i1j1k0 = mpiGrid[nbr_i1j1k0]->cpu_cellParams;
      creal* const cep_i2j1k0 = mpiGrid[nbr_i2j1k0]->cpu_cellParams;
      creal* const cep_i1j2k0 = mpiGrid[nbr_i1j1k0]->cpu_cellParams;
      creal* const cep_i2j2k0 = mpiGrid[nbr_i2j2k0]->cpu_cellParams;
      
      result[0] = HALF*(cep_i1j1k1[cp::EX] + cep_i1j2k1[cp::EX]);
      result[1] = HALF*(cep_i1j1k1[cp::EY] + cep_i2j1k1[cp::EY]);
      result[2] = EIGTH*(cep_i1j1k1[cp::EZ] + cep_i2j1k1[cp::EZ] + cep_i1j2k1[cp::EZ] + cep_i2j2k1[cp::EZ]
		       + cep_i1j1k0[cp::EZ] + cep_i2j1k0[cp::EZ] + cep_i1j2k0[cp::EZ] + cep_i2j2k0[cp::EZ]);
   } else {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
   }
}

void averageFaceXMagnField(const CellID& cellID,const CellID& nbr_i2j1k1,const CellID& nbr_i1j2k1,const CellID& nbr_i1j1k2,
			   creal* const coefficients,ParGrid<SpatialCell>& mpiGrid,const int& I,Real* result) {
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }

   Real*  const cep_i1j1k1 = mpiGrid[cellID]->cpu_cellParams;

   // Store calculated face-averaged B on x-faces:
   result[0] = cep_i1j1k1[CellParams::BX];
   result[1] = coefficients[Rec::b_0] + I*HALF*coefficients[Rec::b_x];
   result[2] = coefficients[Rec::c_0] + I*HALF*coefficients[Rec::c_x];
}

void averageFaceYMagnField(const CellID& cellID,const CellID& nbr_i2j1k1,const CellID& nbr_i1j2k1,const CellID& nbr_i1j1k2,
			   creal* const coefficients,ParGrid<SpatialCell>& mpiGrid,const int& J,Real* result) {
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }

   Real*  const cep_i1j1k1 = mpiGrid[cellID]->cpu_cellParams;

   // Store calculated face-averaged B on y-faces:
   result[0] = coefficients[Rec::a_0] + J*HALF*coefficients[Rec::a_y];
   result[1] = cep_i1j1k1[CellParams::BY];
   result[2] = coefficients[Rec::c_0] + J*HALF*coefficients[Rec::c_y];
}

void averageFaceZMagnField(const CellID& cellID,const CellID& nbr_i2j1k1,const CellID& nbr_i1j2k1,const CellID& nbr_i1j1k2,
			   creal* const coefficients,ParGrid<SpatialCell>& mpiGrid,const int& K,Real* result) {
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }

   Real*  const cep_i1j1k1 = mpiGrid[cellID]->cpu_cellParams;

   // Store calculated face-averaged B on z-faces:
   result[0] = coefficients[Rec::a_0] + K*HALF*coefficients[Rec::a_z];
   result[1] = coefficients[Rec::b_0] + K*HALF*coefficients[Rec::b_z];
   result[2] = cep_i1j1k1[CellParams::BZ];
}
   
void calculateFaceAveragedFields(ParGrid<SpatialCell>& mpiGrid) {
   namespace fs = fieldsolver;
   
   vector<CellID> localCells;
   mpiGrid.getCells(localCells);

   Real faceMagnField[9];
   Real coefficients[Rec::c_zz+1];
   Real coefficients2[Rec::c_zz+1];
   
   uint existingCells = 0;
   for (size_t cell=0; cell<localCells.size(); ++cell) {
      const CellID cellID = localCells[cell];
      
      // Get neighbour flags for the cell:
      map<CellID,uint>::const_iterator it = boundaryFlags.find(cellID);
      if (it == boundaryFlags.end()) existingCells = 0;
      else existingCells = it->second;

      // Get pointer to cellParams array for cell cellID:
      Real* const cellParams = mpiGrid[cellID]->cpu_cellParams;

      // Calculate reconstruction coefficients for this cell:
      const CellID nbr_i2j1k1 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2  ,2  ));
      const CellID nbr_i1j2k1 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2+1,2  ));
      const CellID nbr_i1j1k2 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2  ,2+1));
      const CellID nbr_i1j2k2 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2+1,2+1));
      reconstructionCoefficients(cellID,nbr_i2j1k1,nbr_i1j2k1,nbr_i1j1k2,mpiGrid,coefficients);
      
      // Calculate reconstruction coefficients for i-1 neighbour:
      const CellID nbr_i0j1k1 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2  ,2  ));
      const CellID nbr_i0j2k1 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2+1,2  ));
      const CellID nbr_i0j1k2 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2  ,2+1));
      const CellID nbr_i0j2k2 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2-1,2+1,2+1));
      reconstructionCoefficients(nbr_i0j1k1,cellID,nbr_i0j2k1,nbr_i0j1k2,mpiGrid,coefficients2);
      
      // Calculate B vector on both sides of x-face:
      averageFaceXMagnField(cellID,nbr_i2j1k1,nbr_i1j2k1,nbr_i1j1k2,coefficients ,mpiGrid,-1,cellParams+CellParams::BXFACEX);
      averageFaceXMagnField(nbr_i0j1k1,cellID,nbr_i0j2k1,nbr_i0j1k2,coefficients2,mpiGrid,+1,faceMagnField+0);
      
      // Calculate E vector on x-face (NEEDS IMPROVEMENT):
      averageFaceXElectricField(cellID,nbr_i1j2k1,nbr_i1j1k2,nbr_i1j2k2,nbr_i0j1k1,nbr_i0j2k1,nbr_i0j1k2,nbr_i0j2k2,
				existingCells,mpiGrid,cellParams+CellParams::EXFACEX);
      
      // Calculate B vector on both sides of y-face:
      const CellID nbr_i2j1k2 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2  ,2+1));
      const CellID nbr_i2j0k1 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2-1,2  ));
      const CellID nbr_i1j0k1 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2-1,2  ));
      const CellID nbr_i1j0k2 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2-1,2+1));
      const CellID nbr_i2j0k2 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2-1,2+1));
      reconstructionCoefficients(nbr_i1j0k1,nbr_i2j0k1,cellID,nbr_i1j0k2,mpiGrid,coefficients2);
      
      averageFaceYMagnField(cellID,nbr_i2j1k1,nbr_i1j2k1,nbr_i1j1k2,coefficients ,mpiGrid,-1,cellParams+CellParams::BXFACEY);
      averageFaceYMagnField(nbr_i1j0k1,nbr_i2j0k1,cellID,nbr_i1j0k2,coefficients2,mpiGrid,+1,faceMagnField+3);

      // Calculate E vector on y-face (NEEDS IMPROVEMENT):
      averageFaceYElectricField(cellID,nbr_i2j1k1,nbr_i1j1k2,nbr_i2j1k2,nbr_i1j0k1,nbr_i2j0k1,nbr_i1j0k2,nbr_i2j0k2,
				existingCells,mpiGrid,cellParams+CellParams::EXFACEX);
      
      // Calculate B vector on both sides of z-face:
      const CellID nbr_i2j2k1 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2+1,2  ));
      const CellID nbr_i2j1k0 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2  ,2-1));
      const CellID nbr_i1j2k0 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2+1,2-1));
      const CellID nbr_i1j1k0 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2  ,2-1));
      const CellID nbr_i2j2k0 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2+1,2-1));
      reconstructionCoefficients(nbr_i1j1k0,nbr_i2j1k0,nbr_i1j2k0,cellID,mpiGrid,coefficients2);
      
      averageFaceZMagnField(cellID,nbr_i2j1k1,nbr_i1j2k1,nbr_i1j1k2,coefficients ,mpiGrid,-1,cellParams+CellParams::BXFACEZ);
      averageFaceZMagnField(nbr_i1j1k0,nbr_i2j1k0,nbr_i1j2k0,cellID,coefficients2,mpiGrid,+1,faceMagnField+6);

      // Calculate E vector on z-face (NEEDS IMPROVEMENT):
      averageFaceZElectricField(cellID,nbr_i2j1k1,nbr_i1j2k1,nbr_i2j2k1,nbr_i1j1k0,nbr_i2j1k0,nbr_i1j2k0,nbr_i2j2k0,
				existingCells,mpiGrid,cellParams+CellParams::EXFACEX);
      
      // Store the average value (maybe should store upwinded value?): 
      for (uint i=0; i<9; ++i) {
	cellParams[CellParams::BXFACEX+i] = HALF*(cellParams[CellParams::BXFACEX+i] + faceMagnField[i]);
      }
   }
}

void calculateVolumeAveragedFields(ParGrid<SpatialCell>& mpiGrid) {
   namespace fs = fieldsolver;
   namespace cp = CellParams;
                                   
   vector<CellID> localCells;
   mpiGrid.getCells(localCells);
   
   Real coefficients[Rec::c_zz+1];
      
   cuint EX_CELLS = (1 << calcNbrNumber(1,1,1)) | (1 << calcNbrNumber(1,2,1)) | (1 << calcNbrNumber(1,1,2)) | (1 << calcNbrNumber(1,2,2));
   cuint EY_CELLS = (1 << calcNbrNumber(1,1,1)) | (1 << calcNbrNumber(2,1,1)) | (1 << calcNbrNumber(1,1,2)) | (1 << calcNbrNumber(2,1,2));
   cuint EZ_CELLS = (1 << calcNbrNumber(1,1,1)) | (1 << calcNbrNumber(2,1,1)) | (1 << calcNbrNumber(1,2,1)) | (1 << calcNbrNumber(2,2,1));
   
   uint existingCells = 0;
   for (size_t cell=0; cell<localCells.size(); ++cell) {
      const CellID cellID = localCells[cell];
      
      // Get neighbour flags for the cell:
      map<CellID,uint>::const_iterator it = boundaryFlags.find(cellID);
      if (it == boundaryFlags.end()) existingCells = 0;
      else existingCells = it->second;
      
      // Calculate reconstruction coefficients for this cell:
      const CellID nbr_i2j1k1 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2  ,2  ));
      const CellID nbr_i1j2k1 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2+1,2  ));
      const CellID nbr_i1j1k2 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2  ,2+1));
      reconstructionCoefficients(cellID,nbr_i2j1k1,nbr_i1j2k1,nbr_i1j1k2,mpiGrid,coefficients);
      
      // Calculate volume average of B:
      Real* const cellParams = mpiGrid[cellID]->cpu_cellParams;
      cellParams[cp::BXVOL] = coefficients[Rec::a_0];
      cellParams[cp::BYVOL] = coefficients[Rec::b_0];
      cellParams[cp::BZVOL] = coefficients[Rec::c_0];
      
      // Calculate volume average of E (NEEDS IMPROVEMENT):
      const CellID nbr_i1j2k2 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2  ,2+1,2+1));
      const CellID nbr_i2j1k2 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2  ,2+1));
      const CellID nbr_i2j2k1 = mpiGrid.getNeighbour(cellID,calcNbrTypeID(2+1,2+1,2  ));
      creal* const cep_i1j1k1 = cellParams;
      
      if ((existingCells & EX_CELLS) == EX_CELLS) {
	 creal* const cep_i1j2k1 = mpiGrid[nbr_i1j2k1]->cpu_cellParams;
	 creal* const cep_i1j1k2 = mpiGrid[nbr_i1j1k2]->cpu_cellParams;
	 creal* const cep_i1j2k2 = mpiGrid[nbr_i1j2k2]->cpu_cellParams;
	 cellParams[cp::EXVOL] = FOURTH*(cep_i1j1k1[cp::EX] + cep_i1j2k1[cp::EX] + cep_i1j1k2[cp::EX] + cep_i1j2k2[cp::EX]);
      } else {
	 cellParams[cp::EXVOL] = 0.0;
      }
      
      if ((existingCells & EY_CELLS) == EY_CELLS) {
	 creal* const cep_i2j1k1 = mpiGrid[nbr_i2j1k1]->cpu_cellParams;
	 creal* const cep_i1j1k2 = mpiGrid[nbr_i1j1k2]->cpu_cellParams;
	 creal* const cep_i2j1k2 = mpiGrid[nbr_i2j1k2]->cpu_cellParams;
	 cellParams[cp::EYVOL] = FOURTH*(cep_i1j1k1[cp::EY] + cep_i2j1k1[cp::EY] + cep_i1j1k2[cp::EY] + cep_i2j1k2[cp::EY]);
      } else {
	 cellParams[cp::EYVOL] = 0.0;
      }
      
      if ((existingCells & EZ_CELLS) == EZ_CELLS) {
	 creal* const cep_i2j1k1 = mpiGrid[nbr_i2j1k1]->cpu_cellParams;
	 creal* const cep_i1j2k1 = mpiGrid[nbr_i1j2k1]->cpu_cellParams;
	 creal* const cep_i2j2k1 = mpiGrid[nbr_i2j2k1]->cpu_cellParams;
	 cellParams[cp::EZVOL] = FOURTH*(cep_i1j1k1[cp::EZ] + cep_i2j1k1[cp::EZ] + cep_i1j2k1[cp::EZ] + cep_i2j2k1[cp::EZ]);
      } else {
	 cellParams[cp::EZVOL] = 0.0;
      }
   }
}

#else // #ifdef PARGRID

bool calculateEdgeElectricField(dccrg<SpatialCell>& mpiGrid) { }
void calculateFaceAveragedFields(dccrg<SpatialCell>& mpiGrid) { }
void calculateVolumeAveragedFields(dccrg<SpatialCell>& mpiGrid) { }
bool finalizeFieldPropagator(dccrg<SpatialCell>& mpiGrid) { }
bool initializeFieldPropagator(dccrg<SpatialCell>& mpiGrid) { }
bool propagateFields(dccrg<SpatialCell>& mpiGrid,creal& dt) { }

#endif

