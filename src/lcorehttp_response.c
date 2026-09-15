#include "lcorehttp_response.h"
#include "lcorehttp_client.h"
#include <errno.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include "lcorehttp_time.h"
#include "lerror.h"
#include "lss_transport.h"

lcorehttp_response*
l_corehttp_new_response(lua_State* L) {
    lcorehttp_response* response = lua_newuserdatauv(L, sizeof(lcorehttp_response), 1);
    luaL_getmetatable(L, LCOREHTTP_RESPONSE_METATABLE);
    lua_setmetatable(L, -2);
    memset(response, 0, sizeof(lcorehttp_response));
    response->response.getTime = l_corehttp_get_time_ms;
    response->contentLength = LCOREHTTP_CONTENT_LENGTH_UNKNOWN;
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
    if (response->response.pBuffer != NULL) {
        free(response->response.pBuffer);
        response->response.pBuffer = NULL;
        response->response.bufferLen = 0;
        response->response.pBody = NULL;
        response->response.bodyLen = 0;
        response->cachedBodyRead = 0;
    }

    return 0;
}

int
l_corehttp_response_tostring(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);
    if (response->isChunked) {
        lua_pushfstring(L, "%p (chunked)", lua_touserdata(L, 1));
        return 1;
    }
    lua_pushfstring(L, "%p (%I bytes)", lua_touserdata(L, 1), (lua_Integer)response->contentLength);
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

typedef enum lcorehttp_read_status {
    LCOREHTTP_READ_OK = 0,
    LCOREHTTP_READ_ERROR = -1,   // transport or parse failure
    LCOREHTTP_READ_TIMEOUT = -2, // no data before HTTP_RECV_RETRY_TIMEOUT_MS
} lcorehttp_read_status;

// --- Internal Reader ---
// Handles reading from internal cache and network transport.
// Returns LCOREHTTP_READ_OK on success (0 bytes means EOF).
static lcorehttp_read_status
l_corehttp_response_read_internal(lcorehttp_response* response, uint8_t* buffer, size_t bufferLen,
                                  size_t* outBytesRead) {
    *outBytesRead = 0;

    // No Content
    if (response->contentLength == 0 && !response->isChunked) {
        return LCOREHTTP_READ_OK;
    }

    // Read from Cache (pre-fetched body during header parsing)
    if (response->cachedBodyRead < response->response.bodyLen) {
        const uint8_t* pBody = (const uint8_t*)response->response.pBody;
        size_t available = response->response.bodyLen - response->cachedBodyRead;

        size_t toCopy = (available > bufferLen) ? bufferLen : available;
        memcpy(buffer, pBody + response->cachedBodyRead, toCopy);

        response->cachedBodyRead += toCopy;
        *outBytesRead = toCopy;
        return LCOREHTTP_READ_OK;
    }

    // Close-delimited body: read until the transport reports EOF; a transport
    // or TLS failure is a truncated download, not an end of body.
    if (response->contentLength == LCOREHTTP_CONTENT_LENGTH_UNKNOWN && !response->isChunked) {
        uint32_t startTimeMs = l_corehttp_get_time_ms();
        while (1) {
            int32_t received = response->transport->recv(response->transport->pNetworkContext, buffer, bufferLen);
            if (received > 0) {
                *outBytesRead = (size_t)received;
                return LCOREHTTP_READ_OK;
            }
            if (received == LSS_TRANSPORT_EOF) {
                return LCOREHTTP_READ_OK; // clean end of a close-delimited body
            }
            if (received < 0) {
                return LCOREHTTP_READ_ERROR; // transport error: surface truncated data
            }
            // Zero means the configured read timeout elapsed without data. Keep
            // waiting briefly instead of reporting EOF, then surface a timeout.
            if ((uint32_t)(l_corehttp_get_time_ms() - startTimeMs) >= HTTP_RECV_RETRY_TIMEOUT_MS) {
                return LCOREHTTP_READ_TIMEOUT;
            }
        }
    }

    // Read from Network
    HTTPStatus_t status = HTTPClient_Read(response->transport, &response->response, buffer, bufferLen, outBytesRead);
    if (status != HTTPSuccess) {
        return LCOREHTTP_READ_ERROR;
    }
    return LCOREHTTP_READ_OK;
}

typedef struct lcorehttp_reader_sink {
    lua_State* L;
    int writeFuncIdx;
    int progressFuncIdx;
    luaL_Buffer buffer;
    int useBuffer;
    size_t totalBytesRead;
} lcorehttp_reader_sink;

static void
sink_init(lua_State* L, lcorehttp_reader_sink* sink, int writeFuncIdx, int progressFuncIdx) {
    sink->L = L;
    sink->writeFuncIdx = writeFuncIdx;
    sink->progressFuncIdx = progressFuncIdx;
    sink->useBuffer = writeFuncIdx == 0;
    sink->totalBytesRead = 0;
    if (sink->useBuffer) {
        luaL_buffinit(L, &sink->buffer);
    }
}

// Callbacks are balanced on the stack, which keeps the active luaL_Buffer valid across them.
static void
sink_emit(lcorehttp_reader_sink* sink, const char* data, size_t len) {
    if (len == 0) {
        return;
    }
    if (sink->useBuffer) {
        luaL_addlstring(&sink->buffer, data, len);
        return;
    }
    lua_pushvalue(sink->L, sink->writeFuncIdx);
    lua_pushlstring(sink->L, data, len);
    lua_call(sink->L, 1, 0);
}

static void
sink_progress(lcorehttp_reader_sink* sink, size_t total) {
    if (sink->progressFuncIdx == 0) {
        return;
    }
    lua_pushvalue(sink->L, sink->progressFuncIdx);
    lua_pushinteger(sink->L, total == LCOREHTTP_CONTENT_LENGTH_UNKNOWN ? -1 : (lua_Integer)total);
    lua_pushinteger(sink->L, (lua_Integer)sink->totalBytesRead);
    lua_call(sink->L, 2, 0);
}

// Inflates input into the sink. Returns -1 on error, 1 when the zlib stream ended, 0 otherwise.
static int
inflate_to_sink(lcorehttp_reader_sink* sink, z_stream* strm, const uint8_t* input, size_t inputLen, uint8_t* output,
              size_t outputCapacity, size_t* consumed) {
    strm->next_in = (Bytef*)input;
    strm->avail_in = inputLen;
    *consumed = 0;

    while (strm->avail_in > 0) {
        strm->next_out = output;
        strm->avail_out = outputCapacity;
        int zRet = inflate(strm, Z_NO_FLUSH);
        if (zRet != Z_OK && zRet != Z_STREAM_END) {
            return -1;
        }

        size_t produced = outputCapacity - strm->avail_out;
        sink_emit(sink, (const char*)output, produced);

        if (zRet == Z_STREAM_END) {
            *consumed = inputLen - strm->avail_in;
            return 1;
        }
        if (produced == 0 && zRet == Z_OK) {
            *consumed = inputLen - strm->avail_in;
            return 0;
        }
    }

    *consumed = inputLen - strm->avail_in;
    return 0;
}

// Buffers, optional inflate stream and callbacks shared by read_content and
// read_chunked_content. Buffers are userdata so the GC owns them.
typedef struct lcorehttp_reader {
    size_t bufferCapacity;
    uint8_t* buffer;
    uint8_t* outBuffer;
    z_stream* strm;
    lcorehttp_reader_sink sink;
} lcorehttp_reader;

static int
reader_init(lua_State* L, size_t bufferCapacity, lcorehttp_reader* reader) {
    reader->bufferCapacity = bufferCapacity;

    int inflateMode = l_corehttp_get_encoding_mode(L, 1);
    reader->strm = NULL;
    reader->outBuffer = NULL;
    reader->buffer = (uint8_t*)lua_newuserdatauv(L, bufferCapacity, 0);
    if (inflateMode) {
        int windowBits = (inflateMode == 1) ? 31 : 15;
        reader->strm = create_auto_zstream(L, windowBits); // Pushes userdata on stack
        if (reader->strm == NULL) {
            return push_error(L, "failed to initialize zlib");
        }
        reader->outBuffer = (uint8_t*)lua_newuserdatauv(L, bufferCapacity, 0);
    }

    sink_init(L, &reader->sink, lua_isfunction(L, 2) ? 2 : 0, lua_isfunction(L, 3) ? 3 : 0);
    return 0;
}

// Pushes the collected body (or bytes written count when a write callback was used).
static int
reader_finish(lua_State* L, lcorehttp_reader* reader) {
    if (reader->sink.useBuffer) {
        luaL_pushresult(&reader->sink.buffer);
    } else {
        lua_pushinteger(L, (lua_Integer)reader->sink.totalBytesRead);
    }
    return 1;
}

// Raw Read
// read(buffer_size?)
int
l_corehttp_response_read(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);
    if (response->transport == NULL) {
        return push_error(L, "response is closed");
    }

    lua_Integer reqLen = luaL_optinteger(L, 2, DEFAULT_COREHTTP_BUFFER_SIZE);
    if (reqLen <= 0) {
        return 0;
    }

    luaL_Buffer b;
    luaL_buffinit(L, &b);

    // Prep buffer directly in Lua string builder
    uint8_t* buffer = (uint8_t*)luaL_prepbuffsize(&b, (size_t)reqLen);
    size_t bytesRead = 0;

    int result = l_corehttp_response_read_internal(response, buffer, (size_t)reqLen, &bytesRead);
    if (result == LCOREHTTP_READ_TIMEOUT) {
        return push_error(L, "read timeout");
    }
    if (result != LCOREHTTP_READ_OK) {
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
    if (response->transport == NULL) {
        return push_error(L, "response is closed");
    }

    lua_Integer cap = luaL_optinteger(L, 4, DEFAULT_COREHTTP_BUFFER_SIZE);
    size_t bufferCapacity = (cap > 0) ? (size_t)cap : DEFAULT_COREHTTP_BUFFER_SIZE;

    lcorehttp_reader reader;
    int result = reader_init(L, bufferCapacity, &reader);
    if (result != 0) {
        return result;
    }

    size_t contentLength = response->contentLength;
    while (1) {
        // Calculate read size
        size_t toRead = bufferCapacity;
        if (contentLength != LCOREHTTP_CONTENT_LENGTH_UNKNOWN) {
            size_t remaining = contentLength - reader.sink.totalBytesRead;
            if (remaining == 0) {
                break;
            }
            if (remaining < toRead) {
                toRead = remaining;
            }
        }

        size_t bytesRead = 0;
        int ret = l_corehttp_response_read_internal(response, reader.buffer, toRead, &bytesRead);
        if (ret == LCOREHTTP_READ_TIMEOUT) {
            return push_error(L, "read timeout");
        }
        if (ret != LCOREHTTP_READ_OK) {
            return push_error(L, "failed to read response body");
        }
        if (bytesRead == 0) {
            break; // EOF
        }

        reader.sink.totalBytesRead += bytesRead;

        // Progress Callback
        sink_progress(&reader.sink, contentLength);

        // Process Data
        if (reader.strm != NULL) {
            size_t consumed = 0;
            if (inflate_to_sink(&reader.sink, reader.strm, reader.buffer, bytesRead, reader.outBuffer, bufferCapacity,
                              &consumed) < 0) {
                return push_error(L, "inflate error");
            }
        } else {
            sink_emit(&reader.sink, (const char*)reader.buffer, bytesRead);
        }

        if (contentLength != LCOREHTTP_CONTENT_LENGTH_UNKNOWN && reader.sink.totalBytesRead >= contentLength) {
            break;
        }
    }

    if (contentLength != LCOREHTTP_CONTENT_LENGTH_UNKNOWN && contentLength > 0
        && reader.sink.totalBytesRead < contentLength) {
        lua_pushfstring(L, "incomplete read: expected %I bytes, got %I", (lua_Integer)contentLength,
                        (lua_Integer)reader.sink.totalBytesRead);
        return push_error(L, lua_tostring(L, -1));
    }

    return reader_finish(L, &reader);
}

// Chunked Read
// read_chunked_content(write_cb?, progress_cb?, buffer_size?)

typedef enum lcorehttp_chunk_state {
    LCOREHTTP_CHUNK_HEADER = 0,
    LCOREHTTP_CHUNK_DATA = 1,
    LCOREHTTP_CHUNK_TRAILER = 2,
} lcorehttp_chunk_state;

int
l_corehttp_response_read_chunked_content(lua_State* L) {
    lcorehttp_response* response = luaL_checkudata(L, 1, LCOREHTTP_RESPONSE_METATABLE);
    if (response->transport == NULL) {
        return push_error(L, "response is closed");
    }

    lua_Integer cap = luaL_optinteger(L, 4, DEFAULT_COREHTTP_BUFFER_SIZE);
    size_t bufferCapacity = (cap >= MINIMUM_COREHTTP_BUFFER_SIZE) ? (size_t)cap : MINIMUM_COREHTTP_BUFFER_SIZE;

    lcorehttp_reader reader;
    int result = reader_init(L, bufferCapacity, &reader);
    if (result != 0) {
        return result;
    }

    lcorehttp_chunk_state state = LCOREHTTP_CHUNK_HEADER;
    size_t chunkBytesRemaining = 0;
    size_t cacheLen = 0;
    size_t cacheOff = 0;
    int zlibStreamEnded = 0;
    int done = 0;

    while (!done) {
        size_t available = cacheLen - cacheOff;
        uint8_t* p = reader.buffer + cacheOff;
        int madeProgress = 0;

        // ATTEMPT TO PARSE
        if (state == LCOREHTTP_CHUNK_HEADER) {
            uint8_t* lf = memchr(p, '\n', available);
            if (lf) {
                size_t lineLen = lf - p + 1;

                char lenStr[32];
                size_t hexLen = 0;
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
                    return push_error(L, "invalid chunk header");
                }

                char* endPtr = NULL;
                errno = 0;
                unsigned long sz = strtoul(lenStr, &endPtr, 16);
                if (endPtr == lenStr || *endPtr != 0 || errno == ERANGE || sz > SIZE_MAX - 5U) {
                    return push_error(L, "invalid chunk size");
                }

                chunkBytesRemaining = (size_t)sz;
                cacheOff += lineLen;

                if (sz == 0) {
                    done = 1;
                } else {
                    state = LCOREHTTP_CHUNK_DATA;
                }
                madeProgress = 1;
            } else {
                if (available == bufferCapacity) {
                    return push_error(L, "chunk header too long");
                }
            }

        } else if (state == LCOREHTTP_CHUNK_DATA) {
            size_t toProcess = (available < chunkBytesRemaining) ? available : chunkBytesRemaining;

            if (toProcess > 0) {
                if (reader.strm != NULL && !zlibStreamEnded) {
                    size_t consumed = 0;
                    int zRet =
                        inflate_to_sink(&reader.sink, reader.strm, p, toProcess, reader.outBuffer, bufferCapacity,
                                      &consumed);
                    if (zRet < 0) {
                        return push_error(L, "inflate error");
                    }
                    if (zRet == 1) {
                        zlibStreamEnded = 1;
                    }
                    toProcess = consumed;
                } else {
                    sink_emit(&reader.sink, (const char*)p, toProcess);
                }

                reader.sink.totalBytesRead += toProcess;
                chunkBytesRemaining -= toProcess;
                cacheOff += toProcess;

                sink_progress(&reader.sink, LCOREHTTP_CONTENT_LENGTH_UNKNOWN); // unknown total for chunked

                if (chunkBytesRemaining == 0) {
                    state = LCOREHTTP_CHUNK_TRAILER;
                }
                madeProgress = 1;
            }

        } else if (state == LCOREHTTP_CHUNK_TRAILER) {
            if (available >= 2) {
                if (p[0] != '\r' || p[1] != '\n') {
                    return push_error(L, "expected CRLF after chunk");
                }
                cacheOff += 2;
                state = LCOREHTTP_CHUNK_HEADER;
                madeProgress = 1;
            }
        }

        if (madeProgress) {
            continue;
        }

        if (done) {
            break;
        }

        // --- NETWORK READ ---
        // Compact buffer first
        if (cacheOff > 0) {
            size_t remaining = cacheLen - cacheOff;
            if (remaining > 0) {
                memmove(reader.buffer, reader.buffer + cacheOff, remaining);
            }
            cacheLen = remaining;
            cacheOff = 0;
        }

        // Calculate how much we NEED to read (not the whole buffer!)
        size_t bytesNeeded = 0;
        if (state == LCOREHTTP_CHUNK_HEADER) {
            // Header: Minimum is "0\r\n" = 3 bytes
            bytesNeeded = 3;
        } else if (state == LCOREHTTP_CHUNK_DATA) {
            // Data: We need the remaining chunk bytes + 2 for CRLF + 5 for next header ("0\r\n" = 3, typical = 5)
            bytesNeeded = chunkBytesRemaining + 5;
        } else if (state == LCOREHTTP_CHUNK_TRAILER) {
            // CRLF: We need exactly 2 bytes
            bytesNeeded = 2;
        }

        // Don't read more than we have space for
        size_t spaceAvailable = bufferCapacity - cacheLen;
        if (spaceAvailable == 0) {
            return push_error(L, "chunk buffer exhausted");
        }
        size_t toRead = (bytesNeeded < spaceAvailable) ? bytesNeeded : spaceAvailable;

        size_t readAmt = 0;
        int ret = l_corehttp_response_read_internal(response, reader.buffer + cacheLen, toRead, &readAmt);
        if (ret != LCOREHTTP_READ_OK) {
            return push_error(L, "network error");
        }

        if (readAmt == 0) {
            return push_error(L, "unexpected EOF");
        }

        cacheLen += readAmt;
    }

    return reader_finish(L, &reader);
}

static const luaL_Reg lcorehttp_response_methods[] = {
    {"headers", l_corehttp_response_headers},
    {"status", l_corehttp_response_status},
    {"status_code", l_corehttp_response_status_code},
    {"http_status_code", l_corehttp_response_http_status_code},
    {"read", l_corehttp_response_read},
    {"read_content", l_corehttp_response_read_content},
    {"read_chunked_content", l_corehttp_response_read_chunked_content},
    {NULL, NULL}};

static const luaL_Reg lcorehttp_response_metamethods[] = {
    {"__tostring", l_corehttp_response_tostring},
    {"__gc", l_corehttp_response_gc},
    {"__close", l_corehttp_response_gc},
    {NULL, NULL}};

int
l_corehttp_response_create_meta(lua_State* L) {
    luaL_newmetatable(L, LCOREHTTP_RESPONSE_METATABLE);
    luaL_setfuncs(L, lcorehttp_response_metamethods, 0);

    lua_newtable(L);
    luaL_setfuncs(L, lcorehttp_response_methods, 0);
    lua_pushstring(L, LCOREHTTP_RESPONSE_METATABLE);
    lua_setfield(L, -2, "__type");
    lua_setfield(L, -2, "__index");

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
        lua_pushvalue(L, -2);
        const char* key = lua_tostring(L, -1);
        int matched = key != NULL && strcasecmp(key, headerName) == 0;
        lua_pop(L, 1); // key copy
        if (matched) {
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
