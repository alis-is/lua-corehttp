#ifdef _WIN32
#define LUA_SIMPLE_SOCKET_EXPORT __declspec(dllexport)
#else
#define LUA_SIMPLE_SOCKET_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>

LUA_SIMPLE_SOCKET_EXPORT int luaopen_lua_corehttp(lua_State* L);

#ifdef __cplusplus
}
#endif