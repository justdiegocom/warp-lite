// The phone's core (mobile/core): warp-remote's client behind a synchronous C interface. Every call blocks
// until it is done, so call it from a background queue. Requests and answers are JSON (see src/lib.rs).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WrCore WrCore;

// state_dir: where this device keeps its key and its paired computer. device_name: shown in Warp.
WrCore *wr_core_open(const char *state_dir, const char *device_name);
// request_json → answer JSON; free the answer with wr_string_free.
char *wr_core_call(WrCore *core, const char *request_json);
void wr_string_free(char *s);
void wr_core_close(WrCore *core);

#ifdef __cplusplus
}
#endif
