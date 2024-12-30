#ifndef EXTENDED_COREHTTP_CLIENT_H
#define EXTENDED_COREHTTP_CLIENT_H

#include "core_http_client.h"

HTTPStatus_t HTTPClient_Validate(const TransportInterface_t* pTransport, HTTPRequestHeaders_t* pRequestHeaders,
                                 const uint8_t* pRequestBodyBuf, size_t reqBodyBufLen, HTTPResponse_t* pResponse);

HTTPStatus_t HTTPClient_Read(const TransportInterface_t* pTransport, HTTPResponse_t* pResponse, uint8_t* pBuffer,
                             size_t buffer_capacity, size_t* pBytesRead);

HTTPStatus_t HTTPClient_Write(const TransportInterface_t* pTransport, HTTPClient_GetCurrentTimeFunc_t getTimestampMs,
                              const uint8_t* pData, size_t dataLen);

#endif /* EXTENDED_COREHTTP_CLIENT_H */