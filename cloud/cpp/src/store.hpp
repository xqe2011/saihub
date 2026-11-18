#pragma once

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace saihub {

inline constexpr size_t kWhitelistPageSize = 50;
inline constexpr size_t kFileCachePageSize = 50;
inline constexpr size_t kMaxFileCacheBytes = 256 * 1024;

struct WhitelistRow {
    std::string digest;
    int64_t created_at = 0;
};

struct WhitelistPage {
    int page = 1;
    int page_size = static_cast<int>(kWhitelistPageSize);
    size_t total = 0;
    std::vector<WhitelistRow> items;
};

struct FileCacheRow {
    std::string version;
    std::string filename;
    size_t bytes = 0;
};

struct FileCachePage {
    int page = 1;
    int page_size = static_cast<int>(kFileCachePageSize);
    size_t total = 0;
    std::vector<FileCacheRow> items;
};

bool is_cached_filename(std::string_view value);
bool is_cached_version(std::string_view value);

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return ankerl::unordered_dense::hash<std::string_view>{}(sv);
    }
};

struct StringEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

// SQLite-backed store with an in-memory read cache for hot-path lookups.
class MemoryStore {
public:
    MemoryStore() = default;
    ~MemoryStore();

    MemoryStore(const MemoryStore&) = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    bool init_db(std::string_view db_path);

    bool is_whitelisted(std::string_view digest) const;
    std::pair<bool, int64_t> insert_whitelist(std::string_view digest, int64_t now_ms);
    bool delete_whitelist(std::string_view digest);
    WhitelistPage list_whitelist(int page) const;

    std::optional<std::string_view> get_cached_file(std::string_view version, std::string_view filename) const;
    bool put_cached_file(std::string_view version, std::string_view filename, std::string content);
    bool delete_cached_file(std::string_view version, std::string_view filename);
    FileCachePage list_cached_files(int page) const;

    size_t whitelist_count() const noexcept { return whitelist_.size(); }
    size_t file_cache_count() const noexcept { return file_cache_.size(); }

private:
    ankerl::unordered_dense::map<std::string, int64_t, StringHash, StringEq> whitelist_;
    // Key: version + '\0' + filename
    ankerl::unordered_dense::map<std::string, std::string, StringHash, StringEq> file_cache_;
    ::sqlite3* db_ = nullptr;
    ::sqlite3_stmt* stmt_insert_wl_ = nullptr;
    ::sqlite3_stmt* stmt_delete_wl_ = nullptr;
    ::sqlite3_stmt* stmt_put_fc_ = nullptr;
    ::sqlite3_stmt* stmt_delete_fc_ = nullptr;
};

} // namespace saihub
