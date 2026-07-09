#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <drogon/orm/DbClient.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

class SleepVecStore
{
public:
    explicit SleepVecStore(const drogon::orm::DbClientPtr& client);

    bool tableExists(const std::string& name) const;
    void storeSleepStatEmbedding(int64_t stat_id, const std::vector<float>& embedding);
    void storeSleepReportEmbedding(int64_t report_id, const std::vector<float>& embedding);

private:
    drogon::orm::DbClientPtr m_client;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
