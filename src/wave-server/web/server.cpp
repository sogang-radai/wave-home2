#include "server.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

#include <drogon/drogon.h>
#include <drogon/orm/DbConfig.h>

#undef LOG_TRACE
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#undef LOG_FATAL

#include "../core/coredefs.h"
#include "../core/logger.h"
#include "../app/app_state.h"
#include "../db/database.h"
#include "demo_policy.h"
#include "util/exe_path.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

namespace
{
    std::filesystem::path resolvePath(
        const std::filesystem::path& base_dir,
        const std::string& path)
    {
        std::filesystem::path resolved(path);
        if (resolved.is_absolute())
            return resolved;

        if (!base_dir.empty())
            return base_dir / resolved;

        return std::filesystem::weakly_canonical(
            std::filesystem::current_path() / resolved);
    }

    bool ensureDir(const std::filesystem::path& dir_path)
    {
        if (dir_path.empty())
            return true;

        std::error_code ec;
        std::filesystem::create_directories(dir_path, ec);
        if (ec)
        {
            LOG_ERROR("Failed to create directory {}: {}", dir_path.string(), ec.message());
            return false;
        }

        return true;
    }

    bool ensureParentDir(const std::filesystem::path& file_path)
    {
        const auto parent = file_path.parent_path();
        if (parent.empty())
            return true;

        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            LOG_ERROR("Failed to create directory {}: {}", parent.string(), ec.message());
            return false;
        }

        return true;
    }

    void configureStaticFileTypes(drogon::HttpAppFramework& app)
    {
        // Drogon defaults omit .json; CRA ships manifest.json and asset-manifest.json.
        app.registerCustomExtensionMime("json", "application/json");
        app.registerCustomExtensionMime("map", "application/json");
        app.registerCustomExtensionMime("gltf", "model/gltf+json");
        app.registerCustomExtensionMime("bin", "application/octet-stream");
        app.setFileTypes({
            "html",
            "js",
            "css",
            "xml",
            "xsl",
            "txt",
            "svg",
            "ttf",
            "otf",
            "woff2",
            "woff",
            "eot",
            "png",
            "jpg",
            "jpeg",
            "gif",
            "bmp",
            "ico",
            "icns",
            "webp",
            "json",
            "map",
            "gltf",
            "bin",
        });
    }
}

struct Server::Impl
{
    std::thread thread;
    std::atomic<bool> running{false};
    uint16_t port = 0;
    std::string documentRoot;
};

Server::Server() :
    m_impl(std::make_unique<Impl>())
{
}

Server::~Server() = default;

bool Server::init(const json& config, bool test_mode, bool demo_mode)
{
    if (m_impl->running.load(std::memory_order_acquire))
    {
        LOG_WARN("Web server is already running");
        return false;
    }

    const auto base_dir = getExecutableDir();
    if (base_dir.empty())
        LOG_WARN("Executable directory is unknown; using relative paths from cwd");

    const uint16_t port = config.value("port", 8502);
    const size_t thread_num = config.value("threads_num", 2);
    const std::string document_root = config.value(
        "document_root",
        test_mode ? "../site-test" : (demo_mode ? "../site-demo" : "../site"));
    const std::string home_page = config.value("home_page", "index.html");
    const bool skip_migrations = config.value("skip_migrations", false);
    const bool read_only = config.value("read_only", false);

    const std::string database_path = config.value("database_path", "data/database.db");

    const auto resolved_document_root = [&]() -> std::filesystem::path
    {
        auto path = resolvePath(base_dir, document_root);
        if (std::filesystem::exists(path))
            return path;

#if defined(WAVE_SOURCE_DIR)
        const auto fallback_name = test_mode ? "site-test" : (demo_mode ? "site-demo" : "site");
        const auto fallback = std::filesystem::path(WAVE_SOURCE_DIR) / fallback_name;
        if (std::filesystem::exists(fallback))
        {
            LOG_INFO(
                "Document root {} not found; using {}",
                path.string(),
                fallback.string());
            return fallback;
        }
#endif

        return path;
    }();
    const auto resolved_database_path = resolvePath(base_dir, database_path);
    const auto resolved_upload_path = resolvePath(base_dir, "uploads");
    const auto resolved_gestures_root = [&]() -> std::filesystem::path
    {
        auto path = resolvePath(base_dir, "gestures");
        if (std::filesystem::exists(path))
            return path;

#if defined(WAVE_SOURCE_DIR)
        const auto fallback = std::filesystem::path(WAVE_SOURCE_DIR) / "bin" / "gestures";
        if (std::filesystem::exists(fallback))
        {
            LOG_INFO(
                "Gestures directory {} not found; using {}",
                path.string(),
                fallback.string());
            return fallback;
        }
#endif

        return path;
    }();

    if (!std::filesystem::exists(resolved_document_root))
    {
        LOG_WARN(
            "Document root does not exist: {} (run scripts/build-site.sh, build-site-test.sh, or build-site-demo.sh)",
            resolved_document_root.string());
    }

    if (!test_mode)
    {
        if (read_only)
        {
            if (!std::filesystem::exists(resolved_database_path))
            {
                LOG_ERROR("Read-only database not found: {}", resolved_database_path.string());
                return false;
            }
        }
        else if (!ensureParentDir(resolved_database_path))
        {
            return false;
        }
    }

    if (!ensureDir(resolved_upload_path))
        return false;

    auto& app = drogon::app();
    app.disableSigtermHandling();
    app.setLogLevel(trantor::Logger::kWarn);
    app.setThreadNum(thread_num);
    app.setDocumentRoot(resolved_document_root.string());
    app.setHomePage(home_page);
    app.setImplicitPageEnable(true);
    app.setImplicitPage(home_page);
    app.setUploadPath(resolved_upload_path.string());
    configureStaticFileTypes(app);

    if (!std::filesystem::exists(resolved_gestures_root))
    {
        LOG_WARN(
            "Gestures directory does not exist: {} (thumbnail URLs under /gestures/ will 404)",
            resolved_gestures_root.string());
    }
    else
    {
        const auto gestures_alias = std::filesystem::weakly_canonical(resolved_gestures_root).string();
        app.addALocation("/gestures", "", gestures_alias);
        LOG_INFO("Serving gesture assets at /gestures/ from {}", gestures_alias);
    }

    app.addListener("0.0.0.0", port);

    if (demo_mode)
        registerDemoPolicy();

    if (!test_mode)
    {
        drogon::orm::Sqlite3Config db_config;
        db_config.connectionNumber = thread_num > 0 ? thread_num : 1;
        // Drogon uses sqlite3_open(); "?mode=ro" without the file: URI scheme opens a wrong path.
        db_config.filename = resolved_database_path.string();
        db_config.name = "default";
        db_config.timeout = -1.0;
        app.addDbClient(db_config);

        const bool skip_db = skip_migrations;
        app.registerBeginningAdvice([skip_db, read_only]()
        {
            auto client = drogon::app().getDbClient();
            if (read_only)
            {
                try
                {
                    client->execSqlSync("PRAGMA query_only = ON");
                }
                catch (const std::exception& e)
                {
                    LOG_WARN("PRAGMA query_only failed: {}", e.what());
                }
            }
            else
            {
                db::configureConnectionSettings(client);
            }
            const bool ok = skip_db
                ? db::validateDatabaseSchema(client)
                : db::runMigrations(client);
            if (!ok)
            {
                LOG_ERROR("Database preparation failed; stopping server");
                drogon::app().quit();
                return;
            }

            AppState::get().onDatabaseReady(client);
        });
    }
    else
    {
        LOG_INFO("Test mode: static hosting only (no database, no API DB routes)");
    }

    m_impl->port = port;
    m_impl->documentRoot = resolved_document_root.string();

    LOG_INFO(
        "Web server configured: port={}, threads={}, document_root={}, database={}, uploads={}, test_mode={}, demo_mode={}, read_only={}, skip_migrations={}",
        port,
        thread_num,
        m_impl->documentRoot,
        test_mode ? "(disabled)" : resolved_database_path.string(),
        resolved_upload_path.string(),
        test_mode,
        demo_mode,
        read_only,
        skip_migrations);

    return true;
}

void Server::run()
{
    if (m_impl->running.exchange(true, std::memory_order_acq_rel))
    {
        LOG_WARN("Web server thread is already running");
        return;
    }

    m_impl->thread = std::thread([port = m_impl->port, document_root = m_impl->documentRoot]()
    {
        LOG_INFO("Web server starting on 0.0.0.0:{} (static: {})", port, document_root);
        drogon::app().run();
        LOG_INFO("Web server event loop exited");
    });
}

void Server::shutdown()
{
    if (!m_impl->running.exchange(false, std::memory_order_acq_rel))
        return;

    LOG_INFO("Shutting down web server...");
    drogon::app().quit();

    if (m_impl->thread.joinable())
        m_impl->thread.join();

    LOG_INFO("Web server shutdown complete");
}

WEB_NAMESPACE_END
WAVE_NAMESPACE_END
