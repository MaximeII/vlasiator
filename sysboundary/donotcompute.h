/*
 This file is part of Vlasiator.
 
 Copyright 2010, 2011, 2012 Finnish Meteorological Institute
 
 Vlasiator is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License version 3
 as published by the Free Software Foundation.
 
 Vlasiator is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DONOTCOMPUTE_H
#define DONOTCOMPUTE_H

#include <vector>
#include "../definitions.h"
#include "../readparameters.h"
#include "../spatial_cell.hpp"
#include "sysboundarycondition.h"

using namespace projects;

namespace SBC {
   /*!\brief DoNotCompute is a class handling cells not to be computed.
    * 
    * DoNotCompute is a class handling cells tagged as sysboundarytype::DO_NOT_COMPUTE by a
    * system boundary condition (e.g. SysBoundaryCondition::Ionosphere).
    */
   class DoNotCompute: public SysBoundaryCondition {
   public:
      DoNotCompute();
      virtual ~DoNotCompute();
      
      static void addParameters();
      virtual void getParameters();
      
      virtual bool initSysBoundary(
         creal& t,
         Project &project
      );
      virtual bool assignSysBoundary(dccrg::Dccrg<SpatialCell>& mpiGrid);
      virtual bool applyInitialState(
         const dccrg::Dccrg<SpatialCell>& mpiGrid,
         Project &project
      );
      //       virtual bool applySysBoundaryCondition(const dccrg::Dccrg<SpatialCell>& mpiGrid, creal& t);
      virtual std::string getName() const;
      virtual uint getIndex() const;
   };
}

#endif