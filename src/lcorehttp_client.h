#ifndef LCOREHTTP_CLIENT_H
#define LCOREHTTP_CLIENT_H

#include "lcorehttp_preresponse.h"
#include "lcorehttp_response.h"
#include "lss_transport.h"
#include "lua.h"

#define HTTP_PORT                    80
#define HTTPS_PORT                   443

#define DEFAULT_COREHTTP_BUFFER_SIZE 16384   /* 16KB */
#define MINIMUM_COREHTTP_BUFFER_SIZE 1024    /* 1KB */
#define MAXIMUM_COREHTTP_BUFFER_SIZE 1048576 /* 1MB */

#define TRANSFER_ENCODING_HEADER     "transfer-encoding"

typedef lss_connection NetworkContext;

typedef union lcorehttp_client_connection_options {
    lss_open_tls_connection_options* tls;
    lss_open_connection_options* plaintext;
} lcorehttp_client_connection_options;

typedef struct lcorehttp_client {
    int closed;
    int portno;
    size_t hostname_len;
    const char* hostname;
    lss_connection_kind kind;
} lcorehttp_client;

#define LCOREHTTP_CLIENT_METATABLE "COREHTTP_CLIENT"

int l_corehttp_newclient(lua_State* L);

int l_corehttp_client_create_meta(lua_State* L);
#endif /* LSS_TRANSPORT_MBEDTLS_H */
