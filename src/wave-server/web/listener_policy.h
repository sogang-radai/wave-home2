#pragma once

#include <cstdint>

#include "../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

/** Reject /internal on client-api and non-/internal on agent-api when dual listeners are used. */
void registerListenerIsolation(uint16_t client_api_port, uint16_t agent_api_port);

WEB_NAMESPACE_END
WAVE_NAMESPACE_END
