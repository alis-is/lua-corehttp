#include "lcorehttp_response.h"
#include <assert.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "lcorehttp_time.h"
#include "lerror.h"
#include "llhttp.h"
#include "lss_transport.h"

#define MINIMUM_CHUNK_BUFFER_SIZE 512

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

static void
l_corehttp_get_headers_table(lua_State* L, int idx) {
    lua_getiuservalue(L, idx, 1);
    if (lua_istable(L, -1)) {
        return;
    }
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setiuservalue(L, idx, 1);
}

int
l_corehttp_response_headers(lua_State* L) {
    luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);
    l_corehttp_get_headers_table(L, 1);
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

typedef struct {
    z_stream strm;
    int initialized;
} l_zstream_ud;

static int
l_zstream_gc(lua_State* L) {
    l_zstream_ud* ud = (l_zstream_ud*)lua_touserdata(L, 1);
    if (ud->initialized) {
        inflateEnd(&ud->strm);
        ud->initialized = 0;
    }
    return 0;
}

static z_stream*
create_auto_zstream(lua_State* L, int windowBits) {
    l_zstream_ud* ud = (l_zstream_ud*)lua_newuserdatauv(L, sizeof(l_zstream_ud), 0);
    memset(ud, 0, sizeof(l_zstream_ud));

    if (luaL_newmetatable(L, "l_corehttp_zstream_mt")) {
        lua_pushcfunction(L, l_zstream_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);

    if (inflateInit2(&ud->strm, windowBits) != Z_OK) {
        lua_pop(L, 1);
        return NULL;
    }
    ud->initialized = 1;
    return &ud->strm;
}

// Helper to detect Content-Encoding from response headers
static int
l_corehttp_get_encoding_mode(lua_State* L, int respIdx) {
    int mode = 0; // 0: none, 1: gzip, 2: deflate

    l_corehttp_get_headers_table(L, respIdx);

    lua_pushstring(L, "Content-Encoding");
    lua_gettable(L, -2);
    if (lua_isstring(L, -1)) {
        const char* enc = lua_tostring(L, -1);
        if (strcmp(enc, "gzip") == 0) {
            mode = 1;
        } else if (strcmp(enc, "deflate") == 0) {
            mode = 2;
        }
    }
    lua_pop(L, 2); // pop value and headers table
    return mode;
}

// --- Internal Reader ---
// Handles reading from internal cache and network transport
static int
l_corehttp_response_read_internal(lcorehttp_response* response, uint8_t* buffer, size_t bufferLen,
                                  size_t* outBytesRead) {
    *outBytesRead = 0;

    // No Content
    if (response->contentLength == 0 && !response->isChunked) {
        return 0;
    }

    // Read from Cache (pre-fetched body during header parsing)
    if (response->cachedBodyRead < response->response.bodyLen) {
        const uint8_t* pBody = (const uint8_t*)response->response.pBody;
        size_t available = response->response.bodyLen - response->cachedBodyRead;

        size_t toCopy = (available > bufferLen) ? bufferLen : available;
        memcpy(buffer, pBody + response->cachedBodyRead, toCopy);

        response->cachedBodyRead += toCopy;
        *outBytesRead = toCopy;
        return 0;
    }

    // Read from Network
    HTTPStatus_t status = HTTPClient_Read(response->transport, &response->response, buffer, bufferLen, outBytesRead);
    if (status != HTTPSuccess) {
        return -1;
    }
    return 0;
}

// Raw Read
// read(buffer_size?)
int
l_corehttp_response_read(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);

    lua_Integer reqLen = luaL_optinteger(L, 2, DEFAULT_COREHTTP_BUFFER_SIZE);
    if (reqLen <= 0) {
        return 0;
    }

    luaL_Buffer b;
    luaL_buffinit(L, &b);

    // Prep buffer directly in Lua string builder
    uint8_t* buffer = (uint8_t*)luaL_prepbuffsize(&b, (size_t)reqLen);
    size_t bytesRead = 0;

    if (l_corehttp_response_read_internal(response, buffer, (size_t)reqLen, &bytesRead) != 0) {
        return push_error(L, "failed to read response body");
    }

    luaL_addsize(&b, bytesRead);
    luaL_pushresult(&b);           // Return string
    lua_pushinteger(L, bytesRead); // Return count
    return 2;
}

// Content Read
// read_content(write_cb?, progress_cb?, buffer_size?)
int
l_corehttp_response_read_content(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);

    int hasWriteFunc = lua_isfunction(L, 2);
    int hasProgressFunc = lua_isfunction(L, 3);

    lua_Integer cap = luaL_optinteger(L, 4, DEFAULT_COREHTTP_BUFFER_SIZE);
    size_t bufferCapacity = (cap > 0) ? (size_t)cap : DEFAULT_COREHTTP_BUFFER_SIZE;

    int inflateMode = l_corehttp_get_encoding_mode(L, 1);
    size_t contentLength = response->contentLength;
    size_t totalBytesRead = 0;

    // Buffer Allocation (GC managed)
    uint8_t* buffer = (uint8_t*)lua_newuserdatauv(L, bufferCapacity, 0);
    uint8_t* outBuffer = NULL;

    z_stream* strm = NULL;
    if (inflateMode) {
        int windowBits = (inflateMode == 1) ? 31 : 15;
        strm = create_auto_zstream(L, windowBits); // Pushes userdata on stack
        if (!strm) {
            return luaL_error(L, "failed to initialize zlib");
        }
        outBuffer = (uint8_t*)lua_newuserdatauv(L, bufferCapacity, 0);
    }

    luaL_Buffer b;
    if (!hasWriteFunc) {
        luaL_buffinit(L, &b);
    }

    int status = 0;

    while (1) {
        // Calculate read size
        size_t toRead = bufferCapacity;
        if (contentLength != (size_t)-1) {
            size_t remaining = contentLength - totalBytesRead;
            if (remaining == 0) {
                break;
            }
            if (remaining < toRead) {
                toRead = remaining;
            }
        }

        size_t bytesRead = 0;
        int ret = l_corehttp_response_read_internal(response, buffer, toRead, &bytesRead);

        if (ret != 0) {
            status = -1;
            break;
        }
        if (bytesRead == 0) {
            break; // EOF
        }

        totalBytesRead += bytesRead;

        // Progress Callback
        if (hasProgressFunc) {
            lua_pushvalue(L, 3);
            lua_pushinteger(L, (contentLength != (size_t)-1) ? contentLength : 0);
            lua_pushinteger(L, totalBytesRead);
            lua_call(L, 2, 0);
        }

        // Process Data
        if (inflateMode) {
            strm->next_in = (Bytef*)buffer;
            strm->avail_in = bytesRead;
            while (strm->avail_in > 0) {
                strm->next_out = outBuffer;
                strm->avail_out = bufferCapacity;
                int zRet = inflate(strm, Z_NO_FLUSH);

                if (zRet != Z_OK && zRet != Z_STREAM_END) {
                    status = -2;
                    break;
                }

                size_t have = bufferCapacity - strm->avail_out;
                if (have > 0) {
                    if (hasWriteFunc) {
                        lua_pushvalue(L, 2);
                        lua_pushlstring(L, (const char*)outBuffer, have);
                        lua_call(L, 1, 0);
                    } else {
                        luaL_addlstring(&b, (const char*)outBuffer, have);
                    }
                }
                if (status == -2 || zRet == Z_STREAM_END) {
                    break;
                }
            }
            if (status != 0) {
                break;
            }
        } else {
            if (hasWriteFunc) {
                lua_pushvalue(L, 2);
                lua_pushlstring(L, (const char*)buffer, bytesRead);
                lua_call(L, 1, 0);
            } else {
                luaL_addlstring(&b, (const char*)buffer, bytesRead);
            }
        }

        if (contentLength != (size_t)-1 && totalBytesRead >= contentLength) {
            break;
        }
    }

    if (status == -2) {
        return luaL_error(L, "inflate error");
    }
    if (status == -1) {
        return luaL_error(L, "network error");
    }

    if (contentLength != (size_t)-1 && contentLength > 0 && totalBytesRead < contentLength) {
        return luaL_error(L, "incomplete read: expected %I bytes, got %I", (lua_Integer)contentLength,
                          (lua_Integer)totalBytesRead);
    }

    if (!hasWriteFunc) {
        luaL_pushresult(&b);
    } else {
        lua_pushinteger(L, totalBytesRead);
    }

    return 1;
}

// Chunked Read
// read_chunked_content(write_cb?, progress_cb?, buffer_size?)
int
l_corehttp_response_read_chunked_content(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);
    int hasWriteFunc = lua_isfunction(L, 2);
    int hasProgressFunc = lua_isfunction(L, 3);

    lua_Integer cap = luaL_optinteger(L, 4, DEFAULT_COREHTTP_BUFFER_SIZE);
    size_t bufferCapacity = (cap >= MINIMUM_CHUNK_BUFFER_SIZE) ? (size_t)cap : MINIMUM_CHUNK_BUFFER_SIZE;

    int inflateMode = l_corehttp_get_encoding_mode(L, 1);

    // Buffers
    uint8_t* buffer = (uint8_t*)lua_newuserdatauv(L, bufferCapacity, 0);
    uint8_t* outBuffer = NULL;

    z_stream* strm = NULL;
    if (inflateMode) {
        int windowBits = (inflateMode == 1) ? 31 : 15;
        strm = create_auto_zstream(L, windowBits);
        if (!strm) {
            return luaL_error(L, "failed to initialize zlib");
        }
        outBuffer = (uint8_t*)lua_newuserdatauv(L, bufferCapacity, 0);
    }

    luaL_Buffer b;
    if (!hasWriteFunc) {
        luaL_buffinit(L, &b);
    }

    // State Machine: 0=Header, 1=Data, 2=Trailing CRLF
    int state = 0;
    size_t chunkBytesRemaining = 0;
    size_t totalBytesRead = 0;
    size_t cacheLen = 0; // Bytes valid
    size_t cacheOff = 0; // Read position
    int done = 0;

    while (!done) {
        // Buffer Management
        if (cacheOff >= cacheLen) {
            cacheOff = 0;
            cacheLen = 0;
        } else if (cacheLen == bufferCapacity && cacheOff > 0) {
            // Compact
            size_t remaining = cacheLen - cacheOff;
            memmove(buffer, buffer + cacheOff, remaining);
            cacheLen = remaining;
            cacheOff = 0;
        }

        // Network Read
        if (cacheLen < bufferCapacity) {
            size_t readAmt = 0;
            int ret =
                l_corehttp_response_read_internal(response, buffer + cacheLen, bufferCapacity - cacheLen, &readAmt);

            if (ret != 0) {
                return luaL_error(L, "network error: %d", ret);
            }

            if (readAmt == 0) {
                // EOF logic
                if (state == 0 && chunkBytesRemaining == 0) {
                    break; // Acceptable clean EOF
                }
                if (cacheLen - cacheOff == 0) {
                    break; // Unexpected EOF
                }
            }
            cacheLen += readAmt;
        }

        size_t available = cacheLen - cacheOff;
        if (available == 0) {
            break; // EOF
        }

        uint8_t* p = buffer + cacheOff;

        // Parsing
        if (state == 0) { // Chunk Size
            uint8_t* lf = memchr(p, '\n', available);
            if (!lf) {
                if (cacheLen == bufferCapacity) {
                    return luaL_error(L, "chunk header too long");
                }
                continue; // Need more data
            }

            size_t lineLen = lf - p + 1;
            char lenStr[32];
            size_t hexLen = 0;

            // Extract Hex, stop at separator or CRLF
            for (size_t i = 0; i < lineLen; i++) {
                if (p[i] == ';' || p[i] == '\r' || p[i] == '\n') {
                    break;
                }
                if (hexLen < 31) {
                    lenStr[hexLen++] = p[i];
                }
            }
            lenStr[hexLen] = 0;

            if (hexLen == 0) {
                return luaL_error(L, "invalid chunk header");
            }

            char* endPtr;
            unsigned long sz = strtoul(lenStr, &endPtr, 16);
            if (*endPtr != 0) {
                return luaL_error(L, "invalid chunk size syntax");
            }

            chunkBytesRemaining = sz;
            cacheOff += lineLen;

            if (sz == 0) {
                done = 1;
            } else {
                state = 1;
            }

        } else if (state == 1) { // Data
            size_t toProcess = (available < chunkBytesRemaining) ? available : chunkBytesRemaining;

            if (toProcess > 0) {
                if (inflateMode) {
                    strm->next_in = (Bytef*)p;
                    strm->avail_in = toProcess;
                    while (strm->avail_in > 0) {
                        strm->next_out = outBuffer;
                        strm->avail_out = bufferCapacity;
                        int zRet = inflate(strm, Z_NO_FLUSH);

                        if (zRet != Z_OK && zRet != Z_STREAM_END) {
                            return luaL_error(L, "inflate error: %d", zRet);
                        }

                        size_t have = bufferCapacity - strm->avail_out;
                        if (have > 0) {
                            if (hasWriteFunc) {
                                lua_pushvalue(L, 2);
                                lua_pushlstring(L, (const char*)outBuffer, have);
                                lua_call(L, 1, 0);
                            } else {
                                luaL_addlstring(&b, (const char*)outBuffer, have);
                            }
                        }
                        if (zRet == Z_STREAM_END) {
                            break;
                        }
                    }
                } else {
                    if (hasWriteFunc) {
                        lua_pushvalue(L, 2);
                        lua_pushlstring(L, (const char*)p, toProcess);
                        lua_call(L, 1, 0);
                    } else {
                        luaL_addlstring(&b, (const char*)p, toProcess);
                    }
                }
                totalBytesRead += toProcess;
                chunkBytesRemaining -= toProcess;
                cacheOff += toProcess;

                if (hasProgressFunc) {
                    lua_pushvalue(L, 3);
                    lua_pushinteger(L, 0); // Unknown total
                    lua_pushinteger(L, totalBytesRead);
                    lua_call(L, 2, 0);
                }
            }

            if (chunkBytesRemaining == 0) {
                state = 2;
            }

        } else if (state == 2) { // Trailing CRLF
            if (available < 2) {
                continue; // Need \r\n
            }
            if (p[0] != '\r' || p[1] != '\n') {
                return luaL_error(L, "expected CRLF after chunk data");
            }
            cacheOff += 2;
            state = 0;
        }
    }

    if (!done && state != 0) {
        return luaL_error(L, "unexpected EOF in chunked body");
    }

    if (!hasWriteFunc) {
        luaL_pushresult(&b);
    } else {
        lua_pushinteger(L, totalBytesRead);
    }

    return 1;
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
    lua_pushcfunction(L, l_corehttp_response_read_content);
    lua_setfield(L, -2, "read_content");
    lua_pushcfunction(L, l_corehttp_response_read_chunked_content);
    lua_setfield(L, -2, "read_chunked_content");
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
