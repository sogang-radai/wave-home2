#pragma once

#include <chrono>
#include <string>

std::string formatTimestamp(std::chrono::system_clock::time_point tp = std::chrono::system_clock::now());
