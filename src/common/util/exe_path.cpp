#include "exe_path.h"

#include <climits>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

std::filesystem::path getExecutablePath()
{
    std::error_code ec;

#if defined(__linux__)
    const std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec)
        return {};
    return std::filesystem::weakly_canonical(path, ec);
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return {};
    return std::filesystem::weakly_canonical(buf, ec);
#else
    (void)ec;
    return {};
#endif
}

std::filesystem::path getExecutableDir()
{
    const std::filesystem::path path = getExecutablePath();
    if (path.empty())
        return {};
    return path.parent_path();
}
