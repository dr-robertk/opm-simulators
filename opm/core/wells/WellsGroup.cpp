/*
  Copyright 2012 SINTEF ICT, Applied Mathematics.
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

#include "config.h"
#include <opm/core/wells/WellsGroup.hpp>
#include <opm/core/wells.h>
#include <opm/core/well_controls.h>
#include <opm/core/props/phaseUsageFromDeck.hpp>

#include <cmath>
#include <memory>
#include <iostream>

namespace
{
    static double invalid_alq = -1e100;
    static double invalid_vfp = -2147483647;
} //Namespace

namespace Opm
{

    // ==========   WellPhasesSummed methods   ===========

    WellPhasesSummed::WellPhasesSummed()
    {
        for (int i = 0; i < 3; ++i) {
            res_inj_rates[i] = 0.0;
            res_prod_rates[i] = 0.0;
            surf_inj_rates[i] = 0.0;
            surf_prod_rates[i] = 0.0;
        }
    }

    void WellPhasesSummed::operator+=(const WellPhasesSummed& other)
    {
        for (int i = 0; i < 3; ++i) {
            res_inj_rates[i] += other.res_inj_rates[i];
            res_prod_rates[i] += other.res_prod_rates[i];
            surf_inj_rates[i] += other.surf_inj_rates[i];
            surf_prod_rates[i] += other.surf_prod_rates[i];
        }
    }

    // ==========   WellsGroupInterface methods   ===========


    WellsGroupInterface::WellsGroupInterface(const std::string& myname,
                                             const double efficicency_factor,
                                             const ProductionSpecification& prod_spec,
                                             const InjectionSpecification& inje_spec,
                                             const PhaseUsage& phase_usage)
        : parent_(NULL),
          should_update_well_targets_(false),
          individual_control_(true), // always begin with individual control
          efficicency_factor_(efficicency_factor),
          name_(myname),
          production_specification_(prod_spec),
          injection_specification_(inje_spec),
          phase_usage_(phase_usage)
    {
    }

    WellsGroupInterface::~WellsGroupInterface()
    {
    }

    const WellsGroupInterface* WellsGroupInterface::getParent() const
    {
        return parent_;
    }

    WellsGroupInterface* WellsGroupInterface::getParent()
    {
        return parent_;
    }

    const std::string& WellsGroupInterface::name() const
    {
        return name_;
    }

    const PhaseUsage& WellsGroupInterface::phaseUsage() const
    {
        return phase_usage_;
    }

    bool WellsGroupInterface::isLeafNode() const
    {
        return false;
    }

    void WellsGroupInterface::setParent(WellsGroupInterface* parent)
    {
        parent_ = parent;
    }

    const ProductionSpecification& WellsGroupInterface::prodSpec() const
    {
        return production_specification_;
    }

    /// Injection specifications for the well or well group.

    const InjectionSpecification& WellsGroupInterface::injSpec() const
    {
        return injection_specification_;
    }

    /// Production specifications for the well or well group.

    ProductionSpecification& WellsGroupInterface::prodSpec()
    {
        return production_specification_;
    }

    /// Injection specifications for the well or well group.

    InjectionSpecification& WellsGroupInterface::injSpec()
    {
        return injection_specification_;
    }

    /// Calculates the correct rate for the given ProductionSpecification::ControlMode
    double WellsGroupInterface::rateByMode(const double* res_rates,
                                           const double* surf_rates,
                                           const ProductionSpecification::ControlMode mode)
    {
        switch (mode) {
        case ProductionSpecification::ORAT:
            return surf_rates[phaseUsage().phase_pos[BlackoilPhases::Liquid]];
        case ProductionSpecification::WRAT:
            return surf_rates[phaseUsage().phase_pos[BlackoilPhases::Aqua]];
        case ProductionSpecification::GRAT:
            return surf_rates[phaseUsage().phase_pos[BlackoilPhases::Vapour]];
        case ProductionSpecification::LRAT:
            return surf_rates[phaseUsage().phase_pos[BlackoilPhases::Liquid]]
                + surf_rates[phaseUsage().phase_pos[BlackoilPhases::Aqua]];
        case ProductionSpecification::RESV:
            {
                double tot_rate = 0.0;
                for (int phase = 0; phase < phaseUsage().num_phases; ++phase) {
                    tot_rate += res_rates[phase];
                }
                return tot_rate;
            }
        default:
            OPM_THROW(std::runtime_error, "No rate associated with production control mode" << mode);
        }
    }

    /// Calculates the correct rate for the given InjectionSpecification::ControlMode
    double WellsGroupInterface::rateByMode(const double* res_rates,
                                           const double* surf_rates,
                                           const InjectionSpecification::ControlMode mode)
    {
        const double* rates = 0;
        switch (mode) {
        case InjectionSpecification::RATE:
            rates = surf_rates;
            break;
        case InjectionSpecification::RESV:
            rates = res_rates;
            break;
        default:
            OPM_THROW(std::runtime_error, "No rate associated with injection control mode" << mode);
        }
        double tot_rate = 0.0;
        for (int phase = 0; phase < phaseUsage().num_phases; ++phase) {
            tot_rate += rates[phase];
        }
        return tot_rate;
    }

    double WellsGroupInterface::getTarget(ProductionSpecification::ControlMode mode)
    {
        double target = -1.0;
        switch (mode) {
        case ProductionSpecification::GRAT:
            target = prodSpec().gas_max_rate_;
            break;
        case ProductionSpecification::WRAT:
            target = prodSpec().water_max_rate_;
            break;
        case ProductionSpecification::ORAT:
            target = prodSpec().oil_max_rate_;
            break;
        case ProductionSpecification::RESV:
            target = prodSpec().reservoir_flow_max_rate_;
            break;
        case ProductionSpecification::LRAT:
            target = prodSpec().liquid_max_rate_;
            break;
        case ProductionSpecification::GRUP:
            OPM_THROW(std::runtime_error, "Can't query target production rate for GRUP control keyword");
            break;
        default:
            OPM_THROW(std::runtime_error, "Unsupported control mode to query target " << mode);
            break;
        }

        return target;
    }

    double WellsGroupInterface::getTarget(InjectionSpecification::ControlMode mode)
    {
        double target = -1.0;
        switch (mode) {
        case InjectionSpecification::RATE:
            target = injSpec().surface_flow_max_rate_;
            break;
        case InjectionSpecification::RESV:
            target = injSpec().reservoir_flow_max_rate_;
            break;
        case InjectionSpecification::GRUP:
            OPM_THROW(std::runtime_error, "Can't query target production rate for GRUP control keyword");
            break;
        default:
            OPM_THROW(std::runtime_error, "Unsupported control mode to query target " << mode);
            break;
        }

        return target;
    }


    bool WellsGroupInterface::shouldUpdateWellTargets() const
    {
        return should_update_well_targets_;
    }


    void WellsGroupInterface::setShouldUpdateWellTargets(const bool should_update_well_targets)
    {
        should_update_well_targets_ = should_update_well_targets;
    }

    bool WellsGroupInterface::individualControl() const
    {
        return individual_control_;
    }

    void WellsGroupInterface::setIndividualControl(const bool individual_control)
    {
        individual_control_ = individual_control;
    }

    double WellsGroupInterface::efficicencyFactor() const
    {
        return efficicency_factor_;
    }

    void WellsGroupInterface::setEfficiencyFactor(const double efficicency_factor)
    {
        efficicency_factor_=efficicency_factor;
    }




    // ==============   WellsGroup members =============

    WellsGroupInterface* WellsGroup::findGroup(const std::string& name_of_node)
    {
        if (name() == name_of_node) {
            return this;
        } else {
            for (size_t i = 0; i < children_.size(); i++) {
                WellsGroupInterface* result = children_[i]->findGroup(name_of_node);
                if (result) {
                    return result;
                }
            }

            // Not found in this node.
            return NULL;
        }
    }


    WellsGroup::WellsGroup(const std::string& myname,
                           const double efficiency_factor,
                           const ProductionSpecification& prod_spec,
                           const InjectionSpecification& inj_spec,
                           const PhaseUsage& phase_usage)
        : WellsGroupInterface(myname, efficiency_factor, prod_spec, inj_spec, phase_usage)
    {
    }

    /// Sets the current active control to the provided one for all injectors within the group.
    /// After this call, the combined rate (which rate depending on control_mode) of the group
    /// shall be equal to target.
    /// \param[in] only_group    if true, only children that are under group control will be changed.
    ///                          otherwise, all children will be set under group control
    void WellsGroup::applyInjGroupControl(const InjectionSpecification::ControlMode control_mode,
                                          const double target,
                                          const bool only_group)
    {
        if (injSpec().control_mode_ == InjectionSpecification::NONE) {
            return;
        }

        if (!only_group || injSpec().control_mode_ == InjectionSpecification::FLD) {
            const double my_guide_rate = injectionGuideRate(only_group);
            if (my_guide_rate == 0.0) {
                // Nothing to do here
                return;
            }
            for (size_t i = 0; i < children_.size(); ++i) {
                const double child_target = target / efficicencyFactor() * children_[i]->injectionGuideRate(only_group) / my_guide_rate;
                children_[i]->applyInjGroupControl(control_mode, child_target, false);
            }
            injSpec().control_mode_ = InjectionSpecification::FLD;
        }
    }

    /// Sets the current active control to the provided one for all producers within the group.
    /// After this call, the combined rate (which rate depending on control_mode) of the group
    /// shall be equal to target.
    /// \param[in] only_group    if true, only children that are under group control will be changed.
    ///                          otherwise, all children will be set under group control
    void WellsGroup::applyProdGroupControl(const ProductionSpecification::ControlMode control_mode,
                                           const double target,
                                           const bool only_group)
    {
        if (prodSpec().control_mode_ == ProductionSpecification::NONE) {
            return;
        }
        if (!only_group || prodSpec().control_mode_ == ProductionSpecification::FLD) {
            const double my_guide_rate =  productionGuideRate(false);
            if (my_guide_rate == 0.0) {
                // Nothing to do here
                return;
            }
            for (size_t i = 0; i < children_.size(); ++i) {
                const double child_target = target / efficicencyFactor() * children_[i]->productionGuideRate(only_group) / my_guide_rate;
                children_[i]->applyProdGroupControl(control_mode, child_target, false);
            }
            prodSpec().control_mode_ = ProductionSpecification::FLD;
        }
    }


    bool WellsGroup::conditionsMet(const std::vector<double>& well_bhp,
                                   const std::vector<double>& well_reservoirrates_phase,
                                   const std::vector<double>& well_surfacerates_phase,
                                   WellPhasesSummed& summed_phases)
    {
        // Check children's constraints recursively.
        WellPhasesSummed child_phases_summed;
        for (size_t i = 0; i < children_.size(); ++i) {
            WellPhasesSummed current_child_phases_summed;
            if (!children_[i]->conditionsMet(well_bhp,
                                             well_reservoirrates_phase,
                                             well_surfacerates_phase,
                                             current_child_phases_summed)) {
                return false;
            }
            child_phases_summed += current_child_phases_summed;
        }


        // Injection constraints.
        InjectionSpecification::ControlMode injection_modes[] = {InjectionSpecification::RATE,
                                                                 InjectionSpecification::RESV};
        // RATE
        for (int mode_index = 0; mode_index < 2; ++mode_index) {
            InjectionSpecification::ControlMode mode = injection_modes[mode_index];
            if(injSpec().control_mode_ == mode) {
                continue;
            }
            const double target_rate = getTarget(mode);
            if (target_rate >= 0.0) {
                double my_rate = rateByMode(child_phases_summed.res_inj_rates,
                                            child_phases_summed.surf_inj_rates,
                                            mode);

                if (my_rate > target_rate) {
                    OpmLog::warning("Group " + InjectionSpecification::toString(mode) 
                                    + " target not met for group " + name() + "\n"
                                    + "target = " + std::to_string(target_rate) + "\n"
                                    + "rate   = " + std::to_string(my_rate));
                    applyInjGroupControl(mode, target_rate, false);
                    injSpec().control_mode_ = mode;
                    return false;
                }
            }
        }

        // REIN
        // \TODO: Add support for REIN controls.

        // Production constraints.
        ProductionSpecification::ControlMode production_modes[] = {ProductionSpecification::ORAT,
                                                                   ProductionSpecification::WRAT,
                                                                   ProductionSpecification::GRAT,
                                                                   ProductionSpecification::LRAT,
                                                                   ProductionSpecification::RESV};
        bool production_violated = false;
        ProductionSpecification::ControlMode production_mode_violated;
        for (int mode_index = 0; mode_index < 5; ++mode_index) {
            const ProductionSpecification::ControlMode mode = production_modes[mode_index];
            if (prodSpec().control_mode_ == mode) {
                continue;
            }
            const double target_rate = getTarget(mode);
            if (target_rate >= 0.0) {
                const double my_rate = rateByMode(child_phases_summed.res_prod_rates,
                                                  child_phases_summed.surf_prod_rates,
                                                  mode);
                if (std::fabs(my_rate) > target_rate) {
                    OpmLog::warning("Group" + ProductionSpecification::toString(mode) 
                                    + " target not met for group " + name() + "\n"
                                    + "target = " + std::to_string(target_rate) + '\n'
                                    + "rate   = " + std::to_string(my_rate));
                    production_violated = true;
                    production_mode_violated = mode;
                    break;
                }
            }
        }

        if (production_violated) {
            switch (prodSpec().procedure_) {
            case ProductionSpecification::WELL:
                getWorstOffending(well_reservoirrates_phase,
                                  well_surfacerates_phase,
                                  production_mode_violated).first->shutWell();
                return false;
            case ProductionSpecification::RATE:
                applyProdGroupControl(production_mode_violated,
                                      getTarget(production_mode_violated),
                                      false);
                return false;
            case ProductionSpecification::NONE_P:
                // Do nothing
                return false;
            }
        }

        summed_phases += child_phases_summed;
        return true;
    }

    void WellsGroup::addChild(std::shared_ptr<WellsGroupInterface> child)
    {
        children_.push_back(child);
    }


    int WellsGroup::numberOfLeafNodes() {
        // This could probably use some caching, but seeing as how the number of
        // wells is relatively small, we'll do without for now.
        int sum = 0;

        for(size_t i = 0; i < children_.size(); i++) {
            sum += children_[i]->numberOfLeafNodes();
        }

        return sum;
    }

    std::pair<WellNode*, double> WellsGroup::getWorstOffending(const std::vector<double>& well_reservoirrates_phase,
                                                               const std::vector<double>& well_surfacerates_phase,
                                                               ProductionSpecification::ControlMode mode)
    {
        std::pair<WellNode*, double> max;
        for (size_t i = 0; i < children_.size(); i++) {
            std::pair<WellNode*, double> child_max = children_[i]->getWorstOffending(well_reservoirrates_phase,
                                                                                     well_surfacerates_phase,
                                                                                     mode);
            if (i == 0 || max.second < child_max.second) {
                max = child_max;
            }
        }
        return max;
    }

    void WellsGroup::applyProdGroupControls()
    {
        ProductionSpecification::ControlMode prod_mode = prodSpec().control_mode_;
        switch (prod_mode) {
        case ProductionSpecification::ORAT:
        case ProductionSpecification::WRAT:
        case ProductionSpecification::LRAT:
        case ProductionSpecification::RESV:
        {
            // const double my_guide_rate = productionGuideRate(true);
            const double my_guide_rate = productionGuideRate(false);
            if (my_guide_rate == 0) {
                OPM_THROW(std::runtime_error, "Can't apply group control for group " << name() << " as the sum of guide rates for all group controlled wells is zero.");
            }
            for (size_t i = 0; i < children_.size(); ++i ) {
                // Apply for all children.
                // Note, we do _not_ want to call the applyProdGroupControl in this object,
                // as that would check if we're under group control, something we're not.
                const double children_guide_rate = children_[i]->productionGuideRate(false);
                children_[i]->applyProdGroupControl(prod_mode,
                                                   (children_guide_rate / my_guide_rate) * getTarget(prod_mode),
                                                    false);
            }
            break;
        }
        case ProductionSpecification::FLD:
        case ProductionSpecification::NONE:
            // Call all children
            for (size_t i = 0; i < children_.size(); ++i ) {
                children_[i]->applyProdGroupControls();
            }
            break;
        default:
            OPM_THROW(std::runtime_error, "Unhandled group production control type " << prod_mode);
        }
    }

    void WellsGroup::applyInjGroupControls()
    {
        InjectionSpecification::ControlMode inj_mode = injSpec().control_mode_;
        switch (inj_mode) {
        case InjectionSpecification::RATE:
            // need to be careful in the future.
            // pay attention to the phase under control and the phase for the guide rate
            // they can be different, and more delicate situatioin can happen here.
        case InjectionSpecification::RESV:
        {
            // very hacky way here.
            // The logic of the code is that only a well is specified under GRUP control, it is under group control.
            // Which is not the case observed from the result.
            // From the result, if we specify group control with GCONPROD and WCONPROD for a well, it looks like
            // the well will be under group control.
            // TODO: make the logic correct here instead of using `false here`.
            const double my_guide_rate = injectionGuideRate(false);

            for (size_t i = 0; i < children_.size(); ++i) {
                // Apply for all children.
                // Note, we do _not_ want to call the applyProdGroupControl in this object,
                // as that would check if we're under group control, something we're not.
                const double children_guide_rate = children_[i]->injectionGuideRate(false);
                children_[i]->applyInjGroupControl(inj_mode,
                        (children_guide_rate / my_guide_rate) * getTarget(inj_mode) / efficicencyFactor(),
                        true);
            }
            return;
        }
        case InjectionSpecification::VREP:
        case InjectionSpecification::REIN:
            std::cout << "Replacement keywords found, remember to call applyExplicitReinjectionControls." << std::endl;
            return;
        case InjectionSpecification::FLD:
        case InjectionSpecification::NONE:
            // Call all children
            for (size_t i = 0; i < children_.size(); ++i ) {
                children_[i]->applyInjGroupControls();
            }
            return;
        default:
            OPM_THROW(std::runtime_error, "Unhandled group injection control mode " << inj_mode);
        }
    }

    /// Calculates the production guide rate for the group.
    /// \param[in] only_group If true, will only accumelate guide rates for
    ///                       wells under group control
    double WellsGroup::productionGuideRate(bool only_group)
    {
        double sum = 0.0;
        for (size_t i = 0; i < children_.size(); ++i) {
            if (only_group) {
                if (!children_[i]->individualControl()) {
                    sum += children_[i]->productionGuideRate(only_group);
                }
            } else {
                sum += children_[i]->productionGuideRate(only_group);
            }
        }
        return sum;
    }

    /// Calculates the injection guide rate for the group.
    /// \param[in] only_group If true, will only accumelate guide rates for
    ///                       wells under group control
    double WellsGroup::injectionGuideRate(bool only_group)
    {
        double sum = 0.0;
        for (size_t i = 0; i < children_.size(); ++i) {
            sum += children_[i]->injectionGuideRate(only_group);
        }
        return sum;
    }

    /// Gets the total production flow of the given phase.
    /// \param[in] phase_flows      A vector containing rates by phase for each well.
    ///                             Is assumed to be ordered the same way as the related Wells-struct,
    ///                             with all phase rates of a single well adjacent in the array.
    /// \param[in] phase            The phase for which to sum up.

    double WellsGroup::getTotalProductionFlow(const std::vector<double>& phase_flows,
                                             const BlackoilPhases::PhaseIndex phase) const
    {
        double sum = 0.0;
        for (size_t i = 0; i < children_.size(); ++i) {
            sum += children_[i]->getTotalProductionFlow(phase_flows, phase);
        }
        return sum;
    }

    /// Applies explicit reinjection controls. This must be called at each timestep to be correct.
    /// \param[in]    well_reservoirrates_phase
    ///                         A vector containing reservoir rates by phase for each well.
    ///                         Is assumed to be ordered the same way as the related Wells-struct,
    ///                         with all phase rates of a single well adjacent in the array.
    /// \param[in]    well_surfacerates_phase
    ///                         A vector containing surface rates by phase for each well.
    ///                         Is assumed to be ordered the same way as the related Wells-struct,
    ///                         with all phase rates of a single well adjacent in the array.
    void WellsGroup::applyExplicitReinjectionControls(const std::vector<double>& well_reservoirrates_phase,
                                                      const std::vector<double>& well_surfacerates_phase)
    {
        if (injSpec().control_mode_ == InjectionSpecification::REIN) {
            // Defaulting to water to satisfy -Wmaybe-uninitialized
            BlackoilPhases::PhaseIndex phase = BlackoilPhases::Aqua;
            switch (injSpec().injector_type_) {
            case InjectionSpecification::WATER:
                phase = BlackoilPhases::Aqua;
                break;
            case InjectionSpecification::GAS:
                phase = BlackoilPhases::Vapour;
                break;
            case InjectionSpecification::OIL:
                phase = BlackoilPhases::Liquid;
                break;
            }
            const double total_produced = getTotalProductionFlow(well_surfacerates_phase, phase);
            const double total_reinjected = - total_produced; // Production negative, injection positive
            const double my_guide_rate = injectionGuideRate(true);
            for (size_t i = 0; i < children_.size(); ++i) {
                // Apply for all children.
                // Note, we do _not_ want to call the applyProdGroupControl in this object,
                // as that would check if we're under group control, something we're not.
                const double children_guide_rate = children_[i]->injectionGuideRate(true);
#ifdef DIRTY_WELLCTRL_HACK
                children_[i]->applyInjGroupControl(InjectionSpecification::RESV,
                        (children_guide_rate / my_guide_rate) * total_reinjected * injSpec().reinjection_fraction_target_,
                        true);
#else
                children_[i]->applyInjGroupControl(InjectionSpecification::RATE,
                        (children_guide_rate / my_guide_rate) * total_reinjected * injSpec().reinjection_fraction_target_,
                        true);
#endif
            }
        }
        else if (injSpec().control_mode_ == InjectionSpecification::VREP) {
            double total_produced = 0.0;
            if (phaseUsage().phase_used[BlackoilPhases::Aqua]) {
                total_produced += getTotalProductionFlow(well_reservoirrates_phase, BlackoilPhases::Aqua);
            }
            if (phaseUsage().phase_used[BlackoilPhases::Liquid]) {
                total_produced += getTotalProductionFlow(well_reservoirrates_phase, BlackoilPhases::Liquid);
            }
            if (phaseUsage().phase_used[BlackoilPhases::Vapour]) {
                total_produced += getTotalProductionFlow(well_reservoirrates_phase, BlackoilPhases::Vapour);
            }
            const double total_reinjected = - total_produced; // Production negative, injection positive
            const double my_guide_rate = injectionGuideRate(true);
            for (size_t i = 0; i < children_.size(); ++i) {
                // Apply for all children.
                // Note, we do _not_ want to call the applyProdGroupControl in this object,
                // as that would check if we're under group control, something we're not.
                const double children_guide_rate = children_[i]->injectionGuideRate(true);
                children_[i]->applyInjGroupControl(InjectionSpecification::RESV,
                        (children_guide_rate / my_guide_rate) * total_reinjected * injSpec().voidage_replacment_fraction_,
                        true);
            }

        }
    }


    void WellsGroup::updateWellProductionTargets(const std::vector<double>& well_rates)
    {
        // TODO: currently, we only handle the level of the well groups for the moment, i.e. the level just above wells
        // We believe the relations between groups are similar to the relations between different wells inside the same group.
        // While there will be somre more complication invloved for sure.
        // Basically, we need to update the target rates for the wells still under group control.

        ProductionSpecification::ControlMode prod_mode = prodSpec().control_mode_;
        double target_rate = -1.0;

        switch(prod_mode) {
        case ProductionSpecification::FLD :
            {
                auto* parent_node = getParent();
                prod_mode = parent_node->prodSpec().control_mode_;
                target_rate = parent_node->getTarget(prod_mode) / parent_node->efficicencyFactor();
                break;
            }
        case ProductionSpecification::LRAT :
        case ProductionSpecification::ORAT :
        case ProductionSpecification::GRAT :
        case ProductionSpecification::WRAT :
            target_rate = getTarget(prod_mode);
            break;
        default:
            OPM_THROW(std::runtime_error, "Not supporting type " << ProductionSpecification::toString(prod_mode) <<
                                          " when updating well targets ");
        }

        target_rate /= efficicencyFactor();

        // the rates contributed from wells under individual control due to their own limits.
        // TODO: will handle wells specified not to join group control later.
        double rate_individual_control = 0.;

        for (size_t i = 0; i < children_.size(); ++i) {
            if (children_[i]->individualControl() && children_[i]->isProducer()) {
                rate_individual_control += std::abs(children_[i]->getProductionRate(well_rates, prod_mode) * children_[i]->efficicencyFactor());
            }
        }

        // the rates left for the wells under group control to split
        const double rate_for_group_control = target_rate - rate_individual_control;

        const double my_guide_rate = productionGuideRate(true);

        for (size_t i = 0; i < children_.size(); ++i) {
            if (!children_[i]->individualControl() && children_[i]->isProducer()) {
                const double children_guide_rate = children_[i]->productionGuideRate(true);
                children_[i]->applyProdGroupControl(prod_mode, (children_guide_rate / my_guide_rate) * rate_for_group_control, true);
                children_[i]->setShouldUpdateWellTargets(false);
            }
        }
    }

    void WellsGroup::updateWellInjectionTargets(const std::vector<double>& /*well_rates*/)
    {
        // NOT doing anything yet.
        // Will finish it when having an examples with more than one injection wells within same injection group.
        for (size_t i = 0; i < children_.size(); ++i) {
            if (!children_[i]->individualControl() && children_[i]->isInjector()) {
                children_[i]->setShouldUpdateWellTargets(false);
            }
        }
    }


    bool WellsGroup::isProducer() const
    {
        return false;
    }

    bool WellsGroup::isInjector() const
    {
        return false;
    }

    double WellsGroup::getProductionRate(const std::vector<double>& /* well_rates */,
                                         const ProductionSpecification::ControlMode /* prod_mode */) const
    {
        // TODO: to be implemented
        return -1.e98;
    }

    // ==============    WellNode members   ============



    WellNode::WellNode(const std::string& myname,
                       const double efficiency_factor,
                       const ProductionSpecification& prod_spec,
                       const InjectionSpecification& inj_spec,
                       const PhaseUsage& phase_usage)
        : WellsGroupInterface(myname, efficiency_factor, prod_spec, inj_spec, phase_usage),
          wells_(0),
          self_index_(-1),
          group_control_index_(-1),
          shut_well_(true) // This is default for now
    {
    }

    bool WellNode::conditionsMet(const std::vector<double>& well_bhp,
                                 const std::vector<double>& well_reservoirrates_phase,
                                 const std::vector<double>& well_surfacerates_phase,
                                 WellPhasesSummed& summed_phases)
    {
        // Report on our rates.
        const int np = phaseUsage().num_phases;
        for (int phase = 0; phase < np; ++phase) {
            if (isInjector()) {
                summed_phases.res_inj_rates[phase] = well_reservoirrates_phase[np*self_index_ + phase];
                summed_phases.surf_inj_rates[phase] = well_surfacerates_phase[np*self_index_ + phase];
            } else {
                summed_phases.res_prod_rates[phase] = well_reservoirrates_phase[np*self_index_ + phase];
                summed_phases.surf_prod_rates[phase] = well_surfacerates_phase[np*self_index_ + phase];
            }
        }

        // Check constraints.
        const WellControls * ctrls = wells_->ctrls[self_index_];
        for (int ctrl_index = 0; ctrl_index < well_controls_get_num(ctrls); ++ctrl_index) {
            if (ctrl_index == well_controls_get_current(ctrls) || ctrl_index == group_control_index_) {
                // We do not check constraints that either were used
                // as the active control, or that come from group control.
                continue;
            }
            bool ctrl_violated = false;
            switch (well_controls_iget_type(ctrls , ctrl_index)) {

            case BHP: {
                const double my_well_bhp = well_bhp[self_index_];
                const double my_target_bhp = well_controls_iget_target( ctrls , ctrl_index);
                ctrl_violated = isProducer() ? (my_target_bhp > my_well_bhp)
                    : (my_target_bhp < my_well_bhp);
                if (ctrl_violated) {
                    OpmLog::info("BHP limit violated for well " + name() + ":\n"
                                 + "BHP limit = " + std::to_string(my_target_bhp)
                                 + "BHP       = " + std::to_string(my_well_bhp));
                }
                break;
            }

            case THP: {
                //TODO: Implement support
                OPM_THROW(std::invalid_argument, "THP not implemented in WellNode::conditionsMet.");
            }

            case RESERVOIR_RATE: {
                double my_rate = 0.0;
                const double * ctrls_distr = well_controls_iget_distr( ctrls , ctrl_index );
                for (int phase = 0; phase < np; ++phase) {
                    my_rate += ctrls_distr[phase] * well_reservoirrates_phase[np*self_index_ + phase];
                }
                const double my_rate_target = well_controls_iget_target(ctrls , ctrl_index);
                ctrl_violated = std::fabs(my_rate) - std::fabs(my_rate_target)> std::max(std::abs(my_rate), std::abs(my_rate_target))*1e-6;
                if (ctrl_violated) {
                    OpmLog::info("RESERVOIR_RATE limit violated for well " + name() + ":\n"
                                 + "rate limit = " + std::to_string(my_rate_target)
                                 + "rate       = " + std::to_string(my_rate));
                }
                break;
            }

            case SURFACE_RATE: {
                double my_rate = 0.0;
                const double * ctrls_distr = well_controls_iget_distr( ctrls , ctrl_index );
                for (int phase = 0; phase < np; ++phase) {
                    my_rate += ctrls_distr[phase] * well_surfacerates_phase[np*self_index_ + phase];
                }
                const double my_rate_target = well_controls_iget_target(ctrls , ctrl_index);
                ctrl_violated = std::fabs(my_rate) > std::fabs(my_rate_target);
                if (ctrl_violated) {
                    OpmLog::info("SURFACE_RATE limit violated for well " + name() + ":\n"
                                 + "rate limit = " + std::to_string(my_rate_target)
                                 + "rate       = " + std::to_string(my_rate));
                }
                break;
            }
            } // end of switch()
            if (ctrl_violated) {
                set_current_control(self_index_, ctrl_index, wells_);
                return false;
            }
        }
        return true;
    }

    WellsGroupInterface* WellNode::findGroup(const std::string& name_of_node)
    {
        if (name() == name_of_node) {
            return this;
        } else {
            return NULL;
        }

    }

    bool WellNode::isLeafNode() const
    {
        return true;
    }

    void WellNode::setWellsPointer(Wells* wells, int self_index)
    {
        wells_ = wells;
        self_index_ = self_index;
    }

    int WellNode::numberOfLeafNodes()
    {
        return 1;
    }

    void WellNode::shutWell()
    {
        if (shut_well_) {
            well_controls_stop_well( wells_->ctrls[self_index_]);
        }
        else {
            const double target = 0.0;
            const double alq = 0.0;
            const double distr[3] = {1.0, 1.0, 1.0};

            if (group_control_index_ < 0) {
                // The well only had its own controls, no group controls.
                append_well_controls(SURFACE_RATE, target,
                        invalid_alq, invalid_vfp,
                        distr, self_index_, wells_);
                group_control_index_ = well_controls_get_num(wells_->ctrls[self_index_]) - 1;
            } else {
                // We will now modify the last control, that
                // "belongs to" the group control.
                
                well_controls_iset_type( wells_->ctrls[self_index_] , group_control_index_ , SURFACE_RATE);
                well_controls_iset_target( wells_->ctrls[self_index_] , group_control_index_ , target);
                well_controls_iset_alq( wells_->ctrls[self_index_] , group_control_index_ , alq);
                well_controls_iset_distr(wells_->ctrls[self_index_] , group_control_index_ , distr);
            }
            well_controls_open_well( wells_->ctrls[self_index_]);
        }
    }

    std::pair<WellNode*, double> WellNode::getWorstOffending(const std::vector<double>& well_reservoirrates_phase,
                                                             const std::vector<double>& well_surfacerates_phase,
                                                             ProductionSpecification::ControlMode mode)
    {
        const int np = phaseUsage().num_phases;
        const int index = self_index_*np;
        return std::pair<WellNode*, double>(this,
                                            rateByMode(&well_reservoirrates_phase[index],
                                                       &well_surfacerates_phase[index],
                                                       mode));
    }

    void WellNode::applyInjGroupControl(const InjectionSpecification::ControlMode control_mode,
                                        const double target,
                                        const bool only_group)
    {
        if ( !isInjector() ) {
            assert(target == 0.0);
            return;
        }

        if (only_group && individualControl()) {
            return;
        }

        // considering the efficiency factor
        const double effective_target = target / efficicencyFactor();

        const double distr[3] = { 1.0, 1.0, 1.0 };
        WellControlType wct;
        switch (control_mode) {
        case InjectionSpecification::RATE:
            wct = SURFACE_RATE;
            break;
        case InjectionSpecification::RESV:
            wct = RESERVOIR_RATE;
            break;
        default:
            OPM_THROW(std::runtime_error, "Group injection control mode not handled: " << control_mode);
        }

        if (group_control_index_ < 0) {
            // The well only had its own controls, no group controls.
            append_well_controls(wct, effective_target, invalid_alq, invalid_vfp, distr, self_index_, wells_);
            group_control_index_ = well_controls_get_num(wells_->ctrls[self_index_]) - 1;
        } else {
            // We will now modify the last control, that
            // "belongs to" the group control.
            well_controls_iset_type(wells_->ctrls[self_index_] , group_control_index_ , wct);
            well_controls_iset_target(wells_->ctrls[self_index_] , group_control_index_ , effective_target);
            well_controls_iset_alq(wells_->ctrls[self_index_] , group_control_index_ , -1e100);
            well_controls_iset_distr(wells_->ctrls[self_index_] , group_control_index_ , distr);
        }
        set_current_control(self_index_, group_control_index_, wells_);
        // TODO: it might always be the case
        setIndividualControl(false);
    }


    /// Gets the total production flow of the given phase.
    /// \param[in] phase_flows      A vector containing rates by phase for each well.
    ///                             Is assumed to be ordered the same way as the related Wells-struct,
    ///                             with all phase rates of a single well adjacent in the array.
    /// \param[in] phase            The phase for which to sum up.

    double WellNode::getTotalProductionFlow(const std::vector<double>& phase_flows,
                                            const BlackoilPhases::PhaseIndex phase) const
    {
        if (isInjector()) {
            return 0.0;
        }
        return phase_flows[self_index_*phaseUsage().num_phases + phaseUsage().phase_pos[phase]];
    }

    WellType WellNode::type() const {
        return wells_->type[self_index_];
    }

    /// Applies explicit reinjection controls. This must be called at each timestep to be correct.
    /// \param[in]    well_reservoirrates_phase
    ///                         A vector containing reservoir rates by phase for each well.
    ///                         Is assumed to be ordered the same way as the related Wells-struct,
    ///                         with all phase rates of a single well adjacent in the array.
    /// \param[in]    well_surfacerates_phase
    ///                         A vector containing surface rates by phase for each well.
    ///                         Is assumed to be ordered the same way as the related Wells-struct,
    ///                         with all phase rates of a single well adjacent in the array.
    void WellNode::applyExplicitReinjectionControls(const std::vector<double>&,
                                                    const std::vector<double>&)
    {
        // Do nothing at well level.
    }
    void WellNode::applyProdGroupControl(const ProductionSpecification::ControlMode control_mode,
                                         const double target,
                                         const bool only_group)
    {
        if ( !isProducer() ) {
            assert(target == 0.0);
            return;
        }

        if (only_group && individualControl()) {
            return;
        }
        // We're a producer, so we need to negate the input
        double ntarget = -target / efficicencyFactor();

        double distr[3] = { 0.0, 0.0, 0.0 };
        const int* phase_pos = phaseUsage().phase_pos;
        const int* phase_used = phaseUsage().phase_used;
        WellControlType wct;
        switch (control_mode) {
        case ProductionSpecification::ORAT:
            wct = SURFACE_RATE;
            if (!phase_used[BlackoilPhases::Liquid]) {
                OPM_THROW(std::runtime_error, "Oil phase not active and ORAT control specified.");
            }
            distr[phase_pos[BlackoilPhases::Liquid]] = 1.0;
            break;
        case ProductionSpecification::WRAT:
            wct = SURFACE_RATE;
            if (!phase_used[BlackoilPhases::Aqua]) {
                OPM_THROW(std::runtime_error, "Water phase not active and WRAT control specified.");
            }
            distr[phase_pos[BlackoilPhases::Aqua]] = 1.0;
            break;
        case ProductionSpecification::GRAT:
            wct = SURFACE_RATE;
            if (!phase_used[BlackoilPhases::Vapour]) {
                OPM_THROW(std::runtime_error, "Gas phase not active and GRAT control specified.");
            }
            distr[phase_pos[BlackoilPhases::Vapour]] = 1.0;
            break;
        case ProductionSpecification::LRAT:
            wct = SURFACE_RATE;
            if (!phase_used[BlackoilPhases::Liquid]) {
                OPM_THROW(std::runtime_error, "Oil phase not active and LRAT control specified.");
            }
            if (!phase_used[BlackoilPhases::Aqua]) {
                OPM_THROW(std::runtime_error, "Water phase not active and LRAT control specified.");
            }
            distr[phase_pos[BlackoilPhases::Liquid]] = 1.0;
            distr[phase_pos[BlackoilPhases::Aqua]] = 1.0;
            break;
        case ProductionSpecification::RESV:
            distr[0] = distr[1] = distr[2] = 1.0;
            wct = RESERVOIR_RATE;
            break;
        default:
            OPM_THROW(std::runtime_error, "Group production control mode not handled: " << control_mode);
        }

        if (group_control_index_ < 0) {
            // The well only had its own controls, no group controls.
            append_well_controls(wct, ntarget, invalid_alq, invalid_vfp, distr, self_index_, wells_);
            group_control_index_ = well_controls_get_num(wells_->ctrls[self_index_]) - 1;
        } else {
            // We will now modify the last control, that
            // "belongs to" the group control.
            well_controls_iset_type(wells_->ctrls[self_index_] , group_control_index_ , wct);
            well_controls_iset_target(wells_->ctrls[self_index_] , group_control_index_ , ntarget);
            well_controls_iset_alq(wells_->ctrls[self_index_] , group_control_index_ , -1e100);
            well_controls_iset_distr(wells_->ctrls[self_index_] , group_control_index_ , distr);
        }
        set_current_control(self_index_, group_control_index_, wells_);
    }


    void WellNode::applyProdGroupControls()
    {
        // Empty
    }

    void WellNode::applyInjGroupControls()
    {
        // Empty
    }

     /// Calculates the production guide rate for the group.
    /// \param[in] only_group If true, will only accumelate guide rates for
    ///                       wells under group control
    double WellNode::productionGuideRate(bool only_group)
    {
        // Current understanding. Two ways might prevent to return the guide_rate here
        // 1. preventing the well from group control with keyword WGRUPCON
        // 2. the well violating some limits and working under limits.
        if (!only_group || !individualControl()) {
            return prodSpec().guide_rate_;
        } else {
            return 0.0;
        }
    }

    /// Calculates the injection guide rate for the group.
    /// \param[in] only_group If true, will only accumelate guide rates for
    ///                       wells under group control
    double WellNode::injectionGuideRate(bool only_group)
    {
        if (!only_group || !individualControl()) {
            return injSpec().guide_rate_;
        } else {
            return 0.0;
        }
    }



    /// Returning the group control index
    int WellNode::groupControlIndex() const
    {
        return group_control_index_;
    }


    /// Returing whether the well is a producer
    bool WellNode::isProducer() const
    {
        return (type() == PRODUCER);
    }

    /// Returing whether the well is a injector
    bool WellNode::isInjector() const
    {
        return (type() == INJECTOR);
    }



    double WellNode::getProductionRate(const std::vector<double>& well_rates,
                                       const ProductionSpecification::ControlMode prod_mode) const
    {
        switch(prod_mode) {
        case ProductionSpecification::LRAT :
            return ( getTotalProductionFlow(well_rates, BlackoilPhases::Liquid) +
                     getTotalProductionFlow(well_rates, BlackoilPhases::Aqua) );
        case ProductionSpecification::ORAT :
            return getTotalProductionFlow(well_rates, BlackoilPhases::Liquid);
        case ProductionSpecification::WRAT :
            return getTotalProductionFlow(well_rates, BlackoilPhases::Aqua);
        case ProductionSpecification::GRAT :
            return getTotalProductionFlow(well_rates, BlackoilPhases::Vapour);
        default:
            OPM_THROW(std::runtime_error, "Not supporting type " << ProductionSpecification::toString(prod_mode) <<
                                          " for production rate calculation ");
        }
    }

    void WellNode::updateWellProductionTargets(const std::vector<double>& /*well_rates*/)
    {
    }

    void WellNode::updateWellInjectionTargets(const std::vector<double>& /*well_rates*/)
    {
    }

    namespace
    {

        InjectionSpecification::InjectorType toInjectorType(const std::string& type)
        {
            if (type[0] == 'O') {
                return InjectionSpecification::OIL;
            }
            if (type[0] == 'W') {
                return InjectionSpecification::WATER;
            }
            if (type[0] == 'G') {
                return InjectionSpecification::GAS;
            }
            OPM_THROW(std::runtime_error, "Unknown type " << type << ", could not convert to SurfaceComponent");
        }


        InjectionSpecification::InjectorType toInjectorType( Phase p )
        {
            switch( p ) {
                case Phase::OIL:   return InjectionSpecification::OIL;
                case Phase::WATER: return InjectionSpecification::WATER;
                case Phase::GAS:   return InjectionSpecification::GAS;
            }
            OPM_THROW(std::logic_error, "Invalid state." );
        }


#define HANDLE_ICM(x)                           \
        if (type == #x) {                       \
            return InjectionSpecification::x;   \
        }

        InjectionSpecification::ControlMode toInjectionControlMode(std::string type)
        {
            HANDLE_ICM(NONE);
            HANDLE_ICM(RATE);
            HANDLE_ICM(RESV);
            HANDLE_ICM(BHP);
            HANDLE_ICM(THP);
            HANDLE_ICM(REIN);
            HANDLE_ICM(VREP);
            HANDLE_ICM(GRUP);
            HANDLE_ICM(FLD);
            OPM_THROW(std::runtime_error, "Unknown type " << type << ", could not convert to InjectionSpecification::ControlMode.");
        }
#undef HANDLE_ICM

#define HANDLE_PCM(x)                           \
        if (type == #x) {                       \
            return ProductionSpecification::x;  \
        }

        ProductionSpecification::ControlMode toProductionControlMode(std::string type)
        {
            HANDLE_PCM(NONE);
            HANDLE_PCM(ORAT);
            HANDLE_PCM(WRAT);
            HANDLE_PCM(GRAT);
            HANDLE_PCM(LRAT);
            HANDLE_PCM(CRAT);
            HANDLE_PCM(RESV);
            HANDLE_PCM(PRBL);
            HANDLE_PCM(BHP);
            HANDLE_PCM(THP);
            HANDLE_PCM(GRUP);
            HANDLE_PCM(FLD);
            OPM_THROW(std::runtime_error, "Unknown type " << type << ", could not convert to ProductionSpecification::ControlMode.");
        }
#undef HANDLE_PCM

        ProductionSpecification::Procedure toProductionProcedure(std::string type)
        {
            if (type == "NONE") {
                return ProductionSpecification::NONE_P;
            }
            if (type == "RATE") {
                return ProductionSpecification::RATE;
            }
            if (type == "WELL") {
                return ProductionSpecification::WELL;
            }


            OPM_THROW(std::runtime_error, "Unknown type " << type << ", could not convert to ControlMode.");
        }
    } // anonymous namespace

    std::shared_ptr<WellsGroupInterface> createGroupWellsGroup(const Group& group, size_t timeStep, const PhaseUsage& phase_usage )
    {
        InjectionSpecification injection_specification;
        ProductionSpecification production_specification;
        if (group.isInjectionGroup(timeStep)) {
            injection_specification.injector_type_ = toInjectorType(group.getInjectionPhase(timeStep));
            injection_specification.control_mode_ = toInjectionControlMode(GroupInjection::ControlEnum2String(group.getInjectionControlMode(timeStep)));
            injection_specification.surface_flow_max_rate_ = group.getSurfaceMaxRate(timeStep);
            injection_specification.reservoir_flow_max_rate_ = group.getReservoirMaxRate(timeStep);
            injection_specification.reinjection_fraction_target_ = group.getTargetReinjectFraction(timeStep);
            injection_specification.voidage_replacment_fraction_ = group.getTargetVoidReplacementFraction(timeStep);
        }

        if (group.isProductionGroup(timeStep)) {
            production_specification.oil_max_rate_ = group.getOilTargetRate(timeStep);
            production_specification.control_mode_ = toProductionControlMode(GroupProduction::ControlEnum2String(group.getProductionControlMode(timeStep)));
            production_specification.water_max_rate_ = group.getWaterTargetRate(timeStep);
            production_specification.gas_max_rate_ = group.getGasTargetRate(timeStep);
            production_specification.liquid_max_rate_ = group.getLiquidTargetRate(timeStep);
            production_specification.procedure_ = toProductionProcedure(GroupProductionExceedLimit::ActionEnum2String(group.getProductionExceedLimitAction(timeStep)));
            production_specification.reservoir_flow_max_rate_ = group.getReservoirVolumeTargetRate(timeStep);
        }

        const double efficiency_factor = group.getGroupEfficiencyFactor(timeStep);

        std::shared_ptr<WellsGroupInterface> wells_group(new WellsGroup(group.name(), efficiency_factor, production_specification, injection_specification, phase_usage));
        return wells_group;
    }


    /*
      Wells which are shut with the WELOPEN or WCONPROD keywords
      typically will not have any valid control settings, it is then
      impossible to set a valid control mode. The Schedule::Well
      objects from opm-parser have the possible well controle mode
      'CMODE_UNDEFINED' - we do not carry that over the specification
      objects here.
     */
    std::shared_ptr<WellsGroupInterface> createWellWellsGroup(const Well* well, size_t timeStep, const PhaseUsage& phase_usage )
    {
        InjectionSpecification injection_specification;
        ProductionSpecification production_specification;
        if (well->isInjector(timeStep)) {
            const WellInjectionProperties& properties = well->getInjectionProperties(timeStep);
            injection_specification.BHP_limit_ = properties.BHPLimit;
            injection_specification.injector_type_ = toInjectorType(WellInjector::Type2String(properties.injectorType));
            injection_specification.surface_flow_max_rate_ = properties.surfaceInjectionRate;
            injection_specification.reservoir_flow_max_rate_ = properties.reservoirInjectionRate;
            production_specification.guide_rate_ = 0.0; // We know we're not a producer
            if (properties.controlMode != WellInjector::CMODE_UNDEFINED) {
                injection_specification.control_mode_ = toInjectionControlMode(WellInjector::ControlMode2String(properties.controlMode));
            }
        }
        else if (well->isProducer(timeStep)) {
            const WellProductionProperties& properties = well->getProductionProperties(timeStep);
            production_specification.BHP_limit_ = properties.BHPLimit;
            production_specification.reservoir_flow_max_rate_ = properties.ResVRate;
            production_specification.oil_max_rate_ = properties.OilRate;
            production_specification.water_max_rate_ = properties.WaterRate;
            injection_specification.guide_rate_ = 0.0; // we know we're not an injector
            if (properties.controlMode != WellProducer::CMODE_UNDEFINED) {
                production_specification.control_mode_ = toProductionControlMode(WellProducer::ControlMode2String(properties.controlMode));
            }
        }
        // TODO: should be specified with WEFAC, while we do not have this keyword support yet.
        const double efficiency_factor = 1.0;
        std::shared_ptr<WellsGroupInterface> wells_group(new WellNode(well->name(), efficiency_factor, production_specification, injection_specification, phase_usage));
        return wells_group;
    }




    double WellNode::getAccumulativeEfficiencyFactor() const {
        // TODO: not sure whether a well can be exempted from repsponding to the efficiency factor
        // for the parent group.
        double efficicency_factor = efficicencyFactor();
        const WellsGroupInterface* parent_node = getParent();
        while (parent_node != nullptr) {
            efficicency_factor *= parent_node->efficicencyFactor();
            parent_node = parent_node->getParent();
        }

        return efficicency_factor;
    }

}
