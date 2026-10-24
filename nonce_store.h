#ifndef NONCE_STORE_H
#define NONCE_STORE_H

#include <stddef.h>
#include <stdint.h>

bool nonceStoreBegin();
// Explicit full factory reset only; validates image bounds/geometry first.
bool nonceStoreErase();
bool nonceStoreSave(const uint8_t* nonces);
// Call after a successful begin. Copies RadioLib's nonce buffer when history
// exists; capacity must accommodate RADIOLIB_LORAWAN_NONCES_BUF_SIZE bytes.
bool nonceStoreLoad(uint8_t* out, size_t capacity);

#endif
