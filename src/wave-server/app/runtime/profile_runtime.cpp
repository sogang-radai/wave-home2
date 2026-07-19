#include "profile_runtime.h"

#include "production_profile_runtime.h"
#include "demo_profile_runtime.h"
#include "test_profile_runtime.h"

WAVE_NAMESPACE_BEGIN

std::unique_ptr<IProfileRuntime> createProfileRuntime(ProfileKind kind)
{
    switch (kind)
    {
    case ProfileKind::Demo:
        return std::make_unique<DemoProfileRuntime>();
    case ProfileKind::Test:
        return std::make_unique<TestProfileRuntime>();
    case ProfileKind::Production:
    default:
        return std::make_unique<ProductionProfileRuntime>();
    }
}

WAVE_NAMESPACE_END
