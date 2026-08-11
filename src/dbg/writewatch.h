#pragma once

#include "_global.h"

// Data-pattern write watch (bpmatch command). Returns true when the
// first-chance exception was handled by a watch (either transparently
// continued, or paused on a match).
bool WriteWatchHandleException(EXCEPTION_DEBUG_INFO* ExceptionData);

// bpmatch <hex>[, <addr>[, <size>]] | bpmatch clear
bool cbDebugBpMatch(int argc, char* argv[]);
