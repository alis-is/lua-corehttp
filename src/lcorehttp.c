
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "lcorehttp_client.h"
#include "lcorehttp_preresponse.h"
#include "lcorehttp_response.h"
#include "lss.h"

static const struct luaL_Reg lua_corehttp[] = {
    {"new_client", l_corehttp_newclient},
    {NULL, NULL}};

// expose HTTPClient_strerror() names as module fields: corehttp.HTTPSuccess, ...
static void
register_status_codes(lua_State* L) {
    for (int statusCode = 0;; statusCode++) {
        const char* statusStr = HTTPClient_strerror(statusCode);
        if (statusStr == NULL) {
            return;
        }
        lua_pushinteger(L, statusCode);
        lua_setfield(L, -2, statusStr);
    }
}

int
luaopen_lua_corehttp(lua_State* L) {
    int results = lua_init_simple_socket(L);
    if (results != 0) {
        return results;
    }
    l_corehttp_client_create_meta(L);
    l_corehttp_response_create_meta(L);
    l_corehttp_preresponse_create_meta(L);
    lua_pop(L, 3); // metatables, kept in the registry

    lua_newtable(L);
    luaL_setfuncs(L, lua_corehttp, 0);

    register_status_codes(L);

    l_corehttp_response_headers_create_meta(L);
    lua_setfield(L, -2, "HEADERS_METATABLE");

    return 1;
}
