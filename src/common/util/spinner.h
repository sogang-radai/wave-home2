#pragma once

#include <atomic>
#include <string>
#include <thread>

class Spinner
{
public:
    explicit Spinner(const std::string& label = "Waiting");
    ~Spinner();

    void start();
    void stop();
    void setLabel(const std::string& label);

private:
    void run();

    std::string m_label;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};
