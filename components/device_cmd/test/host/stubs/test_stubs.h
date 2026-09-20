#pragma once
#include <cstdint>
struct StubSend {
    int calls = 0; int lock_depth = 0; bool locked_during_send = false;
    char kind[8] = {}; uint64_t ieee = 0; uint16_t nwk = 0; uint8_t ep = 0;
    char model[64] = {}; char manu[64] = {}; char key[32] = {};
    bool b = false; uint64_t u = 0; double f = 0; char s[64] = {};
};
struct StubShadow { int writes = 0; uint64_t ieee = 0; char key[32] = {}; uint8_t vt = 0; int32_t val = 0; };
extern StubSend g_send; extern StubShadow g_shadow; extern bool g_send_result;
void stub_pool_set(uint64_t ieee, uint16_t nwk, uint8_t ep0, const char* model, const char* manu);
void stub_pool_clear();
void stub_reset();
