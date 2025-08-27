#pragma once

// Lightweight ETW marker helpers. If ETW_TRACELOGGING is defined and
// TraceLoggingProvider is available, real ETW events are emitted.
// Otherwise, macros are no-ops.

#ifdef ETW_TRACELOGGING
#include <TraceLoggingProvider.h>
#include <windows.h>

// Define provider {B1B9D1B5-7F4C-4A64-9A7A-2A6E3E6AF111}
TRACELOGGING_DECLARE_PROVIDER(g_EtwProvider);

inline void EtwInit() {
    TraceLoggingRegister(g_EtwProvider);
}
inline void EtwShutdown() {
    TraceLoggingUnregister(g_EtwProvider);
}

#define ETW_MARK(name) \
    TraceLoggingWrite(g_EtwProvider, name)

#else

inline void EtwInit() {}
inline void EtwShutdown() {}

#define ETW_MARK(name) do {} while(0)

#endif


