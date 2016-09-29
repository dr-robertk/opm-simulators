/*
  Copyright (c) 2014 SINTEF ICT, Applied Mathematics.
  Copyright (c) 2015 IRIS AS

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef OPM_SIMULATORFULLYIMPLICITBLACKOILOUTPUTEBOS_HEADER_INCLUDED
#define OPM_SIMULATORFULLYIMPLICITBLACKOILOUTPUTEBOS_HEADER_INCLUDED
#include <opm/core/grid.h>
#include <opm/core/simulator/SimulatorTimerInterface.hpp>
#include <opm/core/simulator/WellState.hpp>
#include <opm/core/utility/DataMap.hpp>
#include <opm/common/ErrorMacros.hpp>
#include <opm/common/OpmLog/OpmLog.hpp>
#include <opm/output/eclipse/EclipseReader.hpp>
#include <opm/core/utility/miscUtilities.hpp>
#include <opm/core/utility/parameters/ParameterGroup.hpp>
#include <opm/core/wells/DynamicListEconLimited.hpp>

#include <opm/output/Cells.hpp>
#include <opm/output/eclipse/EclipseWriter.hpp>

#include <opm/autodiff/Compat.hpp>
#include <opm/autodiff/GridHelpers.hpp>
#include <opm/autodiff/ParallelDebugOutput.hpp>

#include <opm/autodiff/WellStateFullyImplicitBlackoil.hpp>
#include <opm/autodiff/ThreadHandle.hpp>
#include <opm/autodiff/AutoDiffBlock.hpp>

#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>
#include <opm/parser/eclipse/EclipseState/InitConfig/InitConfig.hpp>


#include <string>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <thread>

#include <boost/filesystem.hpp>

#ifdef HAVE_OPM_GRID
#include <dune/grid/CpGrid.hpp>
#endif
namespace Opm
{
    class BlackoilState;

    /** \brief Wrapper class for VTK, Matlab, and ECL output. */
    class BlackoilOutputWriterEbos
    {

    public:
        // constructor creating different sub writers
        template <class Grid>
        BlackoilOutputWriterEbos(const Grid& grid,
                                 const parameter::ParameterGroup& param,
                                 Opm::EclipseStateConstPtr eclipseState,
                                 const Opm::PhaseUsage &phaseUsage,
                                 const double* permeability );

        /*!
         * \brief Write a blackoil reservoir state to disk for later inspection with
         *        visualization tools like ResInsight. This function will extract the
         *        requested output cell properties specified by the RPTRST keyword
         *        and write these to file.
         */
        template<class Model>
        void writeTimeStep(const SimulatorTimerInterface& timer,
                           const SimulationDataContainer& reservoirState,
                           const Opm::WellState& wellState,
                           const Model& physicalModel,
                           bool substep = false);


        /*!
         * \brief Write a blackoil reservoir state to disk for later inspection with
         *        visualization tools like ResInsight. This function will write all
         *        CellData in simProps to the file as well.
         */
        void writeTimeStepWithCellProperties(
                           const SimulatorTimerInterface& timer,
                           const SimulationDataContainer& reservoirState,
                           const Opm::WellState& wellState,
                           const std::vector<data::CellData>& simProps,
                           bool substep = false);

        /*!
         * \brief Write a blackoil reservoir state to disk for later inspection with
         *        visualization tools like ResInsight. This function will not write
         *        any cell properties (e.g., those requested by RPTRST keyword)
         */
        void writeTimeStepWithoutCellProperties(
                           const SimulatorTimerInterface& timer,
                           const SimulationDataContainer& reservoirState,
                           const Opm::WellState& wellState,
                           bool substep = false);

        /*!
         * \brief Write a blackoil reservoir state to disk for later inspection withS
         *        visualization tools like ResInsight. This is the function which does
         *        the actual write to file.
         */
        void writeTimeStepSerial(const SimulatorTimerInterface& timer,
                                 const SimulationDataContainer& reservoirState,
                                 const Opm::WellState& wellState,
                                 const std::vector<data::CellData>& simProps,
                                 bool substep);

        /** \brief return output directory */
        const std::string& outputDirectory() const { return outputDir_; }

        /** \brief return true if output is enabled */
        bool output () const { return output_; }

        void restore(SimulatorTimerInterface& timer,
                     BlackoilState& state,
                     WellStateFullyImplicitBlackoil& wellState,
                     const std::string& filename,
                     const int desiredReportStep);


        template <class Grid>
        void initFromRestartFile(const PhaseUsage& phaseusage,
                                 const double* permeability,
                                 const Grid& grid,
                                 SimulationDataContainer& simulatorstate,
                                 WellStateFullyImplicitBlackoil& wellstate);

        bool isRestart() const;

    protected:
        const bool output_;
        std::unique_ptr< ParallelDebugOutputInterface > parallelOutput_;

        // Parameters for output.
        const std::string outputDir_;
        const int output_interval_;

        int lastBackupReportStep_;

        std::ofstream backupfile_;
        Opm::PhaseUsage phaseUsage_;
        std::unique_ptr< EclipseWriter > eclWriter_;
        EclipseStateConstPtr eclipseState_;

        std::unique_ptr< ThreadHandle > asyncOutput_;
    };


    //////////////////////////////////////////////////////////////
    //
    //  Implementation
    //
    //////////////////////////////////////////////////////////////
    template <class Grid>
    inline
    BlackoilOutputWriterEbos::
    BlackoilOutputWriterEbos(const Grid& grid,
                         const parameter::ParameterGroup& param,
                         Opm::EclipseStateConstPtr eclipseState,
                         const Opm::PhaseUsage &phaseUsage,
                         const double* permeability )
      : output_( param.getDefault("output", true) ),
        parallelOutput_( output_ ? new ParallelDebugOutput< Grid >( grid, eclipseState, phaseUsage.num_phases, permeability ) : 0 ),
        outputDir_( output_ ? param.getDefault("output_dir", std::string("output")) : "." ),
        output_interval_( output_ ? param.getDefault("output_interval", 1): 0 ),
        lastBackupReportStep_( -1 ),
        phaseUsage_( phaseUsage ),
        eclWriter_( output_ && parallelOutput_->isIORank() &&
                    param.getDefault("output_ecl", true) ?
                    new EclipseWriter(eclipseState,UgGridHelpers::createEclipseGrid( grid , *eclipseState->getInputGrid()))
                   : 0 ),
        eclipseState_(eclipseState),
        asyncOutput_()
    {
        // For output.
        if (output_ && parallelOutput_->isIORank() ) {
            // Ensure that output dir exists
            boost::filesystem::path fpath(outputDir_);
            try {
                create_directories(fpath);
            }
            catch (...) {
                OPM_THROW(std::runtime_error, "Creating directories failed: " << fpath);
            }

            // create output thread if enabled and rank is I/O rank
            // async output is enabled by default if pthread are enabled
#if HAVE_PTHREAD
            const bool asyncOutputDefault = false;
#else
            const bool asyncOutputDefault = false;
#endif
            if( param.getDefault("async_output", asyncOutputDefault ) )
            {
#if HAVE_PTHREAD
                asyncOutput_.reset( new ThreadHandle() );
#else
                OPM_THROW(std::runtime_error,"Pthreads were not found, cannot enable async_output");
#endif
            }

            std::string backupfilename = param.getDefault("backupfile", std::string("") );
            if( ! backupfilename.empty() )
            {
                backupfile_.open( backupfilename.c_str() );
            }
        }
    }


    template <class Grid>
    inline void
    BlackoilOutputWriterEbos::
    initFromRestartFile( const PhaseUsage& phaseusage,
                         const double* permeability,
                         const Grid& grid,
                         SimulationDataContainer& simulatorstate,
                         WellStateFullyImplicitBlackoil& wellstate)
    {
        // gives a dummy dynamic_list_econ_limited
        DynamicListEconLimited dummy_list_econ_limited;
        WellsManager wellsmanager(eclipseState_,
                                  eclipseState_->getInitConfig().getRestartStep(),
                                  Opm::UgGridHelpers::numCells(grid),
                                  Opm::UgGridHelpers::globalCell(grid),
                                  Opm::UgGridHelpers::cartDims(grid),
                                  Opm::UgGridHelpers::dimensions(grid),
                                  Opm::UgGridHelpers::cell2Faces(grid),
                                  Opm::UgGridHelpers::beginFaceCentroids(grid),
                                  permeability,
                                  dummy_list_econ_limited);

        const Wells* wells = wellsmanager.c_wells();
        wellstate.resize(wells, simulatorstate); //Resize for restart step
        auto restarted = Opm::init_from_restart_file(
                                *eclipseState_,
                                Opm::UgGridHelpers::numCells(grid) );

        solutionToSim( restarted.first, phaseusage, simulatorstate );
        wellsToState( restarted.second, wellstate );
    }





    namespace detail {
        template<class Model>
        std::vector<data::CellData> getCellDataEbos(
                const Opm::PhaseUsage& phaseUsage,
                const Model& model,
                const RestartConfig& restartConfig,
                const int reportStepNum)
        {
            typedef typename Model::FluidSystem FluidSystem;

            std::vector<data::CellData> simProps;

            //Get the value of each of the keys
            std::map<std::string, int> outKeywords = restartConfig.getRestartKeywords(reportStepNum);
            for (auto& keyValue : outKeywords) {
                keyValue.second = restartConfig.getKeyword(keyValue.first, reportStepNum);
            }

            //Get shorthands for water, oil, gas
            const int aqua_active = phaseUsage.phase_used[Opm::PhaseUsage::Aqua];
            const int liquid_active = phaseUsage.phase_used[Opm::PhaseUsage::Liquid];
            const int vapour_active = phaseUsage.phase_used[Opm::PhaseUsage::Vapour];

            const auto& ebosModel = model.ebosSimulator().model();

            // extract everything which can possibly be written to disk
            int numCells = ebosModel.numGridDof();
            std::vector<double> bWater(numCells);
            std::vector<double> bOil(numCells);
            std::vector<double> bGas(numCells);

            std::vector<double> rhoWater(numCells);
            std::vector<double> rhoOil(numCells);
            std::vector<double> rhoGas(numCells);

            std::vector<double> muWater(numCells);
            std::vector<double> muOil(numCells);
            std::vector<double> muGas(numCells);

            std::vector<double> krWater(numCells);
            std::vector<double> krOil(numCells);
            std::vector<double> krGas(numCells);

            std::vector<double> Rs(numCells);
            std::vector<double> Rv(numCells);

            for (int cellIdx = 0; cellIdx < numCells; ++cellIdx) {
                const auto& intQuants = *ebosModel.cachedIntensiveQuantities(cellIdx, /*timeIdx=*/0);
                const auto& fs = intQuants.fluidState();

                bWater[cellIdx] = fs.invB(FluidSystem::waterPhaseIdx).value;
                bOil[cellIdx] = fs.invB(FluidSystem::oilPhaseIdx).value;
                bGas[cellIdx] = fs.invB(FluidSystem::gasPhaseIdx).value;

                rhoWater[cellIdx] = fs.density(FluidSystem::waterPhaseIdx).value;
                rhoOil[cellIdx] = fs.density(FluidSystem::oilPhaseIdx).value;
                rhoGas[cellIdx] = fs.density(FluidSystem::gasPhaseIdx).value;

                muWater[cellIdx] = fs.viscosity(FluidSystem::waterPhaseIdx).value;
                muOil[cellIdx] = fs.viscosity(FluidSystem::oilPhaseIdx).value;
                muGas[cellIdx] = fs.viscosity(FluidSystem::gasPhaseIdx).value;

                krWater[cellIdx] = intQuants.relativePermeability(FluidSystem::waterPhaseIdx).value;
                krOil[cellIdx] = intQuants.relativePermeability(FluidSystem::oilPhaseIdx).value;
                krGas[cellIdx] = intQuants.relativePermeability(FluidSystem::gasPhaseIdx).value;

                Rs[cellIdx] = FluidSystem::saturatedDissolutionFactor(fs,
                                                                      FluidSystem::oilPhaseIdx,
                                                                      intQuants.pvtRegionIndex(),
                                                                      /*maxOilSaturation=*/1.0).value;
                Rv[cellIdx] = FluidSystem::saturatedDissolutionFactor(fs,
                                                                      FluidSystem::gasPhaseIdx,
                                                                      intQuants.pvtRegionIndex(),
                                                                      /*maxOilSaturation=*/1.0).value;
            }

            /**
             * Formation volume factors for water, oil, gas
             */
            if (aqua_active && outKeywords["BW"] > 0) {
                outKeywords["BW"] = 0;
                simProps.emplace_back(data::CellData{
                        "1OVERBW",
                        Opm::UnitSystem::measure::water_inverse_formation_volume_factor,
                        std::move(bWater)});
            }
            if (liquid_active && outKeywords["BO"]  > 0) {
                outKeywords["BO"] = 0;
                simProps.emplace_back(data::CellData{
                        "1OVERBO",
                        Opm::UnitSystem::measure::oil_inverse_formation_volume_factor,
                        std::move(bOil)});
            }
            if (vapour_active && outKeywords["BG"] > 0) {
                outKeywords["BG"] = 0;
                simProps.emplace_back(data::CellData{
                        "1OVERBG",
                        Opm::UnitSystem::measure::gas_inverse_formation_volume_factor,
                        std::move(bGas)});
            }

            /**
             * Densities for water, oil gas
             */
            if (outKeywords["DEN"] > 0) {
                outKeywords["DEN"] = 0;
                if (aqua_active) {
                    simProps.emplace_back(data::CellData{
                            "WAT_DEN",
                            Opm::UnitSystem::measure::density,
                            std::move(rhoWater)});
                }
                if (liquid_active) {
                    simProps.emplace_back(data::CellData{
                            "OIL_DEN",
                            Opm::UnitSystem::measure::density,
                            std::move(rhoOil)});
                }
                if (vapour_active) {
                    simProps.emplace_back(data::CellData{
                            "GAS_DEN",
                            Opm::UnitSystem::measure::density,
                            std::move(rhoGas)});
                }
            }

            /**
             * Viscosities for water, oil gas
             */
            if (outKeywords["VISC"] > 0) {
                outKeywords["VISC"] = 0;
                if (aqua_active) {
                    simProps.emplace_back(data::CellData{
                            "WAT_VISC",
                            Opm::UnitSystem::measure::viscosity,
                            std::move(muWater)});
                }
                if (liquid_active) {
                    simProps.emplace_back(data::CellData{
                            "OIL_VISC",
                            Opm::UnitSystem::measure::viscosity,
                            std::move(muOil)});
                }
                if (vapour_active) {
                    simProps.emplace_back(data::CellData{
                            "GAS_VISC",
                            Opm::UnitSystem::measure::viscosity,
                            std::move(muGas)});
                }
            }

            /**
             * Relative permeabilities for water, oil, gas
             */
            if (aqua_active && outKeywords["KRW"] > 0) {
                outKeywords["KRW"] = 0;
                simProps.emplace_back(data::CellData{
                        "WATKR",
                        Opm::UnitSystem::measure::permeability,
                        std::move(krWater)});
            }
            if (liquid_active && outKeywords["KRO"] > 0) {
                outKeywords["KRO"] = 0;
                simProps.emplace_back(data::CellData{
                         "OILKR",
                         Opm::UnitSystem::measure::permeability,
                         std::move(krOil)});
            }
            if (vapour_active && outKeywords["KRG"] > 0) {
                outKeywords["KRG"] = 0;
                simProps.emplace_back(data::CellData{
                          "GASKR",
                          Opm::UnitSystem::measure::permeability,
                          std::move(krGas)});
            }

            /**
             * Vaporized and dissolved gas/oil ratio
             */
            if (vapour_active && liquid_active && outKeywords["RSSAT"] > 0) {
                outKeywords["RSSAT"] = 0;
                simProps.emplace_back(data::CellData{
                        "RSSAT",
                        Opm::UnitSystem::measure::gas_oil_ratio,
                        std::move(Rs)});
            }
            if (vapour_active && liquid_active && outKeywords["RVSAT"] > 0) {
                outKeywords["RVSAT"] = 0;
                simProps.emplace_back(data::CellData{
                        "RVSAT",
                        Opm::UnitSystem::measure::oil_gas_ratio,
                        std::move(Rv)});
            }


            /**
             * Bubble point and dew point pressures
             */
            if (vapour_active && liquid_active && outKeywords["PBPD"] > 0) {
                outKeywords["PBPD"] = 0;
                Opm::OpmLog::warning("Bubble/dew point pressure output unsupported",
                        "Writing bubble points and dew points (PBPD) to file is unsupported, "
                        "as the simulator does not use these internally.");
            }

            //Warn for any unhandled keyword
            for (auto& keyValue : outKeywords) {
                if (keyValue.second > 0) {
                    std::string logstring = "Keyword '";
                    logstring.append(keyValue.first);
                    logstring.append("' is unhandled for output to file.");
                    Opm::OpmLog::warning("Unhandled output keyword", logstring);
                }
            }

            return simProps;
        }
    }




    template<class Model>
    inline void
    BlackoilOutputWriterEbos::
    writeTimeStep(const SimulatorTimerInterface& timer,
                  const SimulationDataContainer& localState,
                  const WellState& localWellState,
                  const Model& physicalModel,
                  bool substep)
    {
        const RestartConfig& restartConfig = eclipseState_->getRestartConfig();
        const int reportStepNum = timer.reportStepNum();
        std::vector<data::CellData> cellData = detail::getCellDataEbos( phaseUsage_, physicalModel, restartConfig, reportStepNum );
        writeTimeStepWithCellProperties(timer, localState, localWellState, cellData, substep);
    }
}
#endif
