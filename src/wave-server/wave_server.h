#pragma once

#include "ncnn_context.h"
#include "whdefs.h"

#include <string>

WH_NAMESPACE_BEGIN

class WaveServer
{
public:
    WaveServer();

    void run();

private:
    std::string resolveSiteDir() const;
    std::string resolveConfigPath() const;

private:
    NcnnContext m_ncnnContext;
    std::string m_siteDir;
    std::string m_configPath;
};

WH_NAMESPACE_END
