#include "lcorehttp_client.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>
#include "core_http_client.h"
#include "extended_core_http_client.h"
#include "lss_options.h"
#include "lutil.h"
#include "socket.h"
#include "socket_mbedtls.h"
#include "transport_mbedtls.h"
#include "transport_plaintext.h"

int
push_error_status(lua_State* L, int httpStatus) {
    lua_pushnil(L);
    lua_pushinteger(L, httpStatus);
    lua_pushstring(L, HTTPClient_strerror(httpStatus));
    return 3;
}

lcorehttp_client_connection_options
load_corehttp_client_connection_options(lua_State* L, lss_connection_kind kind, int idx) {
    lcorehttp_client_connection_options options = {0};
    if (!lua_istable(L, idx)) {
        return options;
    }

    // push to top
    lua_pushvalue(L, idx);

    switch (kind) {
        case LSS_CONNECTION_KIND_PLAINTEXT: options.plaintext = lss_load_plaintext_connection_options(L); break;
        case LSS_CONNECTION_KIND_TLS: options.tls = lss_load_tls_connection_options(L); break;
    }

    // cleanup
    lua_pop(L, 1);
    return options;
}

int
l_corehttp_newclient(lua_State* L) {
    int nargs = lua_gettop(L);
    lcorehttp_client* client = (lcorehttp_client*)lua_newuserdata(L, sizeof(lcorehttp_client));
    client->portno = -1;
    client->closed = 0;

    if (lua_istable(L, nargs) || lua_isnil(L, nargs)) { // right now there are no options
        // last are options, substract nargs by 1
        nargs--;
    }

    const char* protocol = "https";

    if (nargs == 1) {
        client->hostname = strdup(luaL_checklstring(L, 1, &client->hostname_len));
    } else if (nargs == 2) {
        if (lua_type(L, 1) == LUA_TSTRING) {
            protocol = strdup(lua_tostring(L, 1));
        }
        client->hostname = strdup(luaL_checklstring(L, 2, &client->hostname_len));
    } else if (nargs == 3) {
        if (lua_type(L, 1) == LUA_TSTRING) {
            protocol = strdup(lua_tostring(L, 1));
        }
        client->hostname = strdup(luaL_checklstring(L, 2, &client->hostname_len));
        if (lua_type(L, 3) == LUA_TNUMBER) { // port number (optional)
            client->portno = lua_tointeger(L, 3);
        }
    } else {
        return luaL_error(L, "invalid number of arguments");
    }

    if (strcmp(protocol, "http") == 0) {
        client->kind = LSS_CONNECTION_KIND_PLAINTEXT;
    } else if (strcmp(protocol, "https") == 0) {
        client->kind = LSS_CONNECTION_KIND_TLS;
    } else {
        return luaL_error(L, "invalid protocol");
    }

    if (client->portno == -1) {
        if (strcmp(protocol, "https") == 0) {
            client->portno = HTTPS_PORT;
        } else if (strcmp(protocol, "http") == 0) {
            client->portno = HTTP_PORT;
        }
    }

    if (client->portno < 0 || client->portno > 65535) {
        return luaL_error(L, "invalid port number");
    }

    if (client->hostname_len == 0) {
        return luaL_error(L, "invalid hostname");
    }

    luaL_getmetatable(L, LCOREHTTP_CLIENT_METATABLE);
    lua_setmetatable(L, -2);

    return 1; // return the userdata to Lua
}

int
corehttp_client_create_transport(lua_State* L, const lcorehttp_client* client,
                                 TransportInterface_t* const pTransportInterface,
                                 lcorehttp_client_connection_options options) {
    NetworkContext_t* networkContext = NULL;
    switch (client->kind) {
        case LSS_CONNECTION_KIND_PLAINTEXT: {
            lss_connection_result connectionResult =
                lss_open_connection(client->hostname, client->portno, options.plaintext);
            if (connectionResult.error_num != 0) {
                return push_error(L, "failed to open plaintext connection");
            }
            networkContext = malloc(sizeof(NetworkContext_t));
            networkContext->kind = LSS_PLAINTEXT_CONTEXT_KIND;
            networkContext->context.plaintext = connectionResult.context;
            break;
        }
        case LSS_CONNECTION_KIND_TLS: {
            // options have to be on top of the stack
            lss_tls_connection_result connectionResult =
                lss_open_tls_connection(client->hostname, client->portno, options.tls);
            if (connectionResult.error_num != 0) {
                return push_error(L, "failed to open tls connection");
            }
            networkContext = malloc(sizeof(NetworkContext_t));
            networkContext->kind = LSS_TLS_CONTEXT_KIND;
            networkContext->context.tls = connectionResult.context;
        }
    }
    pTransportInterface->recv = lss_recv;
    pTransportInterface->send = lss_send;
    pTransportInterface->pNetworkContext = networkContext;
    return 0;
}

int
l_corehttp_client_gc(lua_State* L) {
    lcorehttp_client* client = (lcorehttp_client*)luaL_checkudata(L, 1, LCOREHTTP_CLIENT_METATABLE);
    if (client->closed) {
        return 0;
    }
    free((void*)client->hostname);
    client->closed = 1;
    return 0;
}

int
l_corehttp_client_tostring(lua_State* L) {
    lcorehttp_client* client = (lcorehttp_client*)luaL_checkudata(L, 1, LCOREHTTP_CLIENT_METATABLE);
    const char* protocol = NULL;
    switch (client->kind) {
        case LSS_CONNECTION_KIND_PLAINTEXT: protocol = "http"; break;
        case LSS_CONNECTION_KIND_TLS: protocol = "https"; break;
    }
    lua_pushfstring(L, "lcorehttp_client (%s://%s:%d)", protocol, client->hostname, client->portno);
    return 1;
}

int
l_corehttp_client_endpoint(lua_State* L) {
    lcorehttp_client* client = (lcorehttp_client*)luaL_checkudata(L, 1, LCOREHTTP_CLIENT_METATABLE);
    const char* protocol = NULL;
    switch (client->kind) {
        case LSS_CONNECTION_KIND_PLAINTEXT: protocol = "http"; break;
        case LSS_CONNECTION_KIND_TLS: protocol = "https"; break;
    }
    lua_pushfstring(L, "%s://%s:%d", protocol, client->hostname, client->portno);
    return 1;
}

int
initializeRequestHeaders(lua_State* L, lcorehttp_client* client, HTTPRequestHeaders_t* requestHeaders) {
    HTTPRequestInfo_t requestInfo = {0};
    size_t bufferSize = DEFAULT_COREHTTP_BUFFER_SIZE;
    // get path from second argument
    requestInfo.pPath = luaL_checklstring(L, 2, &requestInfo.pathLen);
    // get method from third argument
    requestInfo.pMethod = luaL_checklstring(L, 3, &requestInfo.methodLen);
    requestInfo.reqFlags = 0; // TODO: do we want HTTP_REQUEST_KEEP_ALIVE_FLAG ?
    if (lua_istable(L, 4)) {
        // get request flags
        lua_getfield(L, 4, "requestFlags");
        if (lua_isinteger(L, -1)) {
            requestInfo.reqFlags = lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        // get buffer size
        lua_getfield(L, 4, "bufferSize");
        if (lua_isinteger(L, -1)) {
            bufferSize = lua_tointeger(L, -1);
            if (bufferSize < MINIMUM_COREHTTP_BUFFER_SIZE) {
                bufferSize = MINIMUM_COREHTTP_BUFFER_SIZE;
            } else if (bufferSize > MAXIMUM_COREHTTP_BUFFER_SIZE) {
                bufferSize = MAXIMUM_COREHTTP_BUFFER_SIZE;
            }
        }
        lua_pop(L, 1);

        // keep alive
        lua_getfield(L, 4, "keepAlive");
        if (!lua_isboolean(L, -1) || lua_toboolean(L, -1)) { // default is true
            requestInfo.reqFlags |= HTTP_REQUEST_KEEP_ALIVE_FLAG;
        }
        lua_pop(L, 1);
    }
    requestInfo.pHost = client->hostname;
    requestInfo.hostLen = client->hostname_len;
    requestHeaders->pBuffer = malloc(bufferSize);
    if (requestHeaders->pBuffer == NULL) {
        return push_error(L, "failed to allocate buffer");
    }
    requestHeaders->bufferLen = bufferSize;

    // initialize request headers
    HTTPStatus_t httpStatus = HTTPClient_InitializeRequestHeaders(requestHeaders, &requestInfo);
    if (httpStatus != HTTPSuccess) {
        return push_error_status(L, httpStatus);
    }

    if (lua_istable(L, 4)) {
        // headers
        lua_getfield(L, 4, "headers");
        if (lua_istable(L, -1)) {
            // iterate over headers
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                // key is at -2, value is at -1
                size_t header_len = 0;
                const char* header = lua_tolstring(L, -2, &header_len);
                size_t value_len = 0;
                const char* value = lua_tolstring(L, -1, &value_len);
                if (header_len > 0) {
                    HTTPStatus_t httpStatus =
                        HTTPClient_AddHeader(requestHeaders, header, header_len, value, value_len);
                    if (httpStatus != HTTPSuccess) {
                        return push_error_status(L, httpStatus);
                    }
                }
                lua_pop(L, 1); // remove value, keep key for next iteration
            }
        }
        lua_pop(L, 1);

        // range header
        int rangeStart = -1;
        int hasRangeStart = 0;
        lua_getfield(L, 4, "rangeStart");
        if (lua_isinteger(L, -1)) {
            rangeStart = lua_tointeger(L, -1);
            hasRangeStart = 1;
        }
        lua_pop(L, 1);
        int rangeEnd = -1;
        int hasRangeEnd = 0;
        lua_getfield(L, 4, "rangeEnd");
        if (lua_isinteger(L, -1)) {
            rangeEnd = lua_tointeger(L, -1);
            hasRangeEnd = 1;
        }
        lua_pop(L, 1);
        if (rangeStart >= 0 && rangeEnd >= 0) {
            HTTPStatus_t httpStatus = HTTPClient_AddRangeHeader(requestHeaders, rangeStart, rangeEnd);
            if (httpStatus != HTTPSuccess) {
                return push_error_status(L, httpStatus);
            }
        } else if ((hasRangeStart && !hasRangeEnd) || (!hasRangeStart && hasRangeEnd)) {
            return push_error(L, "rangeStart and rangeEnd must be specified together");
        } else if (hasRangeStart || hasRangeEnd) {
            return push_error(L, "rangeStart and rangeEnd must be positive integers");
        }
    }
    return 0;
}

void
preloadHeader(void* pContext, const char* fieldLoc, size_t fieldLen, const char* valueLoc, size_t valueLen,
              uint16_t statusCode) {
    lua_State* L = (lua_State*)pContext;

    // Add the field and value to the Lua table
    lua_pushlstring(L, fieldLoc, fieldLen); // Push field as key
    lua_pushlstring(L, valueLoc, valueLen); // Push value as value
    lua_settable(L, -3);                    // Set key-value pair in table
}

int
l_corehttp_client_request(lua_State* L) {
    HTTPRequestHeaders_t requestHeaders = {0};
    HTTPStatus_t httpStatus = HTTPSuccess;
    HTTPClient_ResponseHeaderParsingCallback_t headerParsingCallback = {.pContext = L,
                                                                        .onHeaderCallback = preloadHeader};
    uint32_t sendFlags = 0;
    lcorehttp_client* client = (lcorehttp_client*)luaL_checkudata(L, 1, LCOREHTTP_CLIENT_METATABLE);
    if (client->closed) {
        return push_error(L, "client is closed");
    }
    int resultCount = 0;
    if ((resultCount = initializeRequestHeaders(L, client, &requestHeaders)) != 0) {
        return resultCount;
    }

    size_t body_len = 0;
    const uint8_t* body = NULL;

    // fourth on the stack may be options table
    if (lua_istable(L, 4)) {
        // get body
        lua_getfield(L, 4, "body");
        if (lua_isstring(L, -1)) {
            body = (const uint8_t*)lua_tolstring(L, -1, &body_len);
        }
        lua_pop(L, 1);
        // write_body_hook
        lua_getfield(L, 4, "write_body_hook");
        if (lua_isfunction(L, -1)) {
            sendFlags |= HTTP_SEND_DISABLE_CONTENT_LENGTH_FLAG;
            body_len = 0;
        }
        lua_pop(L, 1);
    }

    lcorehttp_client_connection_options options =
        load_corehttp_client_connection_options(L, client->kind, 4); // options are -2, client is -1

    TransportInterface_t* transportInterface = malloc(sizeof(TransportInterface_t));
    resultCount = corehttp_client_create_transport(L, client, transportInterface, options);
    switch (client->kind) {
        case LSS_CONNECTION_KIND_PLAINTEXT: lss_free_plain_connection_options(options.plaintext); break;
        case LSS_CONNECTION_KIND_TLS: lss_free_tls_connection_options(options.tls); break;
    }

    if (resultCount != 0) {
        free(requestHeaders.pBuffer);
        free(transportInterface);
        return resultCount;
    }

    lcorehttp_response* response = l_corehttp_new_response(L);
    if (response == NULL) {
        return push_error(L, "failed to create response");
    }
    response->transport = transportInterface;
    response->response.pBuffer = requestHeaders.pBuffer; // reuse buffer for response
    response->response.bufferLen = requestHeaders.bufferLen;

    lua_newtable(L); // for headers
    response->response.pHeaderParsingCallback = &headerParsingCallback;

    response->status = HTTPClient_Validate(transportInterface, &requestHeaders, body, body_len, &response->response);
    if (response->status != HTTPSuccess) {
        return push_error_status(L, response->status);
    }

    response->status = HTTPClient_InternalSendHttpHeaders(transportInterface, response->response.getTime,
                                                          &requestHeaders, body_len, sendFlags);
    if (response->status != HTTPSuccess) {
        return push_error_status(L, response->status);
    }

    if (body_len > 0) { // entire body passed to this function, no hook
        response->status = HTTPClient_Write(transportInterface, response->response.getTime, body, body_len);
        if (response->status != HTTPSuccess) {
            return push_error_status(L, response->status);
        }
    } else if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "write_body_hook");
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -1);
            lcorehttp_preresponse* preresponse = l_corehttp_new_preresponse(L);
            if (preresponse == NULL) {
                return push_error(L, "failed to create preresponse");
            }
            preresponse->transport = transportInterface;
            preresponse->response = &response->response;
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                return push_error(L, lua_tostring(L, -1));
            }
        }
        lua_pop(L, 1);
    }

    response->status =
        HTTPClient_InternalReceiveAndParseHttpResponse(transportInterface, &response->response, &requestHeaders);
    response->strStatus = HTTPClient_strerror(response->status);
    response->contentLength = response->response.contentLength;

    const char* transferEncodingHeaderValue = NULL;
    size_t transferEncodingHeaderValueLen = 0;
    HTTPStatus_t status =
        HTTPClient_ReadHeader(&response->response, TRANSFER_ENCODING_HEADER, strlen(TRANSFER_ENCODING_HEADER),
                              &transferEncodingHeaderValue, &transferEncodingHeaderValueLen);
    if (status == HTTPSuccess) {
        if (strncmp(transferEncodingHeaderValue, "chunked", transferEncodingHeaderValueLen) == 0) {
            response->contentLength = -1;
            response->isChunked = 1;
        }
    }

    luaL_getmetatable(L, LCOREHTTP_HEADERS_METATABLE);
    lua_setmetatable(L, -2);
    lua_setiuservalue(L, -2, 1);

    return 1;
}

int
l_corehttp_client_create_meta(lua_State* L) {
    luaL_newmetatable(L, LCOREHTTP_CLIENT_METATABLE);
    /* Metamethods */
    lua_newtable(L);
    lua_pushcfunction(L, l_corehttp_client_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, l_corehttp_client_request);
    lua_setfield(L, -2, "request");
    lua_pushcfunction(L, l_corehttp_client_endpoint);
    lua_setfield(L, -2, "endpoint");
    lua_pushstring(L, LCOREHTTP_CLIENT_METATABLE);
    lua_setfield(L, -2, "__type");
    /* Metamethods */
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, l_corehttp_client_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_corehttp_client_gc);
    lua_setfield(L, -2, "__close");
    return 1;
}