#pragma once

#include "../../d3d11/plugin_registry.h"

namespace edvr::plugins::cockpit_visuals {

// Cockpit Visuals owns the state behind RemLok's public draw facade. The
// core host registers this immutable record at the existing lifecycle sites.
const EdvrPluginLifecycleOps& remlokLifecycleOps();

}  // namespace edvr::plugins::cockpit_visuals
