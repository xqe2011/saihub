/**
 * @name Tool helpers
 * @file tool.c
 * @author xqe2011
 */
#include "tool.h"

#include <stdlib.h>
#include <string.h>

static bool Tool_RingReady(const Tool_Ring* ring, size_t len, bool hasBuf)
{
  return ring != NULL && ring->data != NULL && ring->cap > 0 && (len == 0 || hasBuf);
}

void Tool_RingInit(Tool_Ring* ring, uint8_t* data, size_t cap)
{
  if (ring == NULL) return;
  ring->data = data;
  ring->cap = data == NULL ? 0 : cap;
  ring->head = 0;
  ring->tail = 0;
  ring->used = 0;
  ring->owned = false;
}

bool Tool_RingCreate(Tool_Ring* ring, size_t cap)
{
  if (ring == NULL || cap == 0) return false;
  uint8_t* data = malloc(cap);
  if (data == NULL) return false;
  if (ring->owned) free(ring->data);
  ring->data = data;
  ring->cap = cap;
  ring->head = 0;
  ring->tail = 0;
  ring->used = 0;
  ring->owned = true;
  return true;
}

void Tool_RingRelease(Tool_Ring* ring)
{
  if (ring == NULL) return;
  if (ring->owned) free(ring->data);
  ring->data = NULL;
  ring->cap = 0;
  ring->head = 0;
  ring->tail = 0;
  ring->used = 0;
  ring->owned = false;
}

void Tool_RingClear(Tool_Ring* ring)
{
  if (ring == NULL) return;
  ring->head = 0;
  ring->tail = 0;
  ring->used = 0;
}

size_t Tool_RingLen(const Tool_Ring* ring)
{
  return ring == NULL ? 0 : ring->used;
}

size_t Tool_RingFree(const Tool_Ring* ring)
{
  if (ring == NULL || ring->cap < ring->used) return 0;
  return ring->cap - ring->used;
}

bool Tool_RingWrite(Tool_Ring* ring, const void* src, size_t len)
{
  if (!Tool_RingReady(ring, len, src != NULL) || len > Tool_RingFree(ring)) return false;
  const uint8_t* in = src;
  size_t first = ring->cap - ring->tail;
  if (first > len) first = len;
  if (first > 0) memcpy(ring->data + ring->tail, in, first);
  if (len > first) memcpy(ring->data, in + first, len - first);
  ring->used += len;
  ring->tail += len;
  if (ring->tail >= ring->cap) ring->tail -= ring->cap;
  return true;
}

bool Tool_RingPeek(const Tool_Ring* ring, void* dst, size_t len)
{
  if (!Tool_RingReady(ring, len, dst != NULL) || len > ring->used) return false;
  uint8_t* out = dst;
  size_t first = ring->cap - ring->head;
  if (first > len) first = len;
  if (first > 0) memcpy(out, ring->data + ring->head, first);
  if (len > first) memcpy(out + first, ring->data, len - first);
  return true;
}

bool Tool_RingDrop(Tool_Ring* ring, size_t len)
{
  if (ring == NULL || len > ring->used) return false;
  ring->used -= len;
  if (ring->used == 0) {
    ring->head = 0;
    ring->tail = 0;
    return true;
  }
  ring->head += len;
  if (ring->head >= ring->cap) ring->head -= ring->cap;
  return true;
}

bool Tool_RingRead(Tool_Ring* ring, void* dst, size_t len)
{
  if (!Tool_RingPeek(ring, dst, len)) return false;
  return Tool_RingDrop(ring, len);
}

static int Tool_HexNibble(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool Tool_HexDecode(const char* hex, uint8_t* out, size_t cap, size_t* lenOut)
{
  if (hex == NULL || lenOut == NULL) return false;
  size_t chars = strlen(hex);
  if ((chars & 1U) != 0) return false;
  size_t len = chars / 2;
  if (len > cap || (len > 0 && out == NULL)) return false;
  for (size_t i = 0; i < len; i++) {
    int hi = Tool_HexNibble(hex[i * 2]);
    int lo = Tool_HexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  *lenOut = len;
  return true;
}

void Tool_HexEncode(const uint8_t* data, size_t len, char* out)
{
  static const char digits[] = "0123456789abcdef";
  if (out == NULL) return;
  if (data == NULL) len = 0;
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = digits[data[i] >> 4];
    out[i * 2 + 1] = digits[data[i] & 0x0f];
  }
  out[len * 2] = '\0';
}
