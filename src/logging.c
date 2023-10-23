#include "logging.h"

void
internal_log(const char* format, ...) {
    va_list args;
    printf("[%s] " LOG_METADATA_FORMAT " ", LIBRARY_LOG_NAME, LOG_METADATA_ARGS);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\r\n");
}
