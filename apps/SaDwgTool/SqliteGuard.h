#pragma once
// SQLite RAII 作用域守卫与严格错误检查封装（CAD-20）：
// - 声明顺序约定（利用 C++ 逆序析构保证 Windows 下句柄先关再删临时文件）：
//     1. ScopedTempFile        （最后析构：未提交释放则删除临时库及其伴生 journal/wal/shm）
//     2. ScopedSqliteDb        （先于临时文件析构：自动关闭数据库连接）
//     3. ScopedSqliteTransaction（先于连接析构：未提交则自动 ROLLBACK）
//     4. ScopedSqliteStmt      （最先析构：自动 finalize 语句）
// - 所有 prepare / bind_* / step / exec 均严格校验返回码，失败立即填充错误原因。
#include "Core/PathUtils.h"
#include "sqlite3.h"
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace MiniCAD::SaTool
{
    inline bool ExecSqliteChecked(sqlite3* db, const char* sql, std::string* error)
    {
        if (!db)
        {
            if (error) *error = "SQLite 连接为空";
            return false;
        }
        char* errMsg = nullptr;
        const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK)
        {
            if (error) *error = std::string("SQLite 执行失败：") + (errMsg ? errMsg : sqlite3_errmsg(db));
            if (errMsg) sqlite3_free(errMsg);
            return false;
        }
        return true;
    }

    class ScopedTempFile
    {
    public:
        ScopedTempFile() = default;
        explicit ScopedTempFile(const std::string& utf8Path)
            : m_utf8Path(utf8Path)
            , m_path(MiniCAD::Utf8ToPath(utf8Path))
            , m_armed(!utf8Path.empty())
        {
        }

        ScopedTempFile(const ScopedTempFile&) = delete;
        ScopedTempFile& operator=(const ScopedTempFile&) = delete;

        ~ScopedTempFile() { Cleanup(); }

        void Release() { m_armed = false; }

        void Cleanup()
        {
            if (!m_armed || m_path.empty())
                return;
            m_armed = false;
            std::error_code ec;
            std::filesystem::remove(m_path, ec);
            std::filesystem::remove(MiniCAD::Utf8ToPath(m_utf8Path + "-journal"), ec);
            std::filesystem::remove(MiniCAD::Utf8ToPath(m_utf8Path + "-wal"), ec);
            std::filesystem::remove(MiniCAD::Utf8ToPath(m_utf8Path + "-shm"), ec);
        }

        const std::string& Utf8Path() const { return m_utf8Path; }

    private:
        std::string           m_utf8Path;
        std::filesystem::path m_path;
        bool                  m_armed = false;
    };

    class ScopedSqliteDb
    {
    public:
        ScopedSqliteDb() = default;
        ScopedSqliteDb(const ScopedSqliteDb&) = delete;
        ScopedSqliteDb& operator=(const ScopedSqliteDb&) = delete;

        ~ScopedSqliteDb() { Close(nullptr); }

        bool Open(const std::string& utf8Path, int flags, std::string* error)
        {
            Close(nullptr);
            const int rc = sqlite3_open_v2(utf8Path.c_str(), &m_db, flags, nullptr);
            if (rc != SQLITE_OK || !m_db)
            {
                if (error)
                    *error = std::string("无法打开 SQLite 文件：") + (m_db ? sqlite3_errmsg(m_db) : utf8Path);
                if (m_db)
                {
                    sqlite3_close(m_db);
                    m_db = nullptr;
                }
                return false;
            }
            return true;
        }

        bool Close(std::string* error)
        {
            if (!m_db)
                return true;
            const int rc = sqlite3_close(m_db);
            if (rc != SQLITE_OK)
            {
                if (error) *error = std::string("关闭 SQLite 文件失败：") + sqlite3_errmsg(m_db);
                sqlite3_close_v2(m_db);
                m_db = nullptr;
                return false;
            }
            m_db = nullptr;
            return true;
        }

        sqlite3* Get() const { return m_db; }
        explicit operator bool() const { return m_db != nullptr; }

    private:
        sqlite3* m_db = nullptr;
    };

    class ScopedSqliteTransaction
    {
    public:
        explicit ScopedSqliteTransaction(sqlite3* db) : m_db(db) {}
        ScopedSqliteTransaction(const ScopedSqliteTransaction&) = delete;
        ScopedSqliteTransaction& operator=(const ScopedSqliteTransaction&) = delete;

        ~ScopedSqliteTransaction()
        {
            if (m_active && m_db)
            {
                char* ignored = nullptr;
                sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, &ignored);
                if (ignored) sqlite3_free(ignored);
                m_active = false;
            }
        }

        bool Begin(const char* beginSql, std::string* error)
        {
            if (!ExecSqliteChecked(m_db, beginSql, error))
                return false;
            m_active = true;
            return true;
        }

        bool Commit(std::string* error)
        {
            if (!m_active)
                return true;
            if (!ExecSqliteChecked(m_db, "COMMIT;", error))
                return false;
            m_active = false;
            return true;
        }

        bool Active() const { return m_active; }

    private:
        sqlite3* m_db     = nullptr;
        bool     m_active = false;
    };

    class ScopedSqliteStmt
    {
    public:
        ScopedSqliteStmt() = default;
        ScopedSqliteStmt(const ScopedSqliteStmt&) = delete;
        ScopedSqliteStmt& operator=(const ScopedSqliteStmt&) = delete;

        ~ScopedSqliteStmt() { Finalize(); }

        void Finalize()
        {
            if (m_stmt)
            {
                sqlite3_finalize(m_stmt);
                m_stmt = nullptr;
            }
            m_db = nullptr;
        }

        bool Prepare(sqlite3* db, const char* sql, std::string* error)
        {
            Finalize();
            if (!db)
            {
                if (error) *error = "SQLite prepare 失败：连接为空";
                return false;
            }
            if (sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr) != SQLITE_OK || !m_stmt)
            {
                if (error) *error = std::string("SQLite prepare 失败：") + sqlite3_errmsg(db);
                m_stmt = nullptr;
                return false;
            }
            m_db = db;
            return true;
        }

        bool BindNull(int idx, std::string* error)
        {
            return CheckBind(sqlite3_bind_null(m_stmt, idx), error);
        }

        bool BindInt(int idx, int v, std::string* error)
        {
            return CheckBind(sqlite3_bind_int(m_stmt, idx, v), error);
        }

        bool BindInt64(int idx, int64_t v, std::string* error)
        {
            return CheckBind(sqlite3_bind_int64(m_stmt, idx, static_cast<sqlite3_int64>(v)), error);
        }

        bool BindDouble(int idx, double v, std::string* error)
        {
            return CheckBind(sqlite3_bind_double(m_stmt, idx, v), error);
        }

        bool BindText(int idx, const std::string& s, std::string* error)
        {
            return CheckBind(
                sqlite3_bind_text(m_stmt, idx, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT),
                error);
        }

        bool BindTextStatic(int idx, const char* s, std::string* error)
        {
            return CheckBind(sqlite3_bind_text(m_stmt, idx, s, -1, SQLITE_STATIC), error);
        }

        bool BindOptText(int idx, const std::string& s, std::string* error)
        {
            return s.empty() ? BindNull(idx, error) : BindText(idx, s, error);
        }

        bool BindOptDouble(int idx, const std::optional<double>& v, std::string* error)
        {
            if (v.has_value() && std::isfinite(*v))
                return BindDouble(idx, *v, error);
            return BindNull(idx, error);
        }

        bool BindOptInt(int idx, const std::optional<int>& v, std::string* error)
        {
            if (v.has_value())
                return BindInt(idx, *v, error);
            return BindNull(idx, error);
        }

        bool StepDone(std::string* error)
        {
            const int rc = sqlite3_step(m_stmt);
            const std::string stepErr = (rc != SQLITE_DONE && m_db) ? sqlite3_errmsg(m_db) : std::string();
            const int resetRc = sqlite3_reset(m_stmt);
            const int clearRc = sqlite3_clear_bindings(m_stmt);
            if (rc != SQLITE_DONE)
            {
                if (error) *error = std::string("SQLite step 失败：") + (stepErr.empty() ? std::to_string(rc) : stepErr);
                return false;
            }
            if (resetRc != SQLITE_OK || clearRc != SQLITE_OK)
            {
                if (error) *error = std::string("SQLite reset/clear 失败：") + (m_db ? sqlite3_errmsg(m_db) : "unknown");
                return false;
            }
            return true;
        }

        sqlite3_stmt* Get() const { return m_stmt; }

    private:
        bool CheckBind(int rc, std::string* error) const
        {
            if (rc != SQLITE_OK)
            {
                if (error) *error = std::string("SQLite bind 失败：") + (m_db ? sqlite3_errmsg(m_db) : std::to_string(rc));
                return false;
            }
            return true;
        }

        sqlite3*      m_db   = nullptr;
        sqlite3_stmt* m_stmt = nullptr;
    };
}
