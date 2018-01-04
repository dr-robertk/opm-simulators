/*
  Copyright 2017 TNO - Heat Transfer & Fluid Dynamics, Modelling & Optimization of the Subsurface
  Copyright 2017 Statoil ASA.

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

#ifndef OPM_AQUIFERCT_HEADER_INCLUDED
#define OPM_AQUIFERCT_HEADER_INCLUDED

#include <Eigen/QR>
#include <opm/parser/eclipse/EclipseState/AquiferCT.hpp>
#include <opm/parser/eclipse/EclipseState/Aquancon.hpp>
#include <opm/autodiff/BlackoilAquiferModel.hpp>
#include <opm/common/OpmLog/OpmLog.hpp>
#include <opm/core/props/BlackoilPhases.hpp>


#include <opm/material/densead/Math.hpp>
#include <opm/material/densead/Evaluation.hpp>
#include <opm/material/fluidstates/BlackOilFluidState.hpp>

#include <string>
#include <memory>
#include <vector>
#include <algorithm>
#include <map>
#include <cassert>

namespace Opm
{

    template<typename TypeTag>
    class AquiferCarterTracy
    {

        public:
            typedef BlackoilModelParameters ModelParameters;

            static const int Water = BlackoilPhases::Aqua;
            static const int Oil = BlackoilPhases::Liquid;
            static const int Gas = BlackoilPhases::Vapour;

            typedef typename GET_PROP_TYPE(TypeTag, Grid) Grid;
            typedef typename GET_PROP_TYPE(TypeTag, GridView) GridView;
            typedef typename GET_PROP_TYPE(TypeTag, Simulator) Simulator;
            typedef typename GET_PROP_TYPE(TypeTag, FluidSystem) FluidSystem;
            typedef typename GET_PROP_TYPE(TypeTag, Indices) BlackoilIndices;
            typedef typename GET_PROP_TYPE(TypeTag, IntensiveQuantities) IntensiveQuantities;
            typedef typename GET_PROP_TYPE(TypeTag, MaterialLaw) MaterialLaw;
            typedef typename GridView::template Codim<0>::Entity Element;
            typedef typename GET_PROP_TYPE(TypeTag, ElementContext) ElementContext;

            static const int numEq = BlackoilIndices::numEq;
            typedef double Scalar;

            typedef DenseAd::Evaluation<double, /*size=*/numEq> Eval;
            typedef Opm::BlackOilFluidState<Eval, FluidSystem> FluidState;
            typedef typename FluidSystem::WaterPvt WaterPvt;

            typedef Ewoms::BlackOilPolymerModule<TypeTag> PolymerModule;




            static const bool has_solvent = GET_PROP_VALUE(TypeTag, EnableSolvent);
            static const bool has_polymer = GET_PROP_VALUE(TypeTag, EnablePolymer);
            static const int contiSolventEqIdx = BlackoilIndices::contiSolventEqIdx;
            static const int contiPolymerEqIdx = BlackoilIndices::contiPolymerEqIdx;


            explicit AquiferCarterTracy( const AquiferCT::AQUCT_data& params, const Aquancon::AquanconOutput& connection,
                                         const int numComponents, const Scalar gravity, const Simulator& ebosSimulator    )
            : phi_aq_ (params.phi_aq), //
              C_t_ (params.C_t), //
              r_o_ (params.r_o), //
              k_a_ (params.k_a), //
              c1_ (params.c1),
              h_ (params.h), //
              p0_defaulted_ (params.p0_defaulted),
              pa0_ (params.p0),
              theta_ (params.theta), //
              c2_ (params.c2), //
              d0_ (params.d0),
              aqutab_td_ (params.td),
              aqutab_pi_ (params.pi),
              aquiferID_ (params.aquiferID),
              inftableID_ (params.inftableID),
              pvttableID_ (params.pvttableID),
              num_components_ (numComponents),
              gravity_ (gravity),
              ebos_simulator_ (ebosSimulator)
            {
                init_quantities(connection);
            }

            inline const PhaseUsage&
            phaseUsage() const
            {
                assert(phase_usage_);

                return *phase_usage_;
            }


            inline void assembleAquiferEq(Simulator& ebosSimulator, const SimulatorTimerInterface& timer)
            {
                dt_ = timer.currentStepLength();
                auto& ebosJac = ebosSimulator.model().linearizer().matrix();
                auto& ebosResid = ebosSimulator.model().linearizer().residual();


                auto cellID = cell_idx_.begin();

                size_t idx;
                for ( idx = 0; cellID != cell_idx_.end(); ++cellID, ++idx )
                {
                    Eval qinflow = 0.0;
                    // We are dereferencing the value of IntensiveQuantities because cachedIntensiveQuantities return a const pointer to
                    // IntensiveQuantities of that particular cell_id
                    const IntensiveQuantities intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(*cellID, /*timeIdx=*/ 0));
                    // This is the pressure at td + dt
                    get_current_Pressure_cell(pressure_current_,idx,intQuants);
                    get_current_density_cell(rhow_,idx,intQuants);
                    calculate_inflow_rate(idx, timer);
                    qinflow = Qai_.at(idx);
                    ebosResid[*cellID][(FluidSystem::waterCompIdx)] -= qinflow.value();

                    for (int pvIdx = 0; pvIdx < numEq; ++pvIdx) 
                    {
                        // also need to consider the efficiency factor when manipulating the jacobians.
                        ebosJac[*cellID][*cellID][(FluidSystem::waterCompIdx)][pvIdx] -= qinflow.derivative(pvIdx);
                    }
                }
            }

            inline void before_time_step(Simulator& ebosSimulator, const SimulatorTimerInterface& timer)
            {
                auto cellID = cell_idx_.begin();
                size_t idx;
                for ( idx = 0; cellID != cell_idx_.end(); ++cellID, ++idx )
                {
                    const auto& intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(*cellID, /*timeIdx=*/ 0));
                    get_current_Pressure_cell(pressure_previous_ ,idx,intQuants);
                }
            }

            inline void after_time_step(const SimulatorTimerInterface& timer)
            {
                for (auto Qai = Qai_.begin(); Qai != Qai_.end(); ++Qai)
                {
                    W_flux_ += (*Qai)*timer.currentStepLength();
                }
            }

            inline const double area_fraction(const size_t i)
            {
                return alphai_.at(i);
            }

            inline const std::vector<int> cell_id() const
            {
                return cell_idx_;
            }

            inline const int& aquiferID() const
            {
                return aquiferID_;
            }


        private:
            const PhaseUsage* phase_usage_;
            const Simulator& ebos_simulator_;


            // Aquifer ID, and other IDs
            int aquiferID_, inftableID_, pvttableID_;
            int num_components_;

            // Grid variables

            std::vector<size_t> cell_idx_;
            std::vector<Scalar> faceArea_connected_;

            // Quantities at each grid id
            std::vector<Scalar> cell_depth_;
            std::vector<Eval> pressure_previous_;
            std::vector<Eval> pressure_current_;
            std::vector<Eval> Qai_;
            std::vector<Eval> rhow_;
            std::vector<Scalar> alphai_;

            // Variables constants
            Scalar mu_w_ , //water viscosity
                   phi_aq_ , //aquifer porosity
                   d0_,     // aquifer datum depth
                   C_t_ , //total compressibility
                   r_o_ , //aquifer inner radius
                   k_a_ , //aquifer permeability
                   c1_, // 0.008527 (METRIC, PVT-M); 0.006328 (FIELD); 3.6 (LAB)
                   h_ , //aquifer thickness
                   theta_ , //angle subtended by the aquifer boundary
                   c2_ ; //6.283 (METRIC, PVT-M); 1.1191 (FIELD); 6.283 (LAB).

            // Variables for influence table
            std::vector<Scalar> aqutab_td_, aqutab_pi_;

            // Cumulative flux
            Scalar dt_, pa0_, gravity_;
            bool p0_defaulted_;
            Eval W_flux_;
            // Also return the polynomial fit
            std::vector<Scalar> coeff_;
            

            // We fit the tabular data using a polynomial fit
            // Modified from Copyright (C) 2014  Clifford Wolf <clifford@clifford.at> 
            // http://svn.clifford.at/handicraft/2014/polyfit/polyfit.cc
            inline void polynomial_fit( const std::vector<Scalar> &X, const std::vector<Scalar> &y, 
                                        std::vector<Scalar> &coeff, int order, bool bias) const
            {
                size_t colNum = (bias)? order + 1 : order;
                Eigen::MatrixXd A(X.size(), colNum);
                Eigen::VectorXd y_mapped = Eigen::VectorXd::Map(&y.front(), y.size());
                Eigen::VectorXd result;

                assert(X.size() == y.size());
                assert(X.size() >= colNum);

                // create matrix
                for (size_t i = 0; i < X.size(); i++)
                for (size_t j = 0; j < colNum; j++)
                    A(i, j) = (bias)? pow(X.at(i), j) : pow(X.at(i), j+1);

                // solve for linear least squares fit
                result = A.householderQr().solve(y_mapped);

                coeff.resize(colNum);
                for (size_t i = 0; i < colNum; i++)
                    coeff[i] = result[i];
            }

            inline void get_influence_table_values(Scalar& pitd, Scalar& pitd_prime, const Scalar& td)
            {
                // http://kluge.in-chemnitz.de/opensource/spline/
            }

            inline void init_quantities(const Aquancon::AquanconOutput& connection)
            {
                // We reset the cumulative flux at the start of any simulation, so, W_flux = 0
                W_flux_ = 0.;

                // We next get our connections to the aquifer and initialize these quantities using the initialize_connections function
                initialize_connections(connection);

                calculate_aquifer_condition();

                pressure_previous_.resize(cell_idx_.size(), 0.);
                pressure_current_.resize(cell_idx_.size(), 0.);
                Qai_.resize(cell_idx_.size(), 0.0);

                polynomial_fit(aqutab_td_, aqutab_pi_, coeff_, 1, true);
            }

            inline void get_current_Pressure_cell(std::vector<Eval>& pressure_water, const int idx, const IntensiveQuantities& intQuants)
            {
                const auto& fs = intQuants.fluidState();
                pressure_water.at(idx) = fs.pressure(FluidSystem::waterPhaseIdx);
            }

            inline void get_current_density_cell(std::vector<Eval>& rho_water, const int idx, const IntensiveQuantities& intQuants)
            {
                const auto& fs = intQuants.fluidState();
                rho_water.at(idx) = fs.density(FluidSystem::waterPhaseIdx);
            }

            inline Scalar dpai(int idx)
            {
                Scalar dp = pa0_ + rhow_.at(idx).value()*gravity_*(cell_depth_.at(idx) - d0_) - pressure_previous_.at(idx).value();
                return dp;
            }

            // This function implements Eqs 5.8 and 5.9 of the EclipseTechnicalDescription
            inline void calculate_a_b_constants(Scalar& a, Scalar& b, const int idx, const SimulatorTimerInterface& timer)
            {
                Scalar beta = aquifer_influx_constant();
                Scalar Tc = time_constant();
                Scalar td_plus_dt = (timer.currentStepLength() + timer.simulationTimeElapsed()) / Tc;
                Scalar td = timer.simulationTimeElapsed() / Tc;

                Scalar PItdprime = coeff_.at(1);
                Scalar PItd = coeff_.at(0) + coeff_.at(1)*td_plus_dt;
                a = 1.0/Tc * ( (beta * dpai(idx)) - (W_flux_.value() * PItdprime) ) / ( PItd - td*PItdprime );
                b = beta / (Tc * ( PItd - td*PItdprime));
            }

            // This function implements Eq 5.7 of the EclipseTechnicalDescription       
            inline void calculate_inflow_rate(int idx, const SimulatorTimerInterface& timer)
            {
                Scalar a, b;
                calculate_a_b_constants(a,b,idx,timer);
                Qai_.at(idx) = area_fraction(idx)*( a - b * ( pressure_current_.at(idx) - pressure_previous_.at(idx).value() ) );
            }

            inline const Scalar time_constant() const
            {
                Scalar Tc = mu_w_*phi_aq_*C_t_*r_o_*r_o_/(k_a_*c1_);
                return Tc;
            }

            inline const Scalar aquifer_influx_constant() const
            {
                Scalar beta = c2_*h_*theta_*phi_aq_*C_t_*r_o_*r_o_;
                return beta;
            }

            // This function is used to initialize and calculate the alpha_i for each grid connection to the aquifer
            inline void initialize_connections(const Aquancon::AquanconOutput& connection)
            {
                const auto& eclState = ebos_simulator_.vanguard().eclState();
                const auto& ugrid = ebos_simulator_.vanguard().grid();
                const auto& grid = eclState.getInputGrid();

                cell_idx_ = connection.global_index;

                assert( cell_idx_ == connection.global_index);
                assert( (cell_idx_.size() == connection.influx_coeff.size()) );
                assert( (connection.influx_coeff.size() == connection.influx_multiplier.size()) );
                assert( (connection.influx_multiplier.size() == connection.reservoir_face_dir.size()) );

                // We hack the cell depth values for now. We can actually get it from elementcontext pos
                cell_depth_.resize(cell_idx_.size(), d0_);
                alphai_.resize(cell_idx_.size(), 1.0);
                faceArea_connected_.resize(cell_idx_.size(),0.0);

                auto cell2Faces = Opm::UgGridHelpers::cell2Faces(ugrid);
                auto faceCells  = Opm::AutoDiffGrid::faceCells(ugrid);

                // Translate the C face tag into the enum used by opm-parser's TransMult class
                Opm::FaceDir::DirEnum faceDirection;

                // denom_face_areas is the sum of the areas connected to an aquifer
                Scalar denom_face_areas = 0.;
                for (size_t idx = 0; idx < cell_idx_.size(); ++idx)
                {
                    auto cellFacesRange = cell2Faces[cell_idx_.at(idx)];
                    
                    for(auto cellFaceIter = cellFacesRange.begin(); cellFaceIter != cellFacesRange.end(); ++cellFaceIter)
                    {
                        // The index of the face in the compressed grid
                        const int faceIdx = *cellFaceIter;

                        // the logically-Cartesian direction of the face
                        const int faceTag = Opm::UgGridHelpers::faceTag(ugrid, cellFaceIter);

                        
                        if (faceTag == 0) // left
                            faceDirection = Opm::FaceDir::XMinus;
                        else if (faceTag == 1) // right
                            faceDirection = Opm::FaceDir::XPlus;
                        else if (faceTag == 2) // back
                            faceDirection = Opm::FaceDir::YMinus;
                        else if (faceTag == 3) // front
                            faceDirection = Opm::FaceDir::YPlus;
                        else if (faceTag == 4) // bottom
                            faceDirection = Opm::FaceDir::ZMinus;
                        else if (faceTag == 5) // top
                            faceDirection = Opm::FaceDir::ZPlus;

                        if (faceDirection == connection.reservoir_face_dir.at(idx))
                        {
                            faceArea_connected_.at(idx) = Opm::UgGridHelpers::faceArea(ugrid, faceIdx);
                            denom_face_areas += faceArea_connected_.at(idx);
                        }
                    }
                    auto cellCenter = grid.getCellCenter(cell_idx_.at(idx));
                    cell_depth_.at(idx) = cellCenter[2];
                }

                for (size_t idx = 0; idx < cell_idx_.size(); ++idx)
                {
                    alphai_.at(idx) = faceArea_connected_.at(idx)/denom_face_areas;
                }
            }

            inline void calculate_aquifer_condition()
            {

                int pvttableIdx = pvttableID_ - 1;
                
                rhow_.resize(cell_idx_.size(),0.);
                
                if (p0_defaulted_)
                {
                   pa0_ = calculate_reservoir_equilibrium(rhow_);
                }

                // Initialize a FluidState object first
                FluidState fs_aquifer;
                // We use the temperature of the first cell connected to the aquifer
                // Here we copy the fluidstate of the first cell, so we do not accidentally mess up the reservoir fs
                fs_aquifer.assign( ebos_simulator_.model().cachedIntensiveQuantities(cell_idx_.at(0), /*timeIdx=*/ 0)->fluidState() );
                Eval temperature_aq, pa0_mean;
                temperature_aq = fs_aquifer.temperature(0);
                pa0_mean = pa0_;

                // rho_mean = FluidSystem::referenceDensity(waterPhaseIdx, pvttableIdx)
                //             *FluidSystem::waterPvt().inverseFormationVolumeFactor(pvttableIdx, temperature_aq, pa0_mean);

                Eval mu_w_aquifer = FluidSystem::waterPvt().viscosity(pvttableIdx, temperature_aq, pa0_mean);
                std::cout << "Pa0 = " << pa0_mean << ", viscosity = " << mu_w_aquifer.value() << std::endl;

                mu_w_ = mu_w_aquifer.value();

            }

            // This function is for calculating the aquifer properties from equilibrium state with the reservoir
            inline Scalar calculate_reservoir_equilibrium(std::vector<Eval>& rho_water_reservoir)
            {
                // Since the global_indices are the reservoir index, we just need to extract the fluidstate at those indices
                std::vector<Scalar> water_pressure_reservoir, pw_aquifer;

                for (size_t idx = 0; idx < cell_idx_.size(); ++idx)
                {
                    size_t cellIDx = cell_idx_.at(idx);
                    const auto& intQuants = *(ebos_simulator_.model().cachedIntensiveQuantities(cellIDx, /*timeIdx=*/ 0));
                    const auto& fs = intQuants.fluidState();
                    
                    water_pressure_reservoir.push_back( fs.pressure(FluidSystem::waterPhaseIdx).value() );
                    rho_water_reservoir.at(idx) = fs.density(FluidSystem::waterPhaseIdx);
                    pw_aquifer.push_back( (water_pressure_reservoir.at(idx) - rho_water_reservoir.at(idx).value()*gravity_*(cell_depth_.at(idx) - d0_))*area_fraction(idx) );
                }

                // We take the average of the calculated equilibrium pressures.
                Scalar aquifer_pres_avg = std::accumulate(pw_aquifer.begin(), pw_aquifer.end(), 0.)/pw_aquifer.size();
                return aquifer_pres_avg;
            }


    }; // class AquiferCarterTracy


} // namespace Opm

#endif