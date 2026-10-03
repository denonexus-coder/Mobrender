// MobileGlues - config/sodium_detect.h
// Sodium mod detection and handshake - validates render data origin
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

#include <cstdint>
#include <cstring>

// ============================================================================
// Sodium Handshake Magic Signature
// This is embedded in Sodium terrain batch data to prove it's from Sodium mod
// ============================================================================

#define SODIUM_HANDSHAKE_MAGIC 0x5F5F534F4449554D // '__SODIUM' in hex
#define SODIUM_HANDSHAKE_VERSION 1

// Struct that Sodium will embed in its batch headers
// This MUST match exactly what Sodium sends
struct SodiumHandshake {
    // Magic signature to identify Sodium data
    uint64_t magic = SODIUM_HANDSHAKE_MAGIC;
    
    // Version for future compatibility
    uint32_t version = SODIUM_HANDSHAKE_VERSION;
    
    // Flags indicating which Sodium-specific features are active
    struct {
        uint32_t useSharedIBO : 1;          // Shared IBO path available
        uint32_t terrainOptimized : 1;      // Terrain-optimized batching
        uint32_t multiDrawBaseVertexReady : 1; // Ready for multi-draw base vertex
        uint32_t reserved : 29;
    } features;
    
    // Checksum to validate integrity
    uint32_t checksum = 0;
    
    // Padding for alignment
    uint32_t reserved = 0;
    
    // Compute checksum for validation
    void computeChecksum() {
        checksum = 0;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(this);
        for (size_t i = 0; i < sizeof(SodiumHandshake); ++i) {
            if (i >= offsetof(SodiumHandshake, checksum) && 
                i < offsetof(SodiumHandshake, checksum) + sizeof(checksum)) {
                continue; // Skip checksum field itself
            }
            checksum = ((checksum << 5) + checksum) + bytes[i];
        }
    }
    
    // Validate checksum
    bool validateChecksum() const {
        SodiumHandshake temp = *this;
        temp.computeChecksum();
        return temp.checksum == checksum;
    }
    
    // Validate entire handshake
    bool isValid() const {
        return magic == SODIUM_HANDSHAKE_MAGIC && 
               version == SODIUM_HANDSHAKE_VERSION &&
               validateChecksum();
    }
};

static_assert(sizeof(SodiumHandshake) == 24, "SodiumHandshake must be exactly 24 bytes");

// ============================================================================
// Sodium Detection Result
// ============================================================================

struct SodiumDetectionResult {
    bool isSodium = false;           // True if Sodium handshake detected
    const SodiumHandshake* handshake = nullptr; // Pointer to valid handshake (if any)
    const char* reason = "";         // Why detection succeeded/failed
};

// ============================================================================
// Sodium Detector
// ============================================================================

class SodiumDetector {
public:
    // Detect Sodium from index data pointer
    static SodiumDetectionResult detectFromIndices(const void* const* indices, int count) {
        if (!indices || count <= 0) {
            return {false, nullptr, "Invalid indices array"};
        }
        
        // Look for handshake marker in the first index pointer area
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(indices[0]);
        
        // Check nearby memory for handshake
        // Sodium typically embeds it in a well-known offset
        const int searchRange = 256;
        for (int offset = 0; offset < searchRange; offset += sizeof(SodiumHandshake)) {
            const SodiumHandshake* candidate = reinterpret_cast<const SodiumHandshake*>(ptr - offset);
            if (candidate->isValid()) {
                return {true, candidate, "Valid Sodium handshake detected"};
            }
        }
        
        return {false, nullptr, "No valid Sodium handshake found"};
    }
    
    // Detect from batch metadata (if Sodium passes struct directly)
    static SodiumDetectionResult detectFromHandshake(const void* metadata) {
        if (!metadata) {
            return {false, nullptr, "Null metadata pointer"};
        }
        
        const SodiumHandshake* hs = reinterpret_cast<const SodiumHandshake*>(metadata);
        if (hs->isValid()) {
            return {true, hs, "Valid Sodium handshake from metadata"};
        }
        
        return {false, nullptr, "Invalid or corrupted handshake"};
    }
};
