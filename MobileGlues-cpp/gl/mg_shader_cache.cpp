// MobileGlues ESSL Shader Cache V2 — Implementation
// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only
//
// Architecture:
//   L0  = in-process unordered_map (zero-copy lookup after first use)
//   L1  = per-provider disk cache  (mmap read, atomic rename write)
//   Worker thread handles all disk I/O; render thread never blocks on writes.
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
// Binary cache header
// ──────────────────────────────────────────────────────────────

#define MG_CACHE_MAGIC   0x4D475632u   // 'MGV2'
#define MG_CACHE_VERSION 2u

#pragma pack(push, 1)
struct MGShaderCacheHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;   // sizeof(MGShaderCacheHeader) — forward-compat guard
    uint32_t flags;         // reserved, must be 0
    uint64_t cache_key;     // combined program key
    uint64_t v_hash;        // vertex GLSL hash
    uint64_t f_hash;        // fragment GLSL hash
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

// GLSL registration (populated by glShaderSource)
static std::unordered_map<GLuint, std::string> g_shader_glsl;
static std::unordered_map<GLuint, GLenum>      g_shader_type;
// Program -> attached shaders mapping (populated by glAttachShader)
static std::unordered_map<GLuint, std::vector<GLuint>> g_program_shaders;
static std::mutex g_state_mutex;

// ──────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────

// Identifies the shader provider from GLSL source.
// "minecraft" is the default/fallback — NEVER "unknown".
static std::string get_provider(const std::string& glsl) {
    if (glsl.find("IRIS_") != std::string::npos ||
        glsl.find("iris:")  != std::string::npos)  return "iris";
    if (glsl.find("SODIUM_") != std::string::npos ||
        glsl.find("FLW_")    != std::string::npos)  return "sodium";
    // Explicit Minecraft markers
    if (glsl.find("moj_import")  != std::string::npos ||
        glsl.find("FOG_")        != std::string::npos ||
        glsl.find("minecraft/")  != std::string::npos) return "minecraft";
    return "minecraft";  // safe default
}

static uint64_t hash_str(const std::string& s) {
    return XXHash64::hash(s.data(), s.size(), 0ULL);
}

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

// Validate header fields
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

// ──────────────────────────────────────────────────────────────
// Worker thread (async disk writes)
// ──────────────────────────────────────────────────────────────

static void worker_thread_func() {
    while (true) {
        CacheTask task;
        {
            std::unique_lock<std::mutex> lock(g_queue_mutex);
            g_queue_cv.wait(lock, [] {
                return g_worker_stop.load() || !g_write_queue.empty();
            });
            if (g_worker_stop && g_write_queue.empty()) break;
            task = std::move(g_write_queue.front());
            g_write_queue.pop();
        }

        // Atomic write: write to .tmp then rename
        std::string tmp_path = task.filepath + ".tmp";
        int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            LOG_E("[MGSC] Worker: open(%s) failed: %s", tmp_path.c_str(), strerror(errno));
            continue;
        }

        bool ok = true;
        ok = ok && (write(fd, &task.header,         sizeof(task.header))    == (ssize_t)sizeof(task.header));
        ok = ok && (write(fd, task.v_essl.data(),   task.v_essl.size())     == (ssize_t)task.v_essl.size());
        ok = ok && (write(fd, task.f_essl.data(),   task.f_essl.size())     == (ssize_t)task.f_essl.size());
        close(fd);

        if (!ok) {
            LOG_E("[MGSC] Worker: write error for %s", tmp_path.c_str());
            unlink(tmp_path.c_str());
            continue;
        }

        if (rename(tmp_path.c_str(), task.filepath.c_str()) != 0) {
            LOG_E("[MGSC] Worker: rename failed: %s", strerror(errno));
            unlink(tmp_path.c_str());
        } else {
            LOG_D("[MGSC] Worker: saved %s", task.filepath.c_str());
        }
    }
}

// ──────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────

void mg_init_shader_cache() {
    // Use the same base directory as the rest of MobileGlues config
    g_cache_dir = std::string(mg_directory_path) + "/mg_essl_cache_v2";
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

void mg_cache_register_glsl(GLuint shader, GLenum type, const std::string& glsl) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_shader_glsl[shader] = glsl;
    g_shader_type[shader] = type;
}

void mg_cache_attach(GLuint program, GLuint shader) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    // Avoid duplicates if glAttachShader called multiple times
    auto& vec = g_program_shaders[program];
    for (GLuint s : vec) {
        if (s == shader) return;
    }
    vec.push_back(shader);
}

bool mg_cache_lookup_program(GLuint program, std::string& out_v_essl, std::string& out_f_essl) {
    // 1. Collect GLSL for this program's attached shaders
    std::string v_glsl, f_glsl;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_program_shaders.find(program);
        if (it == g_program_shaders.end()) return false;
        for (GLuint sh : it->second) {
            auto tit = g_shader_type.find(sh);
            if (tit == g_shader_type.end()) continue;
            if (tit->second == GL_VERTEX_SHADER)   v_glsl = g_shader_glsl[sh];
            if (tit->second == GL_FRAGMENT_SHADER)  f_glsl = g_shader_glsl[sh];
        }
    }
    // Programs with only vertex or only fragment shader skip cache
    // (compute shaders, geometry, etc.)
    if (v_glsl.empty() || f_glsl.empty()) return false;

    // 2. Determine provider (individual identity — not program-global)
    //    Fragment shader provider takes precedence if different from vertex
    std::string prov_f = get_provider(f_glsl);
    std::string prov_v = get_provider(v_glsl);
    // Each STAGE can be from a different provider; use the fragment provider
    // as the key namespace since it usually carries the unique pass identity.
    // If they differ, use the non-minecraft one (external provider).
    std::string provider = (prov_f != "minecraft") ? prov_f : prov_v;

    uint64_t v_hash = hash_str(v_glsl);
    uint64_t f_hash = hash_str(f_glsl);
    uint64_t key    = combine_hashes(v_hash, f_hash);

    // 3. L0 check (fast path, no I/O)
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

    // 4. Per-key lock: prevents two threads from converting the SAME program
    //    simultaneously, while DIFFERENT keys (programs) run in parallel.
    std::mutex* key_lock = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        key_lock = g_key_locks[key];
    }

    std::lock_guard<std::mutex> kl(*key_lock);

    // 5. L0 re-check after acquiring key lock (another thread may have finished)
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

    // 6. L1 disk check (mmap, validated header)
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
                    out_v_essl = std::string(payload,                h->v_size);
                    out_f_essl = std::string(payload + h->v_size,    h->f_size);
                    munmap(mapped, (size_t)st.st_size);
                    close(fd);
                    {
                        std::lock_guard<std::mutex> lock(g_cache_mutex);
                        g_L0_cache[key]    = {out_v_essl, out_f_essl};
                        g_task_state[key]  = TaskState::READY;
                    }
                    LOG_D("[MGSC] DISK HIT prog=%u provider=%s", program, provider.c_str());
                    return true;
                }
                // Header invalid / corrupted — discard silently, fallback
                LOG_W("[MGSC] DISK: invalid/corrupted entry for prog=%u, discarding", program);
                munmap(mapped, (size_t)st.st_size);
            }
        }
        close(fd);
        // Remove corrupt file so it won't be reloaded
        unlink(filepath.c_str());
    }

    // 7. MISS — caller will do GLSL->SPIR-V->SPIRV-Cross then call mg_cache_save_program
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_task_state[key] = TaskState::NOT_STARTED;
    }
    LOG_D("[MGSC] MISS prog=%u provider=%s key=%016llX", program, provider.c_str(),
          (unsigned long long)key);
    return false;
}

void mg_cache_save_program(GLuint program, const std::string& v_essl, const std::string& f_essl) {
    // Reconstruct key from stored GLSL (must match what lookup used)
    std::string v_glsl, f_glsl;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_program_shaders.find(program);
        if (it == g_program_shaders.end()) return;
        for (GLuint sh : it->second) {
            auto tit = g_shader_type.find(sh);
            if (tit == g_shader_type.end()) continue;
            if (tit->second == GL_VERTEX_SHADER)   v_glsl = g_shader_glsl[sh];
            if (tit->second == GL_FRAGMENT_SHADER)  f_glsl = g_shader_glsl[sh];
        }
    }
    if (v_glsl.empty() || f_glsl.empty()) return;

    std::string prov_f = get_provider(f_glsl);
    std::string prov_v = get_provider(v_glsl);
    std::string provider = (prov_f != "minecraft") ? prov_f : prov_v;

    uint64_t v_hash = hash_str(v_glsl);
    uint64_t f_hash = hash_str(f_glsl);
    uint64_t key    = combine_hashes(v_hash, f_hash);

    // Update L0 immediately (render thread benefits on next frame)
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_L0_cache[key]   = {v_essl, f_essl};
        g_task_state[key] = TaskState::WRITE_PENDING;
    }

    // Build task and enqueue for async worker (no blocking I/O on render thread)
    CacheTask task;
    task.filepath         = build_filepath(provider, key);
    task.v_essl           = v_essl;
    task.f_essl           = f_essl;
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

std::string mg_cache_get_glsl(GLuint shader) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto it = g_shader_glsl.find(shader);
    if (it != g_shader_glsl.end()) return it->second;
    return "";
}
