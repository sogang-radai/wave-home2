#pragma once

#include <string>

#include "../core/json.h"

WAVE_NAMESPACE_BEGIN

struct AccountInfo
{

};

class AppSetting
{
public:
    AppSetting();
    ~AppSetting();

    void load(const std::string& path);
    void save(const std::string& path);

private:
    json m_settings;
};

WAVE_NAMESPACE_END