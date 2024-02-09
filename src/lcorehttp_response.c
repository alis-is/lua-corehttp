#include "lcorehttp_response.h"
#include <assert.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>
#include "lcorehttp_time.h"
#include "llhttp.h"
#include "lss_transport.h"
#include "lutil.h"

lcorehttp_response*
l_corehttp_new_response(lua_State* L) {
    lcorehttp_response* response = lua_newuserdatauv(L, sizeof(lcorehttp_response), 1);
    if (response == NULL) {
        return NULL;
    }
    luaL_getmetatable(L, LCOREHTTP_RESPONSE_METATABLE);
    lua_setmetatable(L, -2);
    memset(response, 0, sizeof(lcorehttp_response));
    response->response.getTime = l_corehttp_get_time_ms;
    response->contentLength = -1;
    response->cachedBodyRead = 0;
    return response;
}

int
l_corehttp_response_headers(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);

    // get from first user data
    lua_getiuservalue(L, 1, 1);
    if (lua_istable(L, -1)) {
        return 1;
    }

    lua_newtable(L);
    return 1;
}

int
l_corehttp_response_gc(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);
    if (response->transport != NULL) {
        lss_close(response->transport->pNetworkContext);
        free((void*)response->transport);
        response->transport = NULL;
    }

    return 0;
}

int
l_corehttp_response_tostring(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);
    if (response->isChunked) {
        lua_pushfstring(L, "%p (chunked)", LCOREHTTP_RESPONSE_METATABLE);
        return 1;
    }
    lua_pushfstring(L, "%p (%d bytes)", LCOREHTTP_RESPONSE_METATABLE, response->contentLength);
    return 1;
}

int
l_corehttp_response_status(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);
    lua_pushstring(L, response->strStatus);
    return 1;
}

int
l_corehttp_response_status_code(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);
    lua_pushinteger(L, response->status);
    return 1;
}

int
l_corehttp_response_http_status_code(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);
    lua_pushinteger(L, response->response.statusCode);
    return 1;
}

// read body
int
l_corehttp_response_read(lua_State* L) {
    luaL_Buffer b;
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);
    size_t bufferLen = DEFAULT_COREHTTP_BUFFER_SIZE;
    // get buffer len from stack - optional with default
    if (lua_isnumber(L, 2)) {
        bufferLen = lua_tointeger(L, 2);
    }

    if (bufferLen <= 0) {
        return 0;
    }

    if (response->contentLength == 0 && !response->isChunked) {
        return 0; // no body
    }

    if (!response->cachedBodyRead) {
        const uint8_t* body = (const uint8_t*)response->response.pBody;
        // skip `\r\n\r\n`, `\r\n\n`, `\n\r\n`, and `\n\n` - end of headers
        if (response->response.bodyLen >= 4 && memcmp(body, "\r\n\r\n", 4) == 0) {
            body += 4;
        } else if (response->response.bodyLen >= 3 && memcmp(body, "\r\n\n", 3) == 0) {
            body += 3;
        } else if (response->response.bodyLen >= 3 && memcmp(body, "\n\r\n", 3) == 0) {
            body += 3;
        } else if (response->response.bodyLen >= 2 && memcmp(body, "\n\n", 2) == 0) {
            body += 2;
        }

        response->cachedBodyRead = 1;

        size_t bytesRead = response->response.bodyLen - (body - response->response.pBody);
        lua_pushlstring(L, (const char*)body, bytesRead);
        lua_pushinteger(L, bytesRead);
        return 2;
    }

    luaL_buffinit(L, &b);
    uint8_t* buffer = (uint8_t*)luaL_prepbuffsize(&b, bufferLen);
    size_t bytesRead = 0;
    HTTPStatus_t returnStatus =
        HTTPClient_Read(response->transport, &response->response, buffer, bufferLen, &bytesRead);
    if (returnStatus != HTTPSuccess) {
        return push_error(L, "failed to read response body");
    }

    luaL_addsize(&b, bytesRead);
    luaL_pushresult(&b);
    lua_pushinteger(L, bytesRead);
    return 2;
}

int
l_corehttp_response_create_meta(lua_State* L) {
    luaL_newmetatable(L, LCOREHTTP_RESPONSE_METATABLE);
    /* Metamethods */
    lua_newtable(L);
    lua_pushcfunction(L, l_corehttp_response_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, l_corehttp_response_headers);
    lua_setfield(L, -2, "headers");
    lua_pushcfunction(L, l_corehttp_response_status);
    lua_setfield(L, -2, "status");
    lua_pushcfunction(L, l_corehttp_response_status_code);
    lua_setfield(L, -2, "status_code");
    lua_pushcfunction(L, l_corehttp_response_http_status_code);
    lua_setfield(L, -2, "http_status_code");
    lua_pushcfunction(L, l_corehttp_response_read);
    lua_setfield(L, -2, "read");
    lua_pushstring(L, LCOREHTTP_RESPONSE_METATABLE);
    lua_setfield(L, -2, "__type");
    /* Metamethods */
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, l_corehttp_response_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_corehttp_response_gc);
    lua_setfield(L, -2, "__close");

    return 0;
}

int
l_corehttp_response_headers_get(lua_State* L) {
    if (!lua_istable(L, 1)) {
        return 0;
    }
    const char* headerName = luaL_checkstring(L, 2);

    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        const char* key = lua_tostring(L, -2);
        if (strcasecmp(key, headerName) == 0) {
            return 1;
        }
        lua_pop(L, 1); // pop value
    }
    return 0;
}

int
l_corehttp_response_headers_create_meta(lua_State* L) {
    luaL_newmetatable(L, LCOREHTTP_HEADERS_METATABLE);

    lua_pushcfunction(L, l_corehttp_response_headers_get);
    lua_setfield(L, -2, "__index");

    lua_pushstring(L, LCOREHTTP_HEADERS_METATABLE);
    lua_setfield(L, -2, "__type");

    return 0;
}
