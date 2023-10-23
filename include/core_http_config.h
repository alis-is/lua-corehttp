// #define HTTP_DO_NOT_USE_CUSTOM_CONFIG
#ifndef COREHTTP_CONFIG_H
#define COREHTTP_CONFIG_H

#include <stdio.h>
#include "logging.h"

#define HTTP_USER_AGENT_VALUE "lua-corehttp"
// #define HTTP_RECV_RETRY_TIMEOUT_MS 1U
// #define DEBUG_COREHTTP        1

#if defined(DEBUG_COREHTTP)
#define LogError(args) internal_log args
#define LogWarn(args)  internal_log args
#define LogInfo(args)  internal_log args
#define LogDebug(args) internal_log args
#endif

#endif