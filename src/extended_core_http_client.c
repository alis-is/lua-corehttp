#include <assert.h>
#include <stdlib.h>
#include "core_http_client.h"
#include "core_http_client_private.h"
#include "extended_core_http_client.h"
#include "lcorehttp_client.h"

static uint32_t
getZeroTimestampMs(void) {
    return 0U;
}

HTTPStatus_t
HTTPClient_Validate(const TransportInterface_t* pTransport, HTTPRequestHeaders_t* pRequestHeaders,
                    const uint8_t* pRequestBodyBuf, size_t reqBodyBufLen, HTTPResponse_t* pResponse) {
    HTTPStatus_t returnStatus = HTTPInvalidParameter;

    if (pTransport == NULL) {
        LogError(("Parameter check failed: pTransport interface is NULL."));
    } else if (pTransport->send == NULL) {
        LogError(("Parameter check failed: pTransport->send is NULL."));
    } else if (pTransport->recv == NULL) {
        LogError(("Parameter check failed: pTransport->recv is NULL."));
    } else if (pRequestHeaders == NULL) {
        LogError(("Parameter check failed: pRequestHeaders is NULL."));
    } else if (pRequestHeaders->pBuffer == NULL) {
        LogError(("Parameter check failed: pRequestHeaders->pBuffer is NULL."));
    } else if (pRequestHeaders->headersLen < HTTP_MINIMUM_REQUEST_LINE_LENGTH) {
        LogError(("Parameter check failed: pRequestHeaders->headersLen "
                  "does not meet minimum the required length. "
                  "MinimumRequiredLength=%u, HeadersLength=%lu",
                  HTTP_MINIMUM_REQUEST_LINE_LENGTH, (unsigned long)(pRequestHeaders->headersLen)));
    } else if (pRequestHeaders->headersLen > pRequestHeaders->bufferLen) {
        LogError(("Parameter check failed: pRequestHeaders->headersLen > "
                  "pRequestHeaders->bufferLen."));
    } else if (pResponse == NULL) {
        LogError(("Parameter check failed: pResponse is NULL. "));
    } else if (pResponse->pBuffer == NULL) {
        LogError(("Parameter check failed: pResponse->pBuffer is NULL."));
    } else if (pResponse->bufferLen == 0U) {
        LogError(("Parameter check failed: pResponse->bufferLen is zero."));
    } else if ((pRequestBodyBuf == NULL) && (reqBodyBufLen > 0U)) {
        /* If there is no body to send we must ensure that the reqBodyBufLen is
         * zero so that no Content-Length header is automatically written. */
        LogError(("Parameter check failed: pRequestBodyBuf is NULL, but "
                  "reqBodyBufLen is greater than zero."));
    } else if (reqBodyBufLen > (size_t)(INT32_MAX)) {
        /* This check is needed because convertInt32ToAscii() is used on the
         * reqBodyBufLen to create a Content-Length header value string. */
        LogError(("Parameter check failed: reqBodyBufLen > INT32_MAX."
                  "reqBodyBufLen=%lu",
                  (unsigned long)reqBodyBufLen));
    } else {
        if (pResponse->getTime == NULL) {
            /* Set a zero timestamp function when the application did not configure
             * one. */
            pResponse->getTime = getZeroTimestampMs;
        }

        returnStatus = HTTPSuccess;
    }

    return returnStatus;
}

HTTPStatus_t
HTTPClient_Read(const TransportInterface_t* pTransport, HTTPResponse_t* pResponse, uint8_t* pBuffer,
                size_t buffer_capacity, size_t* pBytesRead) {
    HTTPStatus_t returnStatus = HTTPSuccess;
    uint8_t shouldRecv = 1U, timeoutReached = 0U;
    size_t totalReceived = 0U;
    int32_t currentReceived = 0;
    uint32_t lastRecvTimeMs = 0U, timeSinceLastRecvMs = 0U;
    uint32_t retryTimeoutMs = HTTP_RECV_RETRY_TIMEOUT_MS;

    lastRecvTimeMs = pResponse->getTime();

    while (shouldRecv == 1U) {
        /* Receive the HTTP response data into the pResponse->pBuffer. */
        currentReceived =
            pTransport->recv(pTransport->pNetworkContext, pBuffer + totalReceived, buffer_capacity - totalReceived);
        if (currentReceived < 0) {
            LogError(("Failed to receive HTTP data: Transport recv() "
                      "returned error: TransportStatus=%ld",
                      (long int)currentReceived));
            returnStatus = HTTPNetworkError;
        } else if (currentReceived > 0) {
            /* Reset the time of the last data received when data is received. */
            lastRecvTimeMs = pResponse->getTime();
            /* MISRA compliance requires the cast to an unsigned type, since we have checked that
             * the value of current received is greater than 0 we don't need to worry about int overflow. */
            totalReceived += (size_t)currentReceived;
        } else {
            timeSinceLastRecvMs = pResponse->getTime() - lastRecvTimeMs;
            /* Check if the allowed elapsed time between non-zero data has been
             * reached. */
            if (timeSinceLastRecvMs >= retryTimeoutMs) {
                timeoutReached = 1U;
            }
        }
        shouldRecv =
            ((returnStatus == HTTPSuccess) && (timeoutReached == 0U) && (totalReceived < buffer_capacity)) ? 1U : 0U;
    }
    *pBytesRead = totalReceived;

    return returnStatus;
}

HTTPStatus_t
HTTPClient_Write(const TransportInterface_t* pTransport, HTTPClient_GetCurrentTimeFunc_t getTimestampMs,
                 const uint8_t* pData, size_t dataLen) {
    return HTTPClient_SendHttpData(pTransport, getTimestampMs, pData, dataLen);
}