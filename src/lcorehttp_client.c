#include "lcorehttp_client.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "core_http_client.h"
#include "extended_core_http_client.h"
#include "lerror.h"
#include "lss_options.h"
#include "socket.h"
#include "socket_mbedtls.h"
#include "transport_mbedtls.h"
#include "transport_plaintext.h"

static int
push_error_status(lua_State* L, int httpStatus) {
    lua_pushnil(L);
    lua_pushinteger(L, httpStatus);
    lua_pushstring(L, HTTPClient_strerror(httpStatus));
    return 3;
}

static const char*
connection_kind_protocol(lss_connection_kind kind) {
    return kind == LSS_CONNECTION_KIND_TLS ? "https" : "http";
}

static lcorehttp_client_connection_options
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
    if (lua_istable(L, nargs) || lua_isnil(L, nargs)) { // trailing options table is not used yet
        nargs--;
    }
    if (nargs < 1 || nargs > 3) {
        return luaL_error(L, "invalid number of arguments");
    }

    lcorehttp_client* client = (lcorehttp_client*)lua_newuserdata(L, sizeof(lcorehttp_client));
    memset(client, 0, sizeof(lcorehttp_client));
    client->portno = -1;
    luaL_getmetatable(L, LCOREHTTP_CLIENT_METATABLE);
    lua_setmetatable(L, -2);

    // new_client(host [, options]) or new_client(protocol, host [, port] [, options])
    const char* protocol = "https";
    int hostIdx = 1;
    if (nargs > 1) {
        hostIdx = 2;
        if (lua_type(L, 1) == LUA_TSTRING) {
            protocol = lua_tostring(L, 1);
        }
    }
    client->hostname = strdup(luaL_checklstring(L, hostIdx, &client->hostname_len));
    if (client->hostname == NULL) {
        return luaL_error(L, "failed to allocate hostname");
    }
    if (nargs == 3 && lua_type(L, 3) == LUA_TNUMBER) { // port number (optional)
        client->portno = lua_tointeger(L, 3);
    }

    if (strcmp(protocol, "http") == 0) {
        client->kind = LSS_CONNECTION_KIND_PLAINTEXT;
    } else if (strcmp(protocol, "https") == 0) {
        client->kind = LSS_CONNECTION_KIND_TLS;
    } else {
        return luaL_error(L, "invalid protocol");
    }

    if (client->portno == -1) {
        client->portno = client->kind == LSS_CONNECTION_KIND_TLS ? HTTPS_PORT : HTTP_PORT;
    }

    if (client->portno < 0 || client->portno > 65535) {
        return luaL_error(L, "invalid port number");
    }

    if (client->hostname_len == 0) {
        return luaL_error(L, "invalid hostname");
    }

    return 1; // return the userdata to Lua
}

static int
corehttp_client_create_transport(lua_State* L, const lcorehttp_client* client,
                                 TransportInterface_t* const pTransportInterface,
                                 lcorehttp_client_connection_options options) {
    NetworkContext_t* networkContext = malloc(sizeof(NetworkContext_t));
    if (networkContext == NULL) {
        return push_error(L, "failed to allocate network context");
    }

    switch (client->kind) {
        case LSS_CONNECTION_KIND_PLAINTEXT: {
            lss_connection_result connectionResult =
                lss_open_connection(client->hostname, client->portno, options.plaintext);
            if (connectionResult.error_num != 0) {
                free(networkContext);
                return push_error(L, "failed to open plaintext connection");
            }
            networkContext->kind = LSS_PLAINTEXT_CONTEXT_KIND;
            networkContext->context.plaintext = connectionResult.context;
            break;
        }
        case LSS_CONNECTION_KIND_TLS: {
            // options have to be on top of the stack
            lss_tls_connection_result connectionResult =
                lss_open_tls_connection(client->hostname, client->portno, options.tls);
            if (connectionResult.error_num != 0) {
                free(networkContext);
                return push_error(L, "failed to open tls connection");
            }
            networkContext->kind = LSS_TLS_CONTEXT_KIND;
            networkContext->context.tls = connectionResult.context;
            break;
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
    lua_pushfstring(L, "lcorehttp_client (%s://%s:%d)", connection_kind_protocol(client->kind), client->hostname,
                    client->portno);
    return 1;
}

int
l_corehttp_client_endpoint(lua_State* L) {
    lcorehttp_client* client = (lcorehttp_client*)luaL_checkudata(L, 1, LCOREHTTP_CLIENT_METATABLE);
    lua_pushfstring(L, "%s://%s:%d", connection_kind_protocol(client->kind), client->hostname, client->portno);
    return 1;
}

static size_t
load_request_buffer_size(lua_State* L, int optionsIdx) {
    size_t bufferSize = DEFAULT_COREHTTP_BUFFER_SIZE;
    lua_getfield(L, optionsIdx, "buffer_size");
    if (lua_isinteger(L, -1)) {
        bufferSize = (size_t)lua_tointeger(L, -1);
        if (bufferSize < MINIMUM_COREHTTP_BUFFER_SIZE) {
            bufferSize = MINIMUM_COREHTTP_BUFFER_SIZE;
        } else if (bufferSize > MAXIMUM_COREHTTP_BUFFER_SIZE) {
            bufferSize = MAXIMUM_COREHTTP_BUFFER_SIZE;
        }
    }
    lua_pop(L, 1);
    return bufferSize;
}

static int
add_request_headers(lua_State* L, HTTPRequestHeaders_t* requestHeaders, int optionsIdx) {
    lua_getfield(L, optionsIdx, "headers");
    if (lua_istable(L, -1)) {
        // iterate over headers
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            // key is at -2, value is at -1
            size_t headerLen = 0;
            /* convert a copy: lua_tolstring on the key would invalidate it for lua_next */
            lua_pushvalue(L, -2);
            const char* header = lua_tolstring(L, -1, &headerLen);
            size_t valueLen = 0;
            const char* value = lua_tolstring(L, -2, &valueLen);
            if (headerLen > 0) {
                HTTPStatus_t status = HTTPClient_AddHeader(requestHeaders, header, headerLen, value, valueLen);
                if (status != HTTPSuccess) {
                    lua_pop(L, 1); // key copy
                    return push_error_status(L, status);
                }
            }
            lua_pop(L, 1); // key copy
            lua_pop(L, 1); // remove value, keep key for next iteration
        }
    }
    lua_pop(L, 1);
    return 0;
}

static int
add_request_range(lua_State* L, HTTPRequestHeaders_t* requestHeaders, int optionsIdx) {
    int rangeStart = -1;
    int hasRangeStart = 0;
    lua_getfield(L, optionsIdx, "rangeStart");
    if (lua_isinteger(L, -1)) {
        rangeStart = lua_tointeger(L, -1);
        hasRangeStart = 1;
    }
    lua_pop(L, 1);

    int rangeEnd = -1;
    int hasRangeEnd = 0;
    lua_getfield(L, optionsIdx, "rangeEnd");
    if (lua_isinteger(L, -1)) {
        rangeEnd = lua_tointeger(L, -1);
        hasRangeEnd = 1;
    }
    lua_pop(L, 1);

    if (rangeStart >= 0 && rangeEnd >= 0) {
        HTTPStatus_t status = HTTPClient_AddRangeHeader(requestHeaders, rangeStart, rangeEnd);
        if (status != HTTPSuccess) {
            return push_error_status(L, status);
        }
    } else if ((hasRangeStart && !hasRangeEnd) || (!hasRangeStart && hasRangeEnd)) {
        return push_error(L, "rangeStart and rangeEnd must be specified together");
    } else if (hasRangeStart || hasRangeEnd) {
        return push_error(L, "rangeStart and rangeEnd must be positive integers");
    }
    return 0;
}

static void
load_request_options(lua_State* L, HTTPRequestInfo_t* requestInfo, size_t* bufferSize) {
    *bufferSize = DEFAULT_COREHTTP_BUFFER_SIZE;
    if (!lua_istable(L, 4)) {
        return;
    }

    // get request flags
    lua_getfield(L, 4, "requestFlags");
    if (lua_isinteger(L, -1)) {
        requestInfo->reqFlags = (uint32_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    *bufferSize = load_request_buffer_size(L, 4);

    // keep alive
    lua_getfield(L, 4, "keepAlive");
    if (!lua_isboolean(L, -1) || lua_toboolean(L, -1)) { // default is true
        requestInfo->reqFlags |= HTTP_REQUEST_KEEP_ALIVE_FLAG;
    }
    lua_pop(L, 1);
}

static int
initialize_request_headers(lua_State* L, lcorehttp_client* client, HTTPRequestHeaders_t* requestHeaders, int* isHead) {
    HTTPRequestInfo_t requestInfo = {0};
    // get path from second argument
    requestInfo.pPath = luaL_checklstring(L, 2, &requestInfo.pathLen);
    // get method from third argument
    requestInfo.pMethod = luaL_checklstring(L, 3, &requestInfo.methodLen);
    *isHead = requestInfo.methodLen == sizeof(HTTP_METHOD_HEAD) - 1
        && strncmp(requestInfo.pMethod, HTTP_METHOD_HEAD, requestInfo.methodLen) == 0;

    size_t bufferSize = 0;
    load_request_options(L, &requestInfo, &bufferSize);

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
        int result = add_request_headers(L, requestHeaders, 4);
        if (result != 0) {
            return result;
        }
        result = add_request_range(L, requestHeaders, 4);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

typedef struct lcorehttp_headers_callback_context {
    lua_State* L;
    int headersTableIdx;
} lcorehttp_headers_callback_context;

static void
preload_header(void* pContext, const char* fieldLoc, size_t fieldLen, const char* valueLoc, size_t valueLen,
              uint16_t statusCode) {
    lcorehttp_headers_callback_context* context = (lcorehttp_headers_callback_context*)pContext;

    // Add the field and value to the Lua table
    lua_pushlstring(context->L, fieldLoc, fieldLen); // Push field as key
    lua_pushlstring(context->L, valueLoc, valueLen); // Push value as value
    lua_settable(context->L, context->headersTableIdx); // Set key-value pair in table
}

// Head responses and 1xx/204/304 carry no body by definition.
static void
initialize_response_body_length(HTTPResponse_t* httpResponse, lcorehttp_response* response, int isHead) {
    uint16_t statusCode = httpResponse->statusCode;
    if (isHead || statusCode == 204 || statusCode == 304 || (statusCode >= 100 && statusCode < 200)) {
        response->contentLength = 0;
        return;
    }

    const char* value = NULL;
    size_t valueLen = 0;
    if (HTTPClient_ReadHeader(httpResponse, CONTENT_LENGTH_HEADER, strlen(CONTENT_LENGTH_HEADER), &value, &valueLen)
        == HTTPSuccess) {
        response->contentLength = httpResponse->contentLength;
    } else {
        response->contentLength = LCOREHTTP_CONTENT_LENGTH_UNKNOWN; // close-delimited body, read until EOF
    }
}

static void
initialize_chunked_transfer(HTTPResponse_t* httpResponse, lcorehttp_response* response) {
    const char* value = NULL;
    size_t valueLen = 0;
    if (HTTPClient_ReadHeader(httpResponse, TRANSFER_ENCODING_HEADER, strlen(TRANSFER_ENCODING_HEADER), &value,
                              &valueLen)
            == HTTPSuccess
        && valueLen == sizeof("chunked") - 1 && strncasecmp(value, "chunked", valueLen) == 0) {
        response->contentLength = LCOREHTTP_CONTENT_LENGTH_UNKNOWN;
        response->isChunked = 1;
    }
}

typedef struct lcorehttp_request_body {
    const uint8_t* data;
    size_t len;
    int hasWriteHook;
} lcorehttp_request_body;

static void
load_request_body(lua_State* L, int optionsIdx, lcorehttp_request_body* body, uint32_t* sendFlags) {
    body->data = NULL;
    body->len = 0;
    body->hasWriteHook = 0;
    if (!lua_istable(L, optionsIdx)) {
        return;
    }

    lua_getfield(L, optionsIdx, "body");
    if (lua_isstring(L, -1)) {
        body->data = (const uint8_t*)lua_tolstring(L, -1, &body->len);
    }
    lua_pop(L, 1);

    lua_getfield(L, optionsIdx, "write_body_hook");
    if (lua_isfunction(L, -1)) {
        body->hasWriteHook = 1;
        body->len = 0;
        *sendFlags |= HTTP_SEND_DISABLE_CONTENT_LENGTH_FLAG;
    }
    lua_pop(L, 1);
}

static int
call_write_body_hook(lua_State* L, lcorehttp_response* response, int optionsIdx) {
    lua_getfield(L, optionsIdx, "write_body_hook");
    lcorehttp_preresponse* preresponse = l_corehttp_new_preresponse(L);
    preresponse->transport = response->transport;
    preresponse->response = &response->response;
    lua_pushvalue(L, -2); /* function */
    lua_pushvalue(L, -2); /* argument the hook may discard */
    int hookStatus = lua_pcall(L, 1, 0, 0);
    /* The hook may retain the preresponse past this request; it must never
     * outlive the response/transport it borrows. The userdata stays rooted on
     * the stack below the call, so a hook that drops its argument and collects
     * cannot free it before this cleanup writes. */
    preresponse->transport = NULL;
    preresponse->response = NULL;
    if (hookStatus != LUA_OK) {
        return push_error(L, lua_tostring(L, -1));
    }
    lua_pop(L, 2); // preresponse, write_body_hook
    return 0;
}

static int
send_request_body(lua_State* L, const lcorehttp_request_body* body, lcorehttp_response* response,
                HTTPRequestHeaders_t* requestHeaders, int optionsIdx, uint32_t sendFlags) {
    HTTPStatus_t status = HTTPClient_SendHttpHeaders(response->transport, response->response.getTime, requestHeaders,
                                                     body->len, sendFlags);
    if (status != HTTPSuccess) {
        return push_error_status(L, status);
    }

    if (body->len > 0) { // entire body passed to this function, no hook
        status = HTTPClient_Write(response->transport, response->response.getTime, body->data, body->len);
        return status == HTTPSuccess ? 0 : push_error_status(L, status);
    }

    if (!body->hasWriteHook) {
        return 0;
    }

    return call_write_body_hook(L, response, optionsIdx);
}

// Opens the connection described by client/options and fills in the transport interface.
// On failure the caller must free requestHeaders.pBuffer; nothing is allocated on success
// beyond what response->__gc owns.
static int
create_request_transport(lua_State* L, lcorehttp_client* client, TransportInterface_t** transportInterface) {
    lcorehttp_client_connection_options options = load_corehttp_client_connection_options(L, client->kind, 4);
    if (lua_istable(L, 4) &&
        ((client->kind == LSS_CONNECTION_KIND_PLAINTEXT && options.plaintext == NULL) ||
         (client->kind == LSS_CONNECTION_KIND_TLS && options.tls == NULL))) {
        return push_error(L, client->kind == LSS_CONNECTION_KIND_PLAINTEXT
            ? "failed to allocate plaintext options"
            : "failed to allocate tls options");
    }

    TransportInterface_t* transport = malloc(sizeof(TransportInterface_t));
    int result = transport == NULL ? push_error(L, "failed to allocate transport")
                                  : corehttp_client_create_transport(L, client, transport, options);
    switch (client->kind) {
        case LSS_CONNECTION_KIND_PLAINTEXT: lss_free_plain_connection_options(options.plaintext); break;
        case LSS_CONNECTION_KIND_TLS: lss_free_tls_connection_options(options.tls); break;
    }
    if (result != 0) {
        free(transport);
        return result;
    }

    *transportInterface = transport;
    return 0;
}

int
l_corehttp_client_request(lua_State* L) {
    lcorehttp_client* client = (lcorehttp_client*)luaL_checkudata(L, 1, LCOREHTTP_CLIENT_METATABLE);
    if (client->closed) {
        return push_error(L, "client is closed");
    }

    int isHead = 0;
    HTTPRequestHeaders_t requestHeaders = {0};
    int resultCount = initialize_request_headers(L, client, &requestHeaders, &isHead);
    if (resultCount != 0) {
        free(requestHeaders.pBuffer);
        return resultCount;
    }

    uint32_t sendFlags = 0;
    lcorehttp_request_body body = {0};
    load_request_body(L, 4, &body, &sendFlags);

    TransportInterface_t* transportInterface = NULL;
    resultCount = create_request_transport(L, client, &transportInterface);
    if (resultCount != 0) {
        free(requestHeaders.pBuffer);
        return resultCount;
    }

    lcorehttp_response* response = l_corehttp_new_response(L);
    response->transport = transportInterface;
    response->response.pBuffer = requestHeaders.pBuffer; // response owns the buffer, freed by its __gc
    response->response.bufferLen = requestHeaders.bufferLen;
    response->response.respOptionFlags = HTTP_RESPONSE_DO_NOT_PARSE_BODY_FLAG;

    lua_newtable(L); // response headers
    int headersTableIdx = lua_gettop(L);
    lcorehttp_headers_callback_context headersContext = {L, headersTableIdx};
    HTTPClient_ResponseHeaderParsingCallback_t headerParsingCallback = {.pContext = &headersContext,
                                                                        .onHeaderCallback = preload_header};

    response->status =
        HTTPClient_Validate(transportInterface, &requestHeaders, body.data, body.len, &response->response);
    if (response->status != HTTPSuccess) {
        return push_error_status(L, response->status);
    }

    resultCount = send_request_body(L, &body, response, &requestHeaders, 4, sendFlags);
    if (resultCount != 0) {
        return resultCount;
    }

    response->response.pHeaderParsingCallback = &headerParsingCallback;
    response->status = HTTPClient_ReceiveAndParseHttpResponse(transportInterface, &response->response, &requestHeaders);
    response->response.pHeaderParsingCallback = NULL;
    if ((response->status == HTTPInsufficientMemory || response->status == HTTPPartialResponse)
        && response->response.areHeadersComplete) { // headers are complete, we can read the body later
        response->status = HTTPSuccess;
    }
    response->strStatus = HTTPClient_strerror(response->status);

    initialize_response_body_length(&response->response, response, isHead);
    initialize_chunked_transfer(&response->response, response);

    luaL_getmetatable(L, LCOREHTTP_HEADERS_METATABLE);
    lua_setmetatable(L, headersTableIdx);
    lua_setiuservalue(L, -2, 1);

    return 1;
}

static const luaL_Reg lcorehttp_client_methods[] = {
    {"request", l_corehttp_client_request},
    {"endpoint", l_corehttp_client_endpoint},
    {NULL, NULL}};

static const luaL_Reg lcorehttp_client_metamethods[] = {
    {"__tostring", l_corehttp_client_tostring},
    {"__gc", l_corehttp_client_gc},
    {"__close", l_corehttp_client_gc},
    {NULL, NULL}};

int
l_corehttp_client_create_meta(lua_State* L) {
    luaL_newmetatable(L, LCOREHTTP_CLIENT_METATABLE);
    luaL_setfuncs(L, lcorehttp_client_metamethods, 0);

    lua_newtable(L);
    luaL_setfuncs(L, lcorehttp_client_methods, 0);
    lua_pushstring(L, LCOREHTTP_CLIENT_METATABLE);
    lua_setfield(L, -2, "__type");
    lua_setfield(L, -2, "__index");

    return 1;
}
