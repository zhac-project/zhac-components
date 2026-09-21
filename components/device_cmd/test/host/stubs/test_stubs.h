#pragma once
#include <cstdint>
struct StubSend {
    int calls = 0; int lock_depth = 0; bool locked_during_send = false;
    char kind[8] = {}; uint64_t ieee = 0; uint16_t nwk = 0; uint8_t ep = 0;
    char model[64] = {}; char manu[64] = {}; char key[32] = {};
    bool b = false; uint64_t u = 0; double f = 0; char s[64] = {};
};
struct StubShadow { int writes = 0; uint64_t ieee = 0; char key[32] = {}; uint8_t vt = 0; int32_t val = 0; };
struct StubStore {
    int dirty_marks = 0; int dirty_pri = -1; char dirty_name[30] = {}; uint8_t dirty_flags = 0;
    int deletes = 0; int shadow_removes = 0; int def_cache_invalidates = 0; int fallback_clears = 0;
    int pool_removes = 0; int leave_reqs = 0; uint16_t leave_nwk = 0; uint64_t leave_ieee = 0;
    int permit_calls = 0; uint8_t permit_secs = 0; int backend_removes = 0;
    int changed_calls = 0; uint64_t changed_ieee = 0;
    int leave_events = 0; uint64_t leave_event_ieee = 0;
};
extern StubSend g_send; extern StubShadow g_shadow; extern StubStore g_store;
extern bool g_send_result; extern bool g_radio_result; extern bool g_have_backend;
void stub_pool_set(uint64_t ieee, uint16_t nwk, uint8_t ep0, const char* model, const char* manu);
void stub_pool_clear();
void stub_reset();
bool stub_pool_removed_flag();   // the REMOVED tombstone on the pool device
bool stub_pool_has_device();
