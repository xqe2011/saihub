#include "store.hpp"
#include "log.hpp"
#include "protocol.hpp"

#include <sqlite3.h>

#include <algorithm>

namespace saihub {

namespace {

std::string cache_key(std::string_view version, std::string_view filename) {
    std::string k;
    k.reserve(version.size() + 1 + filename.size());
    k.append(version);
    k.push_back('\0');
    k.append(filename);
    return k;
}

} // namespace

bool is_cached_filename(std::string_view value) {
    return value == "mcp.json" || value == "openapi.json";
}

bool is_cached_version(std::string_view value) {
    return !value.empty() && value.size() <= kMaxVersionLen && value.find('/') == std::string_view::npos;
}

MemoryStore::~MemoryStore() {
    if (stmt_insert_wl_) sqlite3_finalize(stmt_insert_wl_);
    if (stmt_delete_wl_) sqlite3_finalize(stmt_delete_wl_);
    if (stmt_put_fc_) sqlite3_finalize(stmt_put_fc_);
    if (stmt_delete_fc_) sqlite3_finalize(stmt_delete_fc_);
    if (db_) sqlite3_close(db_);
}

bool MemoryStore::init_db(std::string_view db_path) {
    if (db_path.empty()) {
        LOG_ERROR("[db] DB_PATH must be configured");
        return false;
    }
    std::string path(db_path);
    int rc = sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("[db] failed to open path=" << path << " err=\""
                  << (db_ ? sqlite3_errmsg(db_) : "unknown") << "\"");
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    char* errmsg = nullptr;
    const char* init_sql =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA busy_timeout=5000;"
        "CREATE TABLE IF NOT EXISTS device_whitelist ("
        "  digest TEXT PRIMARY KEY NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS file_cache ("
        "  version TEXT NOT NULL,"
        "  filename TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  PRIMARY KEY (version, filename)"
        ");";
    if (sqlite3_exec(db_, init_sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        LOG_ERROR("[db] schema init failed err=\"" << (errmsg ? errmsg : "unknown") << "\"");
        sqlite3_free(errmsg);
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // Load existing whitelist rows into memory
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT digest, created_at FROM device_whitelist", -1, &stmt, nullptr) != SQLITE_OK) {
            LOG_ERROR("[db] prepare whitelist select failed err=\"" << sqlite3_errmsg(db_) << "\"");
            return false;
        }
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const char* dg = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            int dg_len = sqlite3_column_bytes(stmt, 0);
            int64_t ts = sqlite3_column_int64(stmt, 1);
            if (dg && dg_len > 0) {
                whitelist_.try_emplace(std::string(dg, static_cast<size_t>(dg_len)), ts);
            }
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            LOG_ERROR("[db] load whitelist failed err=\"" << sqlite3_errmsg(db_) << "\"");
            return false;
        }
    }

    // Load existing file_cache rows into memory
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT version, filename, content FROM file_cache", -1, &stmt, nullptr) != SQLITE_OK) {
            LOG_ERROR("[db] prepare file_cache select failed err=\"" << sqlite3_errmsg(db_) << "\"");
            return false;
        }
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const char* ver = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            int ver_len = sqlite3_column_bytes(stmt, 0);
            const char* fn = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            int fn_len = sqlite3_column_bytes(stmt, 1);
            const char* ct = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            int ct_len = sqlite3_column_bytes(stmt, 2);
            if (ver && fn && ct) {
                file_cache_.emplace(
                    cache_key(std::string_view(ver, static_cast<size_t>(ver_len)),
                              std::string_view(fn, static_cast<size_t>(fn_len))),
                    std::string(ct, static_cast<size_t>(ct_len))
                );
            }
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            LOG_ERROR("[db] load file_cache failed err=\"" << sqlite3_errmsg(db_) << "\"");
            return false;
        }
    }

    if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO device_whitelist (digest, created_at) VALUES (?1, ?2)", -1, &stmt_insert_wl_, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, "DELETE FROM device_whitelist WHERE digest = ?1", -1, &stmt_delete_wl_, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO file_cache (version, filename, content) VALUES (?1, ?2, ?3)", -1, &stmt_put_fc_, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, "DELETE FROM file_cache WHERE version = ?1 AND filename = ?2", -1, &stmt_delete_fc_, nullptr) != SQLITE_OK) {
        LOG_ERROR("[db] prepare statements failed err=\"" << sqlite3_errmsg(db_) << "\"");
        return false;
    }

    LOG_INFO("[db] opened path=" << path << " whitelisted=" << whitelist_.size()
             << " cachedFiles=" << file_cache_.size());
    return true;
}

bool MemoryStore::is_whitelisted(std::string_view digest) const {
    return whitelist_.find(digest) != whitelist_.end();
}

std::pair<bool, int64_t> MemoryStore::insert_whitelist(std::string_view digest, int64_t now_ms) {
    auto it = whitelist_.find(digest);
    if (it != whitelist_.end()) {
        return {false, it->second};
    }
    if (db_ && stmt_insert_wl_) {
        sqlite3_reset(stmt_insert_wl_);
        sqlite3_clear_bindings(stmt_insert_wl_);
        sqlite3_bind_text(stmt_insert_wl_, 1, digest.data(), static_cast<int>(digest.size()), SQLITE_STATIC);
        sqlite3_bind_int64(stmt_insert_wl_, 2, now_ms);
        if (sqlite3_step(stmt_insert_wl_) != SQLITE_DONE) {
            LOG_ERROR("[db] insert_whitelist failed err=\"" << sqlite3_errmsg(db_) << "\"");
        }
        sqlite3_reset(stmt_insert_wl_);
    }
    auto [inserted_it, inserted] = whitelist_.try_emplace(std::string(digest), now_ms);
    return {inserted, inserted_it->second};
}

bool MemoryStore::delete_whitelist(std::string_view digest) {
    auto it = whitelist_.find(digest);
    if (it == whitelist_.end()) return false;
    if (db_ && stmt_delete_wl_) {
        sqlite3_reset(stmt_delete_wl_);
        sqlite3_clear_bindings(stmt_delete_wl_);
        sqlite3_bind_text(stmt_delete_wl_, 1, digest.data(), static_cast<int>(digest.size()), SQLITE_STATIC);
        if (sqlite3_step(stmt_delete_wl_) != SQLITE_DONE) {
            LOG_ERROR("[db] delete_whitelist failed err=\"" << sqlite3_errmsg(db_) << "\"");
        }
        sqlite3_reset(stmt_delete_wl_);
    }
    whitelist_.erase(it);
    return true;
}

WhitelistPage MemoryStore::list_whitelist(int page) const {
    WhitelistPage out;
    out.page = page;
    out.page_size = static_cast<int>(kWhitelistPageSize);
    out.total = whitelist_.size();

    std::vector<WhitelistRow> all;
    all.reserve(whitelist_.size());
    for (const auto& [d, ts] : whitelist_) {
        all.push_back(WhitelistRow{d, ts});
    }
    std::sort(all.begin(), all.end(), [](const WhitelistRow& a, const WhitelistRow& b) {
        if (a.created_at != b.created_at) return a.created_at > b.created_at;
        return a.digest < b.digest;
    });

    size_t offset = static_cast<size_t>(page - 1) * kWhitelistPageSize;
    if (offset < all.size()) {
        size_t end = std::min(all.size(), offset + kWhitelistPageSize);
        out.items.assign(std::make_move_iterator(all.begin() + offset), std::make_move_iterator(all.begin() + end));
    }
    return out;
}

std::optional<std::string_view> MemoryStore::get_cached_file(std::string_view version, std::string_view filename) const {
    auto it = file_cache_.find(cache_key(version, filename));
    if (it == file_cache_.end()) return std::nullopt;
    return std::string_view(it->second);
}

bool MemoryStore::put_cached_file(std::string_view version, std::string_view filename, std::string content) {
    if (db_ && stmt_put_fc_) {
        sqlite3_reset(stmt_put_fc_);
        sqlite3_clear_bindings(stmt_put_fc_);
        sqlite3_bind_text(stmt_put_fc_, 1, version.data(), static_cast<int>(version.size()), SQLITE_STATIC);
        sqlite3_bind_text(stmt_put_fc_, 2, filename.data(), static_cast<int>(filename.size()), SQLITE_STATIC);
        sqlite3_bind_text(stmt_put_fc_, 3, content.data(), static_cast<int>(content.size()), SQLITE_STATIC);
        if (sqlite3_step(stmt_put_fc_) != SQLITE_DONE) {
            LOG_ERROR("[db] put_cached_file failed err=\"" << sqlite3_errmsg(db_) << "\"");
        }
        sqlite3_reset(stmt_put_fc_);
    }
    auto key = cache_key(version, filename);
    auto it = file_cache_.find(key);
    if (it == file_cache_.end()) {
        file_cache_.emplace(std::move(key), std::move(content));
        return true;
    }
    it->second = std::move(content);
    return false;
}

bool MemoryStore::delete_cached_file(std::string_view version, std::string_view filename) {
    auto it = file_cache_.find(cache_key(version, filename));
    if (it == file_cache_.end()) return false;
    if (db_ && stmt_delete_fc_) {
        sqlite3_reset(stmt_delete_fc_);
        sqlite3_clear_bindings(stmt_delete_fc_);
        sqlite3_bind_text(stmt_delete_fc_, 1, version.data(), static_cast<int>(version.size()), SQLITE_STATIC);
        sqlite3_bind_text(stmt_delete_fc_, 2, filename.data(), static_cast<int>(filename.size()), SQLITE_STATIC);
        if (sqlite3_step(stmt_delete_fc_) != SQLITE_DONE) {
            LOG_ERROR("[db] delete_cached_file failed err=\"" << sqlite3_errmsg(db_) << "\"");
        }
        sqlite3_reset(stmt_delete_fc_);
    }
    file_cache_.erase(it);
    return true;
}

FileCachePage MemoryStore::list_cached_files(int page) const {
    FileCachePage out;
    out.page = page;
    out.page_size = static_cast<int>(kFileCachePageSize);
    out.total = file_cache_.size();

    std::vector<FileCacheRow> all;
    all.reserve(file_cache_.size());
    for (const auto& [k, content] : file_cache_) {
        size_t nul = k.find('\0');
        if (nul == std::string::npos) continue;
        all.push_back(FileCacheRow{k.substr(0, nul), k.substr(nul + 1), content.size()});
    }
    std::sort(all.begin(), all.end(), [](const FileCacheRow& a, const FileCacheRow& b) {
        if (a.version != b.version) return a.version > b.version;
        return a.filename < b.filename;
    });

    size_t offset = static_cast<size_t>(page - 1) * kFileCachePageSize;
    if (offset < all.size()) {
        size_t end = std::min(all.size(), offset + kFileCachePageSize);
        out.items.assign(std::make_move_iterator(all.begin() + offset), std::make_move_iterator(all.begin() + end));
    }
    return out;
}

} // namespace saihub
