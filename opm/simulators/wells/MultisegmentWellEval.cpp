/*
  Copyright 2017 SINTEF Digital, Mathematics and Cybernetics.
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

#include <config.h>

#include <opm/simulators/wells/MultisegmentWellEval.hpp>

#include <dune/istl/umfpack.hh>

#include <opm/material/fluidsystems/BlackOilFluidSystem.hpp>

#include <opm/models/blackoil/blackoilindices.hh>
#include <opm/models/blackoil/blackoilonephaseindices.hh>
#include <opm/models/blackoil/blackoiltwophaseindices.hh>

#include <opm/simulators/timestepping/ConvergenceReport.hpp>
#include <opm/simulators/utils/DeferredLoggingErrorHelpers.hpp>
#include <opm/simulators/wells/MSWellHelpers.hpp>
#include <opm/simulators/wells/MultisegmentWellAssemble.hpp>
#include <opm/simulators/wells/WellAssemble.hpp>
#include <opm/simulators/wells/WellConvergence.hpp>
#include <opm/simulators/wells/WellInterfaceIndices.hpp>
#include <opm/simulators/wells/WellState.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <numeric>
#include <utility>
#include <vector>

namespace Opm
{

template<typename FluidSystem, typename Indices, typename Scalar>
MultisegmentWellEval<FluidSystem,Indices,Scalar>::
MultisegmentWellEval(WellInterfaceIndices<FluidSystem,Indices,Scalar>& baseif)
    : MultisegmentWellGeneric<Scalar>(baseif)
    , baseif_(baseif)
    , linSys_(*this)
    , primary_variables_(baseif)
    , segments_(this->numberOfSegments(), baseif)
    , cell_perforation_depth_diffs_(baseif_.numPerfs(), 0.0)
    , cell_perforation_pressure_diffs_(baseif_.numPerfs(), 0.0)
{
}

template<typename FluidSystem, typename Indices, typename Scalar>
void
MultisegmentWellEval<FluidSystem,Indices,Scalar>::
initMatrixAndVectors(const int num_cells)
{
    linSys_.init(num_cells, baseif_.numPerfs(),
                 baseif_.cells(), segments_.inlets_,
                 segments_.perforations_);
    primary_variables_.resize(this->numberOfSegments());
}

template<typename FluidSystem, typename Indices, typename Scalar>
ConvergenceReport
MultisegmentWellEval<FluidSystem,Indices,Scalar>::
getWellConvergence(const WellState& well_state,
                   const std::vector<double>& B_avg,
                   DeferredLogger& deferred_logger,
                   const double max_residual_allowed,
                   const double tolerance_wells,
                   const double relaxed_inner_tolerance_flow_ms_well,
                   const double tolerance_pressure_ms_wells,
                   const double relaxed_inner_tolerance_pressure_ms_well,
                   const bool relax_tolerance) const
{
    assert(int(B_avg.size()) == baseif_.numComponents());

    // checking if any residual is NaN or too large. The two large one is only handled for the well flux
    std::vector<std::vector<double>> abs_residual(this->numberOfSegments(),
                                                  std::vector<double>(numWellEq, 0.0));
    for (int seg = 0; seg < this->numberOfSegments(); ++seg) {
        for (int eq_idx = 0; eq_idx < numWellEq; ++eq_idx) {
            abs_residual[seg][eq_idx] = std::abs(linSys_.residual()[seg][eq_idx]);
        }
    }

    std::vector<double> maximum_residual(numWellEq, 0.0);

    ConvergenceReport report;
    // TODO: the following is a little complicated, maybe can be simplified in some way?
    for (int eq_idx = 0; eq_idx < numWellEq; ++eq_idx) {
        for (int seg = 0; seg < this->numberOfSegments(); ++seg) {
            if (eq_idx < baseif_.numComponents()) { // phase or component mass equations
                const double flux_residual = B_avg[eq_idx] * abs_residual[seg][eq_idx];
                if (flux_residual > maximum_residual[eq_idx]) {
                    maximum_residual[eq_idx] = flux_residual;
                }
            } else { // pressure or control equation
                // for the top segment (seg == 0), it is control equation, will be checked later separately
                if (seg > 0) {
                    // Pressure equation
                    const double pressure_residual = abs_residual[seg][eq_idx];
                    if (pressure_residual > maximum_residual[eq_idx]) {
                        maximum_residual[eq_idx] = pressure_residual;
                    }
                }
            }
        }
    }

    using CR = ConvergenceReport;
    for (int eq_idx = 0; eq_idx < numWellEq; ++eq_idx) {
        if (eq_idx < baseif_.numComponents()) { // phase or component mass equations
            const double flux_residual = maximum_residual[eq_idx];
            // TODO: the report can not handle the segment number yet.

            if (std::isnan(flux_residual)) {
                report.setWellFailed({CR::WellFailure::Type::MassBalance, CR::Severity::NotANumber, eq_idx, baseif_.name()});
            } else if (flux_residual > max_residual_allowed) {
                report.setWellFailed({CR::WellFailure::Type::MassBalance, CR::Severity::TooLarge, eq_idx, baseif_.name()});
            } else if (!relax_tolerance && flux_residual > tolerance_wells) {
                report.setWellFailed({CR::WellFailure::Type::MassBalance, CR::Severity::Normal, eq_idx, baseif_.name()});
            } else if (flux_residual > relaxed_inner_tolerance_flow_ms_well) {
                report.setWellFailed({CR::WellFailure::Type::MassBalance, CR::Severity::Normal, eq_idx, baseif_.name()});
            }
        } else { // pressure equation
            const double pressure_residual = maximum_residual[eq_idx];
            const int dummy_component = -1;
            if (std::isnan(pressure_residual)) {
                report.setWellFailed({CR::WellFailure::Type::Pressure, CR::Severity::NotANumber, dummy_component, baseif_.name()});
            } else if (std::isinf(pressure_residual)) {
                report.setWellFailed({CR::WellFailure::Type::Pressure, CR::Severity::TooLarge, dummy_component, baseif_.name()});
            } else if (!relax_tolerance && pressure_residual > tolerance_pressure_ms_wells) {
                report.setWellFailed({CR::WellFailure::Type::Pressure, CR::Severity::Normal, dummy_component, baseif_.name()});
            } else if (pressure_residual > relaxed_inner_tolerance_pressure_ms_well) {
                report.setWellFailed({CR::WellFailure::Type::Pressure, CR::Severity::Normal, dummy_component, baseif_.name()});
            }
        }
    }

    WellConvergence(baseif_).
        checkConvergenceControlEq(well_state,
                                  {tolerance_pressure_ms_wells,
                                   tolerance_pressure_ms_wells,
                                   tolerance_wells,
                                   tolerance_wells,
                                   max_residual_allowed},
                                  std::abs(linSys_.residual()[0][SPres]),
                                  report,
                                  deferred_logger);

    return report;
}

template<typename FluidSystem, typename Indices, typename Scalar>
typename MultisegmentWellEval<FluidSystem,Indices,Scalar>::EvalWell
MultisegmentWellEval<FluidSystem,Indices,Scalar>::
extendEval(const Eval& in) const
{
    EvalWell out = 0.0;
    out.setValue(in.value());
    for(int eq_idx = 0; eq_idx < Indices::numEq;++eq_idx) {
        out.setDerivative(eq_idx, in.derivative(eq_idx));
    }
    return out;
}

template<typename FluidSystem, typename Indices, typename Scalar>
void
MultisegmentWellEval<FluidSystem,Indices,Scalar>::
handleAccelerationPressureLoss(const int seg,
                               WellState& well_state)
{
    const double area = this->segmentSet()[seg].crossArea();
    const EvalWell mass_rate = segments_.mass_rates_[seg];
    const int seg_upwind = segments_.upwinding_segments_[seg];
    EvalWell density = segments_.densities_[seg_upwind];
    // WARNING
    // We disregard the derivatives from the upwind density to make sure derivatives
    // wrt. to different segments dont get mixed.
    if (seg != seg_upwind) {
        density.clearDerivatives();
    }

    EvalWell accelerationPressureLoss = mswellhelpers::velocityHead(area, mass_rate, density);
    // handling the velocity head of intlet segments
    for (const int inlet : this->segments_.inlets_[seg]) {
        const int seg_upwind_inlet = segments_.upwinding_segments_[inlet];
        const double inlet_area = this->segmentSet()[inlet].crossArea();
        EvalWell inlet_density = this->segments_.densities_[seg_upwind_inlet];
        // WARNING
        // We disregard the derivatives from the upwind density to make sure derivatives
        // wrt. to different segments dont get mixed.
        if (inlet != seg_upwind_inlet) {
            inlet_density.clearDerivatives();
        }
        const EvalWell inlet_mass_rate = segments_.mass_rates_[inlet];
        accelerationPressureLoss -= mswellhelpers::velocityHead(std::max(inlet_area, area), inlet_mass_rate, inlet_density);
    }

    // We change the sign of the accelerationPressureLoss for injectors.
    // Is this correct? Testing indicates that this is what the reference simulator does
    const double sign = mass_rate < 0. ? 1.0 : - 1.0;
    accelerationPressureLoss *= sign;

    auto& segments = well_state.well(baseif_.indexOfWell()).segments;
    segments.pressure_drop_accel[seg] = accelerationPressureLoss.value();

    MultisegmentWellAssemble<FluidSystem,Indices,Scalar>(baseif_).
        assemblePressureLoss(seg, seg_upwind, accelerationPressureLoss, linSys_);
}

template<typename FluidSystem, typename Indices, typename Scalar>
void
MultisegmentWellEval<FluidSystem,Indices,Scalar>::
assembleDefaultPressureEq(const int seg,
                          WellState& well_state)
{
    assert(seg != 0); // not top segment

    // for top segment, the well control equation will be used.
    EvalWell pressure_equation = primary_variables_.getSegmentPressure(seg);

    // we need to handle the pressure difference between the two segments
    // we only consider the hydrostatic pressure loss first
    // TODO: we might be able to add member variables to store these values, then we update well state
    // after converged
    const auto hydro_pressure_drop = segments_.getHydroPressureLoss(seg);
    auto& ws = well_state.well(baseif_.indexOfWell());
    auto& segments = ws.segments;
    segments.pressure_drop_hydrostatic[seg] = hydro_pressure_drop.value();
    pressure_equation -= hydro_pressure_drop;

    if (this->frictionalPressureLossConsidered()) {
        const auto friction_pressure_drop = segments_.getFrictionPressureLoss(seg);
        pressure_equation -= friction_pressure_drop;
        segments.pressure_drop_friction[seg] = friction_pressure_drop.value();
    }

    // contribution from the outlet segment
    const int outlet_segment_index = this->segmentNumberToIndex(this->segmentSet()[seg].outletSegment());
    const EvalWell outlet_pressure = primary_variables_.getSegmentPressure(outlet_segment_index);

    const int seg_upwind = segments_.upwinding_segments_[seg];
    MultisegmentWellAssemble<FluidSystem,Indices,Scalar>(baseif_).
        assemblePressureEq(seg, seg_upwind, outlet_segment_index,
                           pressure_equation, outlet_pressure, linSys_);

    if (this->accelerationalPressureLossConsidered()) {
        handleAccelerationPressureLoss(seg, well_state);
    }
}

template<typename FluidSystem, typename Indices, typename Scalar>
void
MultisegmentWellEval<FluidSystem,Indices,Scalar>::
assembleICDPressureEq(const int seg,
                      const UnitSystem& unit_system,
                      WellState& well_state,
                      DeferredLogger& deferred_logger)
{
    // TODO: upwinding needs to be taken care of
    // top segment can not be a spiral ICD device
    assert(seg != 0);

    if (const auto& segment = this->segmentSet()[seg];
       (segment.segmentType() == Segment::SegmentType::VALVE) &&
       (segment.valve().status() == Opm::ICDStatus::SHUT) ) { // we use a zero rate equation to handle SHUT valve
        MultisegmentWellAssemble<FluidSystem,Indices,Scalar>(baseif_).
            assembleTrivialEq(seg, this->primary_variables_.eval(seg)[WQTotal].value(), linSys_);

        auto& ws = well_state.well(baseif_.indexOfWell());
        ws.segments.pressure_drop_friction[seg] = 0.;
        return;
    }

    // the pressure equation is something like
    // p_seg - deltaP - p_outlet = 0.
    // the major part is how to calculate the deltaP

    EvalWell pressure_equation = primary_variables_.getSegmentPressure(seg);

    EvalWell icd_pressure_drop;
    switch(this->segmentSet()[seg].segmentType()) {
        case Segment::SegmentType::SICD :
            icd_pressure_drop = segments_.pressureDropSpiralICD(seg);
            break;
        case Segment::SegmentType::AICD :
            icd_pressure_drop = segments_.pressureDropAutoICD(seg, unit_system);
            break;
        case Segment::SegmentType::VALVE :
            icd_pressure_drop = segments_.pressureDropValve(seg);
            break;
        default: {
            OPM_DEFLOG_THROW(std::runtime_error, "Segment " + std::to_string(this->segmentSet()[seg].segmentNumber())
                             + " for well " + baseif_.name() + " is not of ICD type", deferred_logger);
        }
    }
    pressure_equation = pressure_equation - icd_pressure_drop;
    auto& ws = well_state.well(baseif_.indexOfWell());
    ws.segments.pressure_drop_friction[seg] = icd_pressure_drop.value();

    // contribution from the outlet segment
    const int outlet_segment_index = this->segmentNumberToIndex(this->segmentSet()[seg].outletSegment());
    const EvalWell outlet_pressure = primary_variables_.getSegmentPressure(outlet_segment_index);

    const int seg_upwind = segments_.upwinding_segments_[seg];
    MultisegmentWellAssemble<FluidSystem,Indices,Scalar>(baseif_).
        assemblePressureEq(seg, seg_upwind, outlet_segment_index,
                           pressure_equation, outlet_pressure,
                           linSys_,
                           FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx),
                           FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx));
}

template<typename FluidSystem, typename Indices, typename Scalar>
void
MultisegmentWellEval<FluidSystem,Indices,Scalar>::
assemblePressureEq(const int seg,
                   const UnitSystem& unit_system,
                   WellState& well_state,
                   DeferredLogger& deferred_logger)
{
    switch(this->segmentSet()[seg].segmentType()) {
        case Segment::SegmentType::SICD :
        case Segment::SegmentType::AICD :
        case Segment::SegmentType::VALVE : {
            assembleICDPressureEq(seg, unit_system, well_state,deferred_logger);
            break;
        }
        default :
            assembleDefaultPressureEq(seg, well_state);
    }
}

template<typename FluidSystem, typename Indices, typename Scalar>
std::pair<bool, std::vector<Scalar> >
MultisegmentWellEval<FluidSystem,Indices,Scalar>::
getFiniteWellResiduals(const std::vector<Scalar>& B_avg,
                       DeferredLogger& deferred_logger) const
{
    assert(int(B_avg.size() ) == baseif_.numComponents());
    std::vector<Scalar> residuals(numWellEq + 1, 0.0);

    for (int seg = 0; seg < this->numberOfSegments(); ++seg) {
        for (int eq_idx = 0; eq_idx < numWellEq; ++eq_idx) {
            double residual = 0.;
            if (eq_idx < baseif_.numComponents()) {
                residual = std::abs(linSys_.residual()[seg][eq_idx]) * B_avg[eq_idx];
            } else {
                if (seg > 0) {
                    residual = std::abs(linSys_.residual()[seg][eq_idx]);
                }
            }
            if (std::isnan(residual) || std::isinf(residual)) {
                deferred_logger.debug("nan or inf value for residal get for well " + baseif_.name()
                                      + " segment " + std::to_string(seg) + " eq_idx " + std::to_string(eq_idx));
                return {false, residuals};
            }

            if (residual > residuals[eq_idx]) {
                residuals[eq_idx] = residual;
            }
        }
    }

    // handling the control equation residual
    {
        const double control_residual = std::abs(linSys_.residual()[0][numWellEq - 1]);
        if (std::isnan(control_residual) || std::isinf(control_residual)) {
           deferred_logger.debug("nan or inf value for control residal get for well " + baseif_.name());
           return {false, residuals};
        }
        residuals[numWellEq] = control_residual;
    }

    return {true, residuals};
}

template<typename FluidSystem, typename Indices, typename Scalar>
double
MultisegmentWellEval<FluidSystem,Indices,Scalar>::
getControlTolerance(const WellState& well_state,
                    const double tolerance_wells,
                    const double tolerance_pressure_ms_wells,
                    DeferredLogger& deferred_logger) const
{
    double control_tolerance = 0.;

    const int well_index = baseif_.indexOfWell();
    const auto& ws = well_state.well(well_index);
    if (baseif_.isInjector() )
    {
        auto current = ws.injection_cmode;
        switch(current) {
        case Well::InjectorCMode::THP:
            control_tolerance = tolerance_pressure_ms_wells;
            break;
        case Well::InjectorCMode::BHP:
            control_tolerance = tolerance_wells;
            break;
        case Well::InjectorCMode::RATE:
        case Well::InjectorCMode::RESV:
            control_tolerance = tolerance_wells;
            break;
        case Well::InjectorCMode::GRUP:
            control_tolerance = tolerance_wells;
            break;
        default:
            OPM_DEFLOG_THROW(std::runtime_error, "Unknown well control control types for well " << baseif_.name(), deferred_logger);
        }
    }

    if (baseif_.isProducer() )
    {
        auto current = ws.production_cmode;
        switch(current) {
        case Well::ProducerCMode::THP:
            control_tolerance = tolerance_pressure_ms_wells; // 0.1 bar
            break;
        case Well::ProducerCMode::BHP:
            control_tolerance = tolerance_wells; // 0.01 bar
            break;
        case Well::ProducerCMode::ORAT:
        case Well::ProducerCMode::WRAT:
        case Well::ProducerCMode::GRAT:
        case Well::ProducerCMode::LRAT:
        case Well::ProducerCMode::RESV:
        case Well::ProducerCMode::CRAT:
            control_tolerance = tolerance_wells; // smaller tolerance for rate control
            break;
        case Well::ProducerCMode::GRUP:
            control_tolerance = tolerance_wells; // smaller tolerance for rate control
            break;
        default:
            OPM_DEFLOG_THROW(std::runtime_error, "Unknown well control control types for well " << baseif_.name(), deferred_logger);
        }
    }

    return control_tolerance;
}

template<typename FluidSystem, typename Indices, typename Scalar>
double
MultisegmentWellEval<FluidSystem,Indices,Scalar>::
getResidualMeasureValue(const WellState& well_state,
                        const std::vector<double>& residuals,
                        const double tolerance_wells,
                        const double tolerance_pressure_ms_wells,
                        DeferredLogger& deferred_logger) const
{
    assert(int(residuals.size()) == numWellEq + 1);

    const double rate_tolerance = tolerance_wells;
    int count = 0;
    double sum = 0;
    for (int eq_idx = 0; eq_idx < numWellEq - 1; ++eq_idx) {
        if (residuals[eq_idx] > rate_tolerance) {
            sum += residuals[eq_idx] / rate_tolerance;
            ++count;
        }
    }

    const double pressure_tolerance = tolerance_pressure_ms_wells;
    if (residuals[SPres] > pressure_tolerance) {
        sum += residuals[SPres] / pressure_tolerance;
        ++count;
    }

    const double control_tolerance = getControlTolerance(well_state,
                                                         tolerance_wells,
                                                         tolerance_pressure_ms_wells,
                                                         deferred_logger);
    if (residuals[SPres + 1] > control_tolerance) {
        sum += residuals[SPres + 1] / control_tolerance;
        ++count;
    }

    // if (count == 0), it should be converged.
    assert(count != 0);

    return sum;
}

#define INSTANCE(...) \
template class MultisegmentWellEval<BlackOilFluidSystem<double,BlackOilDefaultIndexTraits>,__VA_ARGS__,double>;

// One phase
INSTANCE(BlackOilOnePhaseIndices<0u,0u,0u,0u,false,false,0u,1u,0u>)
INSTANCE(BlackOilOnePhaseIndices<0u,0u,0u,1u,false,false,0u,1u,0u>)
INSTANCE(BlackOilOnePhaseIndices<0u,0u,0u,0u,false,false,0u,1u,5u>)

// Two phase
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,0u,false,false,0u,0u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,0u,false,false,0u,1u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,0u,false,false,0u,2u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,0u,false,true,0u,2u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,1u,0u,false,false,0u,2u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,2u,0u,false,false,0u,2u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,1u,false,false,0u,1u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,0u,false,true,0u,0u,0u>)

// Blackoil
INSTANCE(BlackOilIndices<0u,0u,0u,0u,false,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,0u,true,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,0u,false,true,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,0u,false,true,2u,0u>)
INSTANCE(BlackOilIndices<1u,0u,0u,0u,false,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,1u,0u,0u,false,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,1u,0u,false,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,1u,false,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,0u,false,false,1u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,1u,false,true,0u,0u>)

} // namespace Opm
