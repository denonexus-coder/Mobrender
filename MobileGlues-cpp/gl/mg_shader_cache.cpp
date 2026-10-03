// MobileGlues ESSL Shader Cache V3 — Implementation
// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only
//
// Architecture:
//   L0  = in-process unordered_map (zero-copy lookup after first use)
//   L1  = per-provider disk cache  (mmap read, atomic rename write)
//   Worker thread handles all disk I/O; render thread never blocks on writes.
//
// Cache stores ESSL ONLY — never GLSL.
//
// Program pair cache key = combine_hashes(hash(v_essl), hash(f_essl))
// The pair (vertex ESSL + fragment ESSL) is stored together per program entry.
//
// Per-shader ESSL map: shader_id → essl string
//   Populated by mg_shader_cache_save_essl (called from glShaderSource after conversion).
//   Consumed by mg_cache_lookup_program / mg_cache_save_program via mg_cache_get_essl.
//
// File layout: <cache_dir>/<provider>/<XX>/<XXXXXXXXXXXXXXXX>.essl
//   where XX = first 2 hex chars of key (reduces dentries per dir)
//
// Binary format:
//   [MGShaderCacheHeader][vertex ESSL bytes][fragment ESSL bytes]

// Required by log.h macros (DEBUG controls LOG_D/LOG_W/LOG_E verbosity)
#define DEBUG 0

#include "mg_shader_cache.h"
#include "../config/config.h"   // mg_directory_path
#include "../config/settings.h"
#include "mg.h"                 // write_log, write_log_n
#include "log.h"


#include <xxhash64.h>

#include <unordered_map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <vector>
#include <string>
#include <cstring>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// ──────────────────────────────────────────────────────────────
// Binary cache header — program pair (vertex ESSL + fragment ESSL)
// ──────────────────────────────────────────────────────────────

#define MG_CACHE_MAGIC   0x4D475633u   // 'MGV3'
#define MG_CACHE_VERSION 3u

#pragma pack(push, 1)
struct MGShaderCacheHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;   // sizeof(MGShaderCacheHeader) — forward-compat guard
    uint32_t flags;         // reserved, must be 0
    uint64_t cache_key;     // combined program key = combine_hashes(v_essl_hash, f_essl_hash)
    uint64_t v_hash;        // XXHash64 of vertex ESSL
    uint64_t f_hash;        // XXHash64 of fragment ESSL
    uint32_t v_size;        // byte length of vertex ESSL payload
    uint32_t f_size;        // byte length of fragment ESSL payload
};
#pragma pack(pop)

// ──────────────────────────────────────────────────────────────
// Internal state
// ──────────────────────────────────────────────────────────────

// Per-program state enum (for per-key deduplication)
enum class TaskState { NOT_STARTED, IN_PROGRESS, READY, WRITE_PENDING, FAILED };

struct CachedProgram {
    std::string v_essl;
    std::string f_essl;
};

struct CacheTask {
    std::string filepath;
    std::string v_essl;
    std::string f_essl;
    MGShaderCacheHeader header;
};

// L0: in-process hot cache  (key -> CachedProgram)
static std::unordered_map<uint64_t, CachedProgram>  g_L0_cache;
static std::unordered_map<uint64_t, TaskState>       g_task_state;
// Per-key mutex to prevent double-conversion of the same program
static std::unordered_map<uint64_t, std::mutex*>     g_key_locks;
static std::mutex                                     g_cache_mutex;

// Worker (async disk write) state
static std::queue<CacheTask>      g_write_queue;
static std::mutex                 g_queue_mutex;
static std::condition_variable    g_queue_cv;
static std::atomic<bool>          g_worker_stop{false};
static std::thread                g_worker_thread;
static std::string                g_cache_dir;

// Program -> attached shaders mapping (populated by mg_cache_attach / glAttachShader)
static std::unordered_map<GLuint, std::vector<GLuint>> g_program_shaders;
static std::mutex g_state_mutex;

// ── Per-shader individual ESSL storage ──────────────────────────────────────
// g_shader_essl_L0     : essl_hash → essl string  (L0 in-process, never cleared)
// g_shader_essl_result : shader_id → essl string  (for glLinkProgram pair save)
// g_shader_type        : shader_id → GL_VERTEX_SHADER / GL_FRAGMENT_SHADER
//
// Populated by mg_shader_cache_save_essl after GLSL→ESSL conversion in glShaderSource.
// Protected by g_shader_essl_mutex.
static std::unordered_map<uint64_t, std::string> g_shader_essl_L0;
static std::unordered_map<GLuint,   std::string> g_shader_essl_result;
static std::unordered_map<GLuint,   GLenum>      g_shader_type;
static std::mutex                                g_shader_essl_mutex;

// Per-shader disk format header
#define MG_SESSL_MAGIC   0x4D475333u   // 'MGS3'
#define MG_SESSL_VERSION 2u

#pragma pack(push, 1)
struct MGShaderESSLHeader {
    uint32_t magic;        // MG_SESSL_MAGIC
    uint32_t version;      // MG_SESSL_VERSION
    uint32_t header_size;  // sizeof(MGShaderESSLHeader)
    uint32_t flags;        // reserved, must be 0
    uint64_t essl_hash;    // XXHash64 of the ESSL source (NOT GLSL)
    uint32_t essl_size;    // byte length of ESSL payload
};
#pragma pack(pop)

// Per-shader disk-write task (enqueued to same worker thread)
struct ShaderCacheTask {
    std::string filepath;
    std::string essl;
    MGShaderESSLHeader header;
};
static std::queue<ShaderCacheTask> g_shader_write_queue; // also protected by g_queue_mutex
// ── End per-shader ESSL state ────────────────────────────────────────────────

// ──────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────

// Identifies the shader provider from ESSL source.
// "minecraft" is the default/fallback — NEVER "unknown".
static std::string get_provider(const std::string& essl) {
    if (essl.find("IRIS_") != std::string::npos ||
        essl.find("iris:")  != std::string::npos)  return "iris";
    if (essl.find("SODIUM_") != std::string::npos ||
        essl.find("FLW_")    != std::string::npos)  return "sodium";
    // Explicit Minecraft markers
    if (essl.find("moj_import")  != std::string::npos ||
        essl.find("FOG_")        != std::string::npos ||
        essl.find("minecraft/")  != std::string::npos) return "minecraft";
    return "minecraft";  // safe default
}

static uint64_t hash_str(const std::string& s) {
    return XXHash64::hash(s.data(), s.size(), 0ULL);
}

// Key for the program pair: combine_hashes(hash(v_essl), hash(f_essl))
// Both ESSL sources determine the key — no GLSL involved.
static uint64_t combine_hashes(uint64_t v_hash, uint64_t f_hash) {
    return XXHash64::hash(&v_hash, sizeof(v_hash), f_hash);
}

static void ensure_dir(const std::string& dir) {
    // Idempotent; ignore EEXIST
    mkdir(dir.c_str(), 0755);
}

// Build the filesystem path for a given key + provider
static std::string build_filepath(const std::string& provider, uint64_t key) {
    char hex[20];
    snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)key);
    std::string subdir  = g_cache_dir + "/" + provider;
    std::string subdir2 = subdir + "/" + std::string(hex, 2);
    ensure_dir(subdir);
    ensure_dir(subdir2);
    return subdir2 + "/" + hex + ".essl";
}

// Validate header fields (all hashes are ESSL-based)
static bool header_valid(const MGShaderCacheHeader* h, uint64_t key,
                         uint64_t v_hash, uint64_t f_hash,
                         size_t total_file_size) {
    if (h->magic       != MG_CACHE_MAGIC)           return false;
    if (h->version     != MG_CACHE_VERSION)         return false;
    if (h->header_size != sizeof(MGShaderCacheHeader)) return false;
    if (h->cache_key   != key)                      return false;
    if (h->v_hash      != v_hash)                   return false;
    if (h->f_hash      != f_hash)                   return false;
    // Guard against truncation / corruption
    size_t expected = sizeof(MGShaderCacheHeader) + h->v_size + h->f_size;
    if (expected != total_file_size)                return false;
    return true;
}

// Build path for a per-shader ESSL file:  <cache_dir>/shaders/<XX>/<XXXXXXXXXXXXXXXX>.sessl
static std::string build_shader_filepath(uint64_t essl_hash) {
    char hex[20];
    snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)essl_hash);
    std::string subdir  = g_cache_dir + "/shaders";
    std::string subdir2 = subdir + "/" + std::string(hex, 2);
    ensure_dir(subdir);
    ensure_dir(subdir2);
    return subdir2 + "/" + hex + ".sessl";
}

// Validate per-shader header (hash is ESSL-based)
static bool shader_header_valid(const MGShaderESSLHeader* h, uint64_t essl_hash,
                                 size_t total_file_size) {
    if (h->magic       != MG_SESSL_MAGIC)              return false;
    if (h->version     != MG_SESSL_VERSION)            return false;
    if (h->header_size != sizeof(MGShaderESSLHeader))  return false;
    if (h->essl_hash   != essl_hash)                   return false;
    size_t expected = sizeof(MGShaderESSLHeader) + h->essl_size;
    return expected == total_file_size;
}

// ──────────────────────────────────────────────────────────────
// Worker thread (async disk writes — pair + per-shader)
// ──────────────────────────────────────────────────────────────

static void worker_thread_func() {
    while (true) {
        // Drain pair tasks
        {
            CacheTask task;
            bool has_task = false;
            {
                std::unique_lock<std::mutex> lock(g_queue_mutex);
                g_queue_cv.wait(lock, [] {
                    return g_worker_stop.load()
                        || !g_write_queue.empty()
                        || !g_shader_write_queue.empty();
                });
                if (g_worker_stop && g_write_queue.empty() && g_shader_write_queue.empty()) break;

                if (!g_write_queue.empty()) {
                    task     = std::move(g_write_queue.front());
                    g_write_queue.pop();
                    has_task = true;
                }
            }
            if (has_task) {
                std::string tmp_path = task.filepath + ".tmp";
                int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    LOG_E("[MGSC] Worker: open(%s) failed: %s", tmp_path.c_str(), strerror(errno));
                } else {
                    bool ok = true;
                    ok = ok && (write(fd, &task.header,       sizeof(task.header))  == (ssize_t)sizeof(task.header));
                    ok = ok && (write(fd, task.v_essl.data(), task.v_essl.size())   == (ssize_t)task.v_essl.size());
                    ok = ok && (write(fd, task.f_essl.data(), task.f_essl.size())   == (ssize_t)task.f_essl.size());
                    close(fd);
                    if (!ok) {
                        LOG_E("[MGSC] Worker: write error for %s", tmp_path.c_str());
                        unlink(tmp_path.c_str());
                    } else if (rename(tmp_path.c_str(), task.filepath.c_str()) != 0) {
                        LOG_E("[MGSC] Worker: rename failed: %s", strerror(errno));
                        unlink(tmp_path.c_str());
                    } else {
                        LOG_D("[MGSC] Worker: saved pair %s", task.filepath.c_str());
                    }
                }
            }
        }

        // Drain per-shader tasks (same iteration, no extra wait)
        {
            ShaderCacheTask stask;
            bool has_stask = false;
            {
                std::lock_guard<std::mutex> lock(g_queue_mutex);
                if (!g_shader_write_queue.empty()) {
                    stask     = std::move(g_shader_write_queue.front());
                    g_shader_write_queue.pop();
                    has_stask = true;
                }
            }
            if (has_stask) {
                std::string tmp_path = stask.filepath + ".tmp";
                int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    LOG_E("[MGSC] Worker: open shader(%s) failed: %s", tmp_path.c_str(), strerror(errno));
                } else {
                    bool ok = true;
                    ok = ok && (write(fd, &stask.header,      sizeof(stask.header)) == (ssize_t)sizeof(stask.header));
                    ok = ok && (write(fd, stask.essl.data(),  stask.essl.size())    == (ssize_t)stask.essl.size());
                    close(fd);
                    if (!ok) {
                        LOG_E("[MGSC] Worker: write error for shader %s", tmp_path.c_str());
                        unlink(tmp_path.c_str());
                    } else if (rename(tmp_path.c_str(), stask.filepath.c_str()) != 0) {
                        LOG_E("[MGSC] Worker: rename failed shader: %s", strerror(errno));
                        unlink(tmp_path.c_str());
                    } else {
                        LOG_D("[MGSC] Worker: saved shader %s", stask.filepath.c_str());
                    }
                }
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────
// Public API — Lifecycle
// ──────────────────────────────────────────────────────────────

void mg_init_shader_cache() {
    // Use the same base directory as the rest of MobileGlues config
    g_cache_dir = std::string(mg_directory_path) + "/mg_essl_cache_v3";
    ensure_dir(g_cache_dir);

    g_worker_stop = false;
    g_worker_thread = std::thread(worker_thread_func);
    LOG_D("[MGSC] Initialized. Cache dir: %s", g_cache_dir.c_str());
}

void mg_destroy_shader_cache() {
    g_worker_stop = true;
    g_queue_cv.notify_all();
    if (g_worker_thread.joinable()) {
        g_worker_thread.join();
    }
}

// ──────────────────────────────────────────────────────────────
// Public API — Per-shader ESSL storage
// ──────────────────────────────────────────────────────────────
//
// Called from glShaderSource after GLSL→ESSL conversion.
// Stores the ESSL string keyed by:
//   - essl_hash (for L0 disk dedup)
//   - shader GLuint (for glLinkProgram pair assembly)
//   - shader type (GL_VERTEX_SHADER / GL_FRAGMENT_SHADER)
//
// The key is hash(ESSL) — never hash(GLSL).

__attribute__((noinline))
void mg_shader_cache_save_essl(GLuint shader, GLenum shader_type, const std::string& essl) {
    if (essl.empty()) return;

    const uint64_t h = hash_str(essl);  // key = hash(ESSL), not hash(GLSL)

    // Update L0 + per-shader result immediately (render thread benefits instantly)
    {
        std::lock_guard<std::mutex> lock(g_shader_essl_mutex);
        g_shader_essl_L0[h]          = essl;
        g_shader_essl_result[shader] = essl;
        g_shader_type[shader]        = shader_type;
    }

    // Build and enqueue async disk write
    ShaderCacheTask task;
    task.filepath           = build_shader_filepath(h);
    task.essl               = essl;
    task.header.magic       = MG_SESSL_MAGIC;
    task.header.version     = MG_SESSL_VERSION;
    task.header.header_size = sizeof(MGShaderESSLHeader);
    task.header.flags       = 0;
    task.header.essl_hash   = h;   // hash of ESSL
    task.header.essl_size   = static_cast<uint32_t>(essl.size());

    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_shader_write_queue.push(std::move(task));
    }
    g_queue_cv.notify_one();
    LOG_D("[MGSC-S] WRITE_QUEUED shader=%u type=%s hash=%016llX",
          shader, (shader_type == 0x8B31 ? "VERT" : "FRAG"), (unsigned long long)h);
}

// Returns the ESSL stored by mg_shader_cache_save_essl for a given shader ID.
// Called from glLinkProgram MISS path to build the V+F pair for mg_cache_save_program.
std::string mg_cache_get_essl(GLuint shader) {
    std::lock_guard<std::mutex> lock(g_shader_essl_mutex);
    auto it = g_shader_essl_result.find(shader);
    if (it != g_shader_essl_result.end()) return it->second;
    return "";
}

// ──────────────────────────────────────────────────────────────
// Public API — Program pair attachment tracking
// ──────────────────────────────────────────────────────────────

void mg_cache_attach(GLuint program, GLuint shader) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    // Avoid duplicates if glAttachShader called multiple times
    auto& vec = g_program_shaders[program];
    for (GLuint s : vec) {
        if (s == shader) return;
    }
    vec.push_back(shader);
}

// ──────────────────────────────────────────────────────────────
// Public API — Program pair cache lookup
// ──────────────────────────────────────────────────────────────
//
// Called at the START of glLinkProgram.
// Identifies the vertex+fragment shaders attached to the program,
// retrieves their ESSL from g_shader_essl_result, builds a combined
// key = combine_hashes(hash(v_essl), hash(f_essl)), then:
//   HIT  → populates out_v_essl / out_f_essl, returns true.
//   MISS → returns false; caller uses shaders' existing ESSL and calls
//           mg_cache_save_program to persist the pair.
//
// The key is derived from ESSL content — never from GLSL.

bool mg_cache_lookup_program(GLuint program, std::string& out_v_essl, std::string& out_f_essl) {
    // 1. Collect ESSL for this program's attached vertex+fragment shaders
    std::string v_essl, f_essl;
    GLuint v_shader_id = 0, f_shader_id = 0;
    {
        // First get the shader list under state_mutex
        std::vector<GLuint> attached;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            auto it = g_program_shaders.find(program);
            if (it == g_program_shaders.end()) return false;
            attached = it->second;
        }
        // Then get the ESSL under essl_mutex
        {
            std::lock_guard<std::mutex> lock(g_shader_essl_mutex);
            for (GLuint sh : attached) {
                auto tit = g_shader_type.find(sh);
                if (tit == g_shader_type.end()) continue;
                auto eit = g_shader_essl_result.find(sh);
                if (eit == g_shader_essl_result.end()) continue;
                if (tit->second == GL_VERTEX_SHADER) {
                    v_essl     = eit->second;
                    v_shader_id = sh;
                }
                if (tit->second == GL_FRAGMENT_SHADER) {
                    f_essl     = eit->second;
                    f_shader_id = sh;
                }
            }
        }
    }
    (void)v_shader_id;
    (void)f_shader_id;

    // Programs with only vertex or only fragment shader skip pair cache
    // (compute shaders, geometry, etc.)
    if (v_essl.empty() || f_essl.empty()) return false;

    // 2. Build cache key from ESSL content — not from GLSL
    uint64_t v_hash = hash_str(v_essl);
    uint64_t f_hash = hash_str(f_essl);
    uint64_t key    = combine_hashes(v_hash, f_hash);

    // 3. Determine provider (from ESSL content)
    std::string prov_f   = get_provider(f_essl);
    std::string prov_v   = get_provider(v_essl);
    std::string provider = (prov_f != "minecraft") ? prov_f : prov_v;

    // 4. L0 check (fast path, no I/O)
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_L0_cache.find(key);
        if (it != g_L0_cache.end()) {
            out_v_essl = it->second.v_essl;
            out_f_essl = it->second.f_essl;
            LOG_D("[MGSC] L0 HIT prog=%u provider=%s", program, provider.c_str());
            return true;
        }
        // Ensure per-key mutex exists
        if (g_key_locks.find(key) == g_key_locks.end()) {
            g_key_locks[key] = new std::mutex();
        }
    }

    // 5. Per-key lock: prevents two threads from converting the SAME program
    //    simultaneously, while DIFFERENT keys (programs) run in parallel.
    std::mutex* key_lock = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        key_lock = g_key_locks[key];
    }

    std::lock_guard<std::mutex> kl(*key_lock);

    // 6. L0 re-check after acquiring key lock (another thread may have finished)
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_L0_cache.find(key);
        if (it != g_L0_cache.end()) {
            out_v_essl = it->second.v_essl;
            out_f_essl = it->second.f_essl;
            LOG_D("[MGSC] L0 HIT (recheck) prog=%u", program);
            return true;
        }
        g_task_state[key] = TaskState::IN_PROGRESS;
    }

    // 7. L1 disk check (mmap, validated header)
    std::string filepath = build_filepath(provider, key);
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd >= 0) {
        struct stat st{};
        fstat(fd, &st);
        if (st.st_size > (off_t)sizeof(MGShaderCacheHeader)) {
            void* mapped = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (mapped != MAP_FAILED) {
                const auto* h = static_cast<const MGShaderCacheHeader*>(mapped);
                if (header_valid(h, key, v_hash, f_hash, (size_t)st.st_size)) {
                    const char* payload = (const char*)mapped + sizeof(MGShaderCacheHeader);
                    out_v_essl = std::string(payload,             h->v_size);
                    out_f_essl = std::string(payload + h->v_size, h->f_size);
                    munmap(mapped, (size_t)st.st_size);
                    close(fd);
                    {
                        std::lock_guard<std::mutex> lock(g_cache_mutex);
                        g_L0_cache[key]   = {out_v_essl, out_f_essl};
                        g_task_state[key] = TaskState::READY;
                    }
                    LOG_D("[MGSC] DISK HIT prog=%u provider=%s", program, provider.c_str());
                    return true;
                }
                // Header invalid / corrupted — discard silently, fallback to MISS
                LOG_W("[MGSC] DISK: invalid/corrupted entry for prog=%u, discarding", program);
                munmap(mapped, (size_t)st.st_size);
            }
        }
        close(fd);
        // Remove corrupt file so it won't be reloaded
        unlink(filepath.c_str());
    }

    // 8. MISS — caller uses ESSL already produced by glShaderSource
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_task_state[key] = TaskState::NOT_STARTED;
    }
    LOG_D("[MGSC] MISS prog=%u provider=%s key=%016llX", program, provider.c_str(),
          (unsigned long long)key);
    return false;
}

// ──────────────────────────────────────────────────────────────
// Public API — Program pair cache save
// ──────────────────────────────────────────────────────────────
//
// Called after a MISS, once both ESSL sources are known.
// key = combine_hashes(hash(v_essl), hash(f_essl)) — derived from ESSL, never GLSL.
// Updates L0 immediately and enqueues async disk write (atomic rename).

void mg_cache_save_program(GLuint program, const std::string& v_essl, const std::string& f_essl) {
    if (v_essl.empty() || f_essl.empty()) return;

    // Key derived from ESSL content — matches what mg_cache_lookup_program computes
    uint64_t v_hash = hash_str(v_essl);
    uint64_t f_hash = hash_str(f_essl);
    uint64_t key    = combine_hashes(v_hash, f_hash);

    std::string prov_f   = get_provider(f_essl);
    std::string prov_v   = get_provider(v_essl);
    std::string provider = (prov_f != "minecraft") ? prov_f : prov_v;

    // Update L0 immediately (render thread benefits on next frame)
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_L0_cache[key]   = {v_essl, f_essl};
        g_task_state[key] = TaskState::WRITE_PENDING;
    }

    // Build task and enqueue for async worker (no blocking I/O on render thread)
    CacheTask task;
    task.filepath            = build_filepath(provider, key);
    task.v_essl              = v_essl;
    task.f_essl              = f_essl;
    task.header.magic        = MG_CACHE_MAGIC;
    task.header.version      = MG_CACHE_VERSION;
    task.header.header_size  = sizeof(MGShaderCacheHeader);
    task.header.flags        = 0;
    task.header.cache_key    = key;
    task.header.v_hash       = v_hash;
    task.header.f_hash       = f_hash;
    task.header.v_size       = static_cast<uint32_t>(v_essl.size());
    task.header.f_size       = static_cast<uint32_t>(f_essl.size());

    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_write_queue.push(std::move(task));
    }
    g_queue_cv.notify_one();
    LOG_D("[MGSC] WRITE_QUEUED prog=%u provider=%s", program, provider.c_str());
}

// ──────────────────────────────────────────────────────────────
// Public API — Per-shader ESSL disk lookup (by ESSL hash)
// ──────────────────────────────────────────────────────────────
//
// Called at the START of glCompileShader (optional fast path):
// if the per-shader ESSL is already in L0 or disk, returns it without
// requiring re-conversion. Returns false on MISS.
//
// __attribute__((noinline)) prevents dead-code-elimination of disk branch.

__attribute__((noinline))
bool mg_shader_cache_lookup_essl_by_hash(uint64_t essl_hash, std::string& out_essl) {
    const uint64_t h = essl_hash;

    // ── L0: in-process map ──────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(g_shader_essl_mutex);
        auto it = g_shader_essl_L0.find(h);
        if (__builtin_expect(it != g_shader_essl_L0.end(), 1)) {
            out_essl = it->second;
            LOG_D("[MGSC-S] L0 HIT hash=%016llX", (unsigned long long)h);
            return true;
        }
    }

    // ── L1: disk (mmap) ─────────────────────────────────────────────────────
    std::string filepath = build_shader_filepath(h);
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd >= 0) {
        struct stat st{};
        fstat(fd, &st);
        if (st.st_size > (off_t)sizeof(MGShaderESSLHeader)) {
            void* mapped = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (mapped != MAP_FAILED) {
                const auto* hdr = static_cast<const MGShaderESSLHeader*>(mapped);
                if (shader_header_valid(hdr, essl_hash, (size_t)st.st_size)) {
                    // Valid: copy ESSL payload and populate L0
                    const char* payload = (const char*)mapped + sizeof(MGShaderESSLHeader);
                    out_essl = std::string(payload, hdr->essl_size);
                    munmap(mapped, (size_t)st.st_size);
                    close(fd);
                    {
                        std::lock_guard<std::mutex> lock(g_shader_essl_mutex);
                        g_shader_essl_L0[h] = out_essl;
                    }
                    LOG_D("[MGSC-S] DISK HIT hash=%016llX", (unsigned long long)h);
                    return true;
                }
                // Corrupt/stale: discard and let MISS path regenerate
                LOG_W("[MGSC-S] DISK: invalid entry for hash=%016llX, discarding", (unsigned long long)h);
                munmap(mapped, (size_t)st.st_size);
            }
        }
        close(fd);
        unlink(filepath.c_str());   // Remove stale file
    }

    // ── MISS ─────────────────────────────────────────────────────────────────
    LOG_D("[MGSC-S] MISS hash=%016llX", (unsigned long long)h);
    return false;
}
