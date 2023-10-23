
#ifndef LCOREHTTP_CLIENT_PRERESPONSE_H
#define LCOREHTTP_CLIENT_PRERESPONSE_H

#include "core_http_client.h"
#include "extended_core_http_client.h"
#include "lcorehttp_client.h"
#include "lua.h"

typedef struct lcorehttp_preresponse {
    HTTPResponse_t* response;
    const TransportInterface_t* transport;
} lcorehttp_preresponse;

#define LCOREHTTP_PRERESPONSE_METATABLE "COREHTTP_PRERESPONSE"

int l_corehttp_preresponse_create_meta(lua_State* L);
lcorehttp_preresponse* l_corehttp_new_preresponse(lua_State* L);

#endif /* LCOREHTTP_CLIENT_PRERESPONSE_H */