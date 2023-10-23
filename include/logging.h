#ifndef LOGGING_H
#define LOGGING_H

#include <stdarg.h>
#include <stdio.h>

#define LOG_METADATA_FORMAT "%s:%d"
#define LIBRARY_LOG_NAME    "YourLibrary"
#define LOG_METADATA_ARGS   __FILE__, __LINE__

void internal_log(const char* format, ...);

#endif // LOGGING_H