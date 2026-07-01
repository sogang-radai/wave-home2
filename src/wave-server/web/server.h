#pragma once

#include <memory>
#include <string>

#include "../core/json.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

class Server
{
public:
    Server();
    ~Server();

    bool init(const json& config);
    void shutdown();

    // run server on background thread
    void run();

private:
    struct Impl;

    std::unique_ptr<Impl> m_impl;
};

WEB_NAMESPACE_END
WAVE_NAMESPACE_END