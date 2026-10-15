#pragma once
///@file

#include "nix/util/configuration.hh"

namespace nix {

/**
 * Scoped guard that enables an experimental feature for the lifetime
 * of the object.
 */
struct EnableExperimentalFeature
{
    std::set<ExperimentalFeature> previous;

    explicit EnableExperimentalFeature(std::string_view feature)
        : previous(experimentalFeatureSettings.experimentalFeatures.get())
    {
        experimentalFeatureSettings.set("extra-experimental-features", std::string{feature});
    }

    ~EnableExperimentalFeature()
    {
        experimentalFeatureSettings.experimentalFeatures.assign(previous);
    }

    EnableExperimentalFeature(const EnableExperimentalFeature &) = delete;
    EnableExperimentalFeature & operator=(const EnableExperimentalFeature &) = delete;
};

} // namespace nix
