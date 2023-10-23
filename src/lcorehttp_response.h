#ifndef LCOREHTTP_CLIENT_RESPONSE_H
#define LCOREHTTP_CLIENT_RESPONSE_H

#include "core_http_client.h"
#include "extended_core_http_client.h"
#include "lcorehttp_client.h"
#include "lua.h"

typedef struct lcorehttp_response {
    HTTPResponse_t response;
    HTTPStatus_t status;
    const char* strStatus;
    const TransportInterface_t* transport;
    size_t bodyBytesRead;
    size_t contentLength;
    int isChunked;
} lcorehttp_response;

#define LCOREHTTP_RESPONSE_METATABLE "COREHTTP_RESPONSE"
#define LCOREHTTP_HEADERS_METATABLE  "COREHTTP_HEADERS"

int l_corehttp_response_create_meta(lua_State* L);
int l_corehttp_response_headers_create_meta(lua_State* L);
lcorehttp_response* l_corehttp_new_response(lua_State* L);

#endif /* LCOREHTTP_CLIENT_RESPONSE_H */