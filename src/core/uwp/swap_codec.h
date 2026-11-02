// SPDX-License-Identifier: GPL-2.0-only
// Adapted from Test-project-arena-8ac58bdb-test-project, xbox/platform.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UWP_LZ_HASH_BITS 14
#define UWP_LZ_MIN_MATCH 4
#define UWP_LZ_LAST_LITERALS 8
#define UWP_LZ_MAX_OFFSET 65535
#define UWP_LZ_MIN_LENGTH 13

static uint32_t uwp_lz_read32(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t uwp_lz_hash(uint32_t sequence) {
    return (sequence * UINT32_C(2654435761)) >> (32 - UWP_LZ_HASH_BITS);
}

static size_t uwp_lz_compress(const unsigned char* source, size_t size, unsigned char* dest,
                              size_t capacity, void* scratch) {

    int32_t* table = (int32_t*)scratch;
    memset(table, 0xFF, sizeof(int32_t) << UWP_LZ_HASH_BITS);
    size_t ip = 0;
    size_t anchor = 0;
    size_t op = 0;
    if (size < UWP_LZ_MIN_LENGTH) {
        return 0;
    }
    const size_t limit = size - UWP_LZ_LAST_LITERALS;
    while (ip < limit) {
        const uint32_t sequence = uwp_lz_read32(source + ip);
        const uint32_t hash = uwp_lz_hash(sequence);
        const int32_t reference = table[hash];
        table[hash] = (int32_t)ip;
        if (reference < 0 || (size_t)(ip - (size_t)reference) > UWP_LZ_MAX_OFFSET ||
            uwp_lz_read32(source + (size_t)reference) != sequence) {
            ++ip;
            continue;
        }
        const size_t start = (size_t)reference;
        size_t match = UWP_LZ_MIN_MATCH;

        const size_t match_limit = size - UWP_LZ_LAST_LITERALS;
        while (ip + match < match_limit && source[start + match] == source[ip + match]) {
            ++match;
        }
        const size_t literals = ip - anchor;
        const size_t match_code = match - UWP_LZ_MIN_MATCH;

        size_t need = 1 + literals + 2;
        if (literals >= 15) {
            need += (literals - 15) / 255 + 1;
        }
        if (match_code >= 15) {
            need += (match_code - 15) / 255 + 1;
        }
        if (op + need > capacity) {
            return 0;
        }
        size_t token_at = op++;
        const size_t literal_header = literals >= 15 ? 15 : literals;
        unsigned char token = (unsigned char)(literal_header << 4);
        if (literals >= 15) {
            size_t rest = literals - 15;
            while (rest >= 255) {
                dest[op++] = 255;
                rest -= 255;
            }
            dest[op++] = (unsigned char)rest;
        }
        memcpy(dest + op, source + anchor, literals);
        op += literals;
        const size_t offset = ip - start;
        dest[op++] = (unsigned char)(offset & 0xFF);
        dest[op++] = (unsigned char)(offset >> 8);
        const size_t match_header = match_code >= 15 ? 15 : match_code;
        token |= (unsigned char)match_header;
        dest[token_at] = token;
        if (match_code >= 15) {
            size_t rest = match_code - 15;
            while (rest >= 255) {
                dest[op++] = 255;
                rest -= 255;
            }
            dest[op++] = (unsigned char)rest;
        }
        ip += match;
        anchor = ip;
        if (ip >= limit) {
            break;
        }

        if (ip >= 2) {
            table[uwp_lz_hash(uwp_lz_read32(source + ip - 2))] = (int32_t)(ip - 2);
        }
    }
    const size_t literals = size - anchor;
    size_t need = 1 + literals;
    if (literals >= 15) {
        need += (literals - 15) / 255 + 1;
    }
    if (op + need > capacity) {
        return 0;
    }
    const size_t token_at = op++;
    const size_t literal_header = literals >= 15 ? 15 : literals;
    dest[token_at] = (unsigned char)(literal_header << 4);
    if (literals >= 15) {
        size_t rest = literals - 15;
        while (rest >= 255) {
            dest[op++] = 255;
            rest -= 255;
        }
        dest[op++] = (unsigned char)rest;
    }
    memcpy(dest + op, source + anchor, literals);
    op += literals;
    return op;
}

static size_t uwp_swap_encode_page(const unsigned char* source, size_t size, unsigned char* dest,
                                   size_t capacity, void* scratch, int* raw) {
    size_t length = size;
    int stored_raw = 1;
    if (size <= UINT16_MAX && capacity >= size + 2) {
        const size_t packed = uwp_lz_compress(source, size, dest + 2, capacity - 2, scratch);
        if (packed != 0 && packed < size) {
            length = packed;
            stored_raw = 0;
        }
    }
    if (stored_raw)
        memcpy(dest + 2, source, size);
    dest[0] = (unsigned char)(length & 0xFF);
    dest[1] = (unsigned char)((length >> 8) & 0xFF);
    if (raw)
        *raw = stored_raw;
    return length + 2;
}

static size_t uwp_lz_decompress(const unsigned char* source, size_t size, unsigned char* dest,
                                size_t capacity) {
    size_t ip = 0;
    size_t op = 0;
    while (ip < size) {
        const unsigned char token = source[ip++];
        size_t literals = token >> 4;
        if (literals == 15) {
            unsigned char extra;
            do {
                if (ip >= size)
                    return 0;
                extra = source[ip++];
                literals += extra;
            } while (extra == 255);
        }
        if (literals > size - ip || literals > capacity - op)
            return 0;
        memcpy(dest + op, source + ip, literals);
        ip += literals;
        op += literals;
        if (ip == size) {
            break;
        }
        if (size - ip < 2)
            return 0;
        const size_t offset = (size_t)source[ip] | ((size_t)source[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > op)
            return 0;
        size_t match = (size_t)(token & 0x0F);
        if (match == 15) {
            unsigned char extra;
            do {
                if (ip >= size)
                    return 0;
                extra = source[ip++];
                match += extra;
            } while (extra == 255);
        }
        match += UWP_LZ_MIN_MATCH;
        if (match > capacity - op)
            return 0;
        const unsigned char* from = dest + op - offset;
        for (size_t i = 0; i < match; ++i) {
            dest[op + i] = from[i];
        }
        op += match;
    }
    return op == capacity ? op : 0;
}
