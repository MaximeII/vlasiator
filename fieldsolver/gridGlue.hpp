#pragma once
#include <fsgrid.hpp>
#include <vector>
#include <array>

/*! Take input moments from DCCRG grid and put them into the Fieldsolver grid
 * \param mpiGrid The DCCRG grid carrying rho, rhoV and P
 * \param cells List of local cells
 * \param momentsGrid Fieldsolver grid for these quantities
 *
 * This function assumes that proper grid coupling has been set up.
 */
void feedMomentsIntoFsGrid(dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
                           const std::vector<CellID>& cells,
                           FsGrid< std::array<Real, fsgrids::moments::N_MOMENTS>, 2>& momentsGrid);

/*! Copy field solver result (Volume-averaged fields) and store them back into DCCRG
 * \param mpiGrid The DCCRG grid carrying fields.
 * \param cells List of local cells
 * \param volumeFieldsGrid Fieldsolver grid for these quantities
 *
 * This function assumes that proper grid coupling has been set up.
 */
void getVolumeFieldsFromFsGrid(FsGrid< std::array<Real, fsgrids::volfields::N_VOL>, 2>& volumeFieldsGrid,
                           dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
                           const std::vector<CellID>& cells);


/*! Transfer data into technical grid (boundary info etc.)
 * \param mpiGrid The DCCRG grid carrying rho, rhoV and P
 * \param cells List of local cells
 * \param technicalGrid the target Fieldsolver grid for this information
 *
 * This function assumes that proper grid coupling has been set up.
 */
void setupTechnicalFsGrid(dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
      const std::vector<CellID>& cells, FsGrid< fsgrids::technical, 2>& technicalGrid);


/*! Transfer field data from DCCRG cellparams into the appropriate FsGrid structure
 * \param mpiGrid The DCCRG grid carrying fieldparam data
 * \param cells List of local cells
 * \param index Index into the cellparams array from which to copy
 * \param targetGrid Fieldsolver grid for these quantities
 *
 * The cellparams with indices from index to index+numFields are copied over, and
 * have to be continuous in memory.
 *
 * This function assumes that proper grid coupling has been set up.
 */
template< unsigned int numFields > void feedFieldDataIntoFsGrid(
      dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
      const std::vector<CellID>& cells, int index,
      FsGrid< std::array<Real, numFields>, 2>& targetGrid) {

   targetGrid.setupForTransferIn(cells.size());

   for(CellID i : cells) {
      // TODO: This assumes that RHO, RHOV and P (diagonals) are lying continuous in memory.
      // Check definition of CellParams in common.h if unsure.
      std::array<Real, numFields>* cellDataPointer = reinterpret_cast<std::array<Real, numFields>*>(
            &(mpiGrid[i]->get_cell_parameters()[index]));
      targetGrid.transferDataIn(i - 1, cellDataPointer);
   }

   targetGrid.finishTransfersIn();
}

