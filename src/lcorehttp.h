#ifndef LCOREHTTP_H
#define LCOREHTTP_H

#ifdef _WIN32
#define LCOREHTTP_EXPORT __declspec(dllexport)
#else
#define LCOREHTTP_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>

LCOREHTTP_EXPORT int luaopen_lua_corehttp(lua_State* L);

#ifdef __cplusplus
}
#endif

#endif /* LCOREHTTP_H */