#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../core/json.h"

WAVE_NAMESPACE_BEGIN
NN_NAMESPACE_BEGIN

class FrameAggregator
{
public:
    FrameAggregator();
    ~FrameAggregator();

    bool init(std::string_view base_dir, const json& config, std::string& out_error);
    void shutdown();

    uint32_t getOutputSize() const;

    void evaluate(const std::vector<float>& embedding_matrix, std::vector<float>& out_scores);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

NN_NAMESPACE_END
WAVE_NAMESPACE_END