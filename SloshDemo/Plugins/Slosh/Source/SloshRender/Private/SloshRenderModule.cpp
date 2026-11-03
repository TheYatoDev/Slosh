// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "Modules/ModuleManager.h"

// Global shaders for the renderer are implemented in SloshRenderer.cpp; the shader directory mapping
// is registered by SloshCore, which this module depends on.
IMPLEMENT_MODULE(FDefaultModuleImpl, SloshRender)
