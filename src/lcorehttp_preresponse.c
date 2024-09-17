#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <string.h>
#include "core_http_client.h"
#include "extended_core_http_client.h"
#include "lcorehttp_preresponse.h"
#include "lerror.h"

lcorehttp_preresponse*
l_corehttp_new_preresponse(lua_State* L) {
    lcorehttp_preresponse* preresponse = lua_newuserdatauv(L, sizeof(lcorehttp_preresponse), 0);
    if (preresponse == NULL) {
        return NULL;
    }
    luaL_getmetatable(L, LCOREHTTP_PRERESPONSE_METATABLE);
    lua_setmetatable(L, -2);

    return preresponse;
}

int
l_corehttp_preresponse_write(lua_State* L) {
    lcorehttp_preresponse* preresponse = luaL_checkudata(L, 1, LCOREHTTP_PRERESPONSE_METATABLE);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    if (preresponse->transport == NULL) {
        return push_error(L, "preresponse is closed");
    }
    HTTPStatus_t status =
        HTTPClient_Write(preresponse->transport, preresponse->response->getTime, (const uint8_t*)data, len);
    if (status != HTTPSuccess) {
        return push_error(L, "failed to write to preresponse");
    }
    return 0;
}

int
l_corehttp_preresponse_gc(lua_State* L) {
    lcorehttp_preresponse* preresponse = luaL_checkudata(L, 1, LCOREHTTP_PRERESPONSE_METATABLE);

    // noop - preresponse is closed by response

    return 0;
}

int
l_corehttp_preresponse_create_meta(lua_State* L) {
    luaL_newmetatable(L, LCOREHTTP_PRERESPONSE_METATABLE);

    lua_newtable(L);
    lua_pushcfunction(L, l_corehttp_preresponse_write);
    lua_setfield(L, -2, "write");
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, l_corehttp_preresponse_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_corehttp_preresponse_gc);
    lua_setfield(L, -2, "__close");

    return 0;
}