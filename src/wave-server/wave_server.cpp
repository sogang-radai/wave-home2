#include "wave_server.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

#include <drogon/drogon.h>

WH_NAMESPACE_BEGIN

namespace
{
    std::filesystem::path canonical_if_exists(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec))
            return {};

        return std::filesystem::weakly_canonical(path, ec);
    }
}

WaveServer::WaveServer() :
    m_ncnnContext(),
    m_siteDir(resolveSiteDir()),
    m_configPath(resolveConfigPath())
{
}

std::string WaveServer::resolveSiteDir() const
{
    if (const char* env = std::getenv("WH_SITE_DIR"))
    {
        if (env[0] != '\0')
        {
            const auto path = canonical_if_exists(env);
            if (!path.empty())
                return path.string();
        }
    }

    std::error_code ec;
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
    {
        const auto fromExe = canonical_if_exists(exePath.parent_path() / ".." / "site");
        if (!fromExe.empty())
            return fromExe.string();
    }

    const auto fromCwd = canonical_if_exists(std::filesystem::current_path() / "site");
    if (!fromCwd.empty())
        return fromCwd.string();

#ifdef WH_SOURCE_DIR
    const auto fromSource = canonical_if_exists(WH_SOURCE_DIR "/site");
    if (!fromSource.empty())
        return fromSource.string();
#endif

    return (std::filesystem::current_path() / "site").string();
}

std::string WaveServer::resolveConfigPath() const
{
    std::error_code ec;
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
    {
        const auto configPath = exePath.parent_path() / "config.json";
        if (std::filesystem::is_regular_file(configPath, ec))
            return configPath.string();
    }

    const auto cwdConfig = std::filesystem::current_path() / "config.json";
    if (std::filesystem::is_regular_file(cwdConfig, ec))
        return cwdConfig.string();

    return "config.json";
}

void WaveServer::run()
{
    if (!m_ncnnContext.isReady())
        std::cerr << "Warning: NCNN context is not ready\n";

    drogon::app().loadConfigFile(m_configPath);
    drogon::app().setDocumentRoot(m_siteDir);

    std::cout << "Wave Home server starting\n";
    std::cout << "  site:   " << m_siteDir << "\n";
    std::cout << "  config: " << m_configPath << "\n";

    drogon::app().run();
}

WH_NAMESPACE_END
