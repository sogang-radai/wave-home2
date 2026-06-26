#include "spinner.h"

#include <array>
#include <chrono>
#include <iostream>

Spinner::Spinner(const std::string& label) :
    m_label(label)
{
}

Spinner::~Spinner()
{
    stop();
}

void Spinner::start()
{
    if (m_running.exchange(true))
        return;

    m_thread = std::thread([this]() { run(); });
}

void Spinner::stop()
{
    if (!m_running.exchange(false))
        return;

    if (m_thread.joinable())
        m_thread.join();

    std::cout << "\r\033[K";
    std::cout.flush();
}

void Spinner::setLabel(const std::string& label)
{
    m_label = label;
}

void Spinner::run()
{
    constexpr std::array<const char*, 10> kSpinnerFrames = {
        "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
    };

    size_t index = 0;

    while (m_running)
    {
        std::cout << "\r\033[K" << m_label << " " << kSpinnerFrames[index % kSpinnerFrames.size()] << std::flush;
        index++;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}
