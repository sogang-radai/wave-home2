#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../db/database.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

/** insight.surface → vec_insight_* table name (nullptr if unknown). */
const char* vecTableForInsightSurface(const std::string& surface);

class InsightVecStore
{
public:
    explicit InsightVecStore(const db::DbClientPtr& client);

    bool tableExists(const std::string& name) const;

    /** Delete embeddings for insight ids in the surface's vec table. */
    void deleteEmbeddings(const std::string& surface, const std::vector<int64_t>& insight_ids);

    void storeEmbedding(
        const std::string& surface,
        int64_t insight_id,
        const std::vector<float>& embedding);

private:
    db::DbClientPtr m_client;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
