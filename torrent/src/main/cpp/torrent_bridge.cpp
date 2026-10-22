/*
 * Copyright (c) 2026 honyomi
 *
 * Torrent bridge — BitTorrent download engine backed by libtorrent-rasterbar
 * 2.0.15 (static, prebuilt per ABI; see third_party/torrent/README.md).
 *
 * Architecture:
 *   - One lt::session per process, created by torrentStart() and torn down
 *     by torrentShutdown(). libtorrent runs its own network/disk threads;
 *     the NAPI calls only post work to them, so the session is driven from
 *     a single dedicated worker thread (workers/TorrentWorker.ets) and never
 *     blocks the UI thread.
 *   - Torrents are addressed by a small integer key returned from the add
 *     calls; the key stays valid until torrentRemove() completes.
 *   - Events are pull-based: torrentPollAlerts() drains the alert queue and
 *     returns a JSON array; the worker hosts the polling timer.
 *   - Files are written straight into the caller-provided save directory
 *     (app sandbox); ArkTS reads completed files via @ohos.file.fs.
 */

/*
 * OHOS musl compatibility (must precede every include):
 *   - OHOS musl has no pthread cancellation APIs. Boost.Core's
 *     sp_thread_sleep.hpp (pulled in by libtorrent headers) calls
 *     pthread_setcancelstate() on non-Android POSIX targets; shim it to a
 *     no-op. The PTHREAD_CANCEL_* constants exist, only the functions are
 *     missing. This mirrors the -D used when building the prebuilt library
 *     (see third_party/torrent/README.md).
 */
#define pthread_setcancelstate(state, oldstate) ((void)(state), (void)(oldstate))

#include "libtorrent/add_torrent_params.hpp"
#include "libtorrent/address.hpp"
#include "libtorrent/announce_entry.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/ip_filter.hpp"
#include "libtorrent/libtorrent.hpp"
#include "libtorrent/magnet_uri.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/torrent_handle.hpp"
#include "libtorrent/torrent_info.hpp"

#include "hilog/log.h"
#include "napi/native_api.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#define TORRENT_LOG_DOMAIN 0x0002
#define TORRENT_LOG_TAG "TorrentEngine"

// ============================================================================
// Session state
// ============================================================================

namespace {

struct TorrentEntry {
    lt::torrent_handle handle;
    std::string savePath;
};

std::mutex g_sessionLock;
std::unique_ptr<lt::session> g_session;
std::string g_downloadDir;
std::map<uint32_t, TorrentEntry> g_torrents;
std::atomic<uint32_t> g_nextTorrentId{1};
// DHT node count is fetched via session stats: metric index into
// session_stats_alert::counters() and the latest cached value.
int g_dhtNodesMetricIdx = -1;
int g_dhtNodes = 0;

// ============================================================================
// String helpers
// ============================================================================

// Short name for libtorrent socket_type_t (used by listen* alert events).
const char *SocketTypeName(lt::socket_type_t type)
{
    switch (type) {
        case lt::socket_type_t::tcp:
            return "tcp";
        case lt::socket_type_t::socks5:
            return "socks5";
        case lt::socket_type_t::http:
            return "http";
        case lt::socket_type_t::utp:
            return "utp";
        case lt::socket_type_t::i2p:
            return "i2p";
        case lt::socket_type_t::tcp_ssl:
            return "tcpSSL";
        case lt::socket_type_t::socks5_ssl:
            return "socks5SSL";
        case lt::socket_type_t::http_ssl:
            return "httpSSL";
        case lt::socket_type_t::utp_ssl:
            return "utpSSL";
        default:
            return "other";
    }
}

// Escapes JSON-special characters AND sanitizes to strict UTF-8: any invalid
// byte sequence is replaced with U+FFFD so napi_create_string_utf8 can never
// emit a corrupted JS string (torrent names come from untrusted metadata and
// may contain arbitrary non-UTF-8 bytes).
std::string JsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 8);
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        char simple = '\0';
        switch (c) {
            case '"':
                simple = '"';
                break;
            case '\\':
                simple = '\\';
                break;
            case '\n':
                simple = 'n';
                break;
            case '\r':
                simple = 'r';
                break;
            case '\t':
                simple = 't';
                break;
            default:
                break;
        }
        if (simple != '\0') {
            out += '\\';
            out += simple;
            i += 1;
            continue;
        }
        if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
            i += 1;
            continue;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
            i += 1;
            continue;
        }
        // Multi-byte UTF-8 sequence: validate structure, overlong forms,
        // surrogates and > U+10FFFF before passing through.
        size_t len = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07u;
        }
        bool valid = len > 0 && i + len <= n;
        if (valid) {
            for (size_t k = 1; k < len; k++) {
                const unsigned char cc = static_cast<unsigned char>(text[i + k]);
                if ((cc & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
                cp = (cp << 6) | (cc & 0x3Fu);
            }
        }
        if (valid) {
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
                (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
                (cp >= 0xD800 && cp <= 0xDFFF)) {
                valid = false;
            }
        }
        if (valid) {
            out.append(text, i, len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD"; // U+FFFD
            i += 1;
        }
    }
    return out;
}

const char *StateName(lt::torrent_status::state_t state)
{
    switch (state) {
        case lt::torrent_status::checking_files:
            return "checking_files";
        case lt::torrent_status::downloading_metadata:
            return "downloading_metadata";
        case lt::torrent_status::downloading:
            return "downloading";
        case lt::torrent_status::finished:
            return "finished";
        case lt::torrent_status::seeding:
            return "seeding";
        case lt::torrent_status::checking_resume_data:
            return "checking_resume_data";
        default:
            return "unknown";
    }
}

// ============================================================================
// NAPI helpers
// ============================================================================

napi_value NapiString(napi_env env, const std::string &text)
{
    napi_value value = nullptr;
    napi_create_string_utf8(env, text.c_str(), text.size(), &value);
    return value;
}

std::string NapiUtf8(napi_env env, napi_value value)
{
    napi_valuetype type = napi_undefined;
    if (value == nullptr || napi_typeof(env, value, &type) != napi_ok || type != napi_string) {
        return "";
    }
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) {
        return "";
    }
    std::vector<char> buf(len + 1, '\0');
    size_t written = 0;
    if (napi_get_value_string_utf8(env, value, buf.data(), buf.size(), &written) != napi_ok) {
        return "";
    }
    return std::string(buf.data(), written);
}

bool NapiBool(napi_env env, napi_value value, bool fallback)
{
    bool result = fallback;
    if (value == nullptr || napi_get_value_bool(env, value, &result) != napi_ok) {
        return fallback;
    }
    return result;
}

// Throws a JS Error with `message`; returns nullptr so callers can
// `return Throw(env, "...")` directly.
napi_value Throw(napi_env env, const std::string &message)
{
    napi_throw_error(env, "torrent_error", message.c_str());
    return nullptr;
}

// ============================================================================
// Status JSON
// ============================================================================

std::string BuildStatusJson(uint32_t key, const TorrentEntry &entry)
{
    const lt::torrent_handle &handle = entry.handle;
    if (!handle.is_valid()) {
        return "{\"error\":\"invalid handle\"}";
    }
    const lt::torrent_status st = handle.status();

    std::string name = st.name;
    // torrent_status::torrent_file is a weak_ptr in libtorrent 2.x
    const std::shared_ptr<const lt::torrent_info> meta = st.torrent_file.lock();
    if (name.empty() && meta != nullptr) {
        name = meta->name();
    }
    // Assembled as std::string: torrent names can exceed any fixed buffer,
    // and snprintf truncation would produce invalid JSON.
    std::string json = "{\"key\":";
    json += std::to_string(key);
    json += ",\"name\":\"";
    json += JsonEscape(name);
    json += "\",\"savePath\":\"";
    json += JsonEscape(entry.savePath);
    json += "\",\"state\":\"";
    json += StateName(st.state);
    char num[64];
    std::snprintf(num, sizeof(num), "%.4f", static_cast<double>(st.progress));
    json += "\",\"progress\":";
    json += num;
    json += ",\"totalWanted\":";
    json += std::to_string(static_cast<long long>(st.total_wanted));
    json += ",\"totalDone\":";
    json += std::to_string(static_cast<long long>(st.total_done));
    json += ",\"downloadRate\":";
    json += std::to_string(static_cast<long long>(st.download_payload_rate));
    json += ",\"uploadRate\":";
    json += std::to_string(static_cast<long long>(st.upload_payload_rate));
    json += ",\"numPeers\":";
    json += std::to_string(static_cast<int>(st.num_peers));
    json += ",\"numSeeds\":";
    json += std::to_string(static_cast<int>(st.num_seeds));
    json += ",\"numPieces\":";
    json += std::to_string(static_cast<int>(st.num_pieces));
    json += ",\"piecesDone\":";
    json += std::to_string(static_cast<int>(st.pieces.count()));
    json += ",\"isFinished\":";
    json += (st.is_finished ? "true" : "false");
    json += ",\"isSeeding\":";
    json += (st.is_seeding ? "true" : "false");
    json += ",\"hasMetadata\":";
    // libtorrent 2.x sets torrent_file to a *placeholder* torrent_info built
    // from just the info hash for magnet-added torrents without metadata;
    // is_valid() distinguishes that placeholder from real metadata.
    json += (meta != nullptr && meta->is_valid() ? "true" : "false");
    json += ",\"infoHash\":\"";
    if (!st.info_hashes.v1.is_all_zeros()) {
        // sha1_hash::to_string() returns the 20 RAW hash bytes, not hex.
        // Raw binary would corrupt the JSON string (invalid UTF-8 / quotes).
        const unsigned char *hash = reinterpret_cast<const unsigned char *>(st.info_hashes.v1.data());
        static const char *hexDigits = "0123456789abcdef";
        for (size_t b = 0; b < 20; b++) {
            json += hexDigits[hash[b] >> 4];
            json += hexDigits[hash[b] & 0x0F];
        }
    }
    json += "\"}";
    return json;
}

// ============================================================================
// NAPI surface
// ============================================================================

// torrentStart(downloadDir, listenInterfaces, enableDht, downloadLimit,
//              uploadLimit, connectionsLimit, userAgent) -> boolean
napi_value TorrentStart(napi_env env, napi_callback_info info)
{
    size_t argc = 7;
    napi_value args[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        return Throw(env, "torrentStart: downloadDir is required");
    }
    const std::string downloadDir = NapiUtf8(env, args[0]);
    if (downloadDir.empty()) {
        return Throw(env, "torrentStart: downloadDir must not be empty");
    }
    // 注意：ArkTS 侧传 undefined 时 NapiUtf8 返回空串（不是缺参），
    // 空的 listen_interfaces 会让 libtorrent 不建任何监听套接字，
    // DHT（依赖 UDP 监听）随之失效 —— 纯 infohash magnet 将永远找不到 peer。
    std::string listen = argc > 1 && args[1] != nullptr ? NapiUtf8(env, args[1]) : std::string();
    if (listen.empty()) {
        listen = "0.0.0.0:0";
    }
    const bool enableDht = argc > 2 ? NapiBool(env, args[2], true) : true;
    int64_t downloadLimit = 0;
    if (argc > 3 && args[3] != nullptr) {
        napi_get_value_int64(env, args[3], &downloadLimit);
    }
    int64_t uploadLimit = 0;
    if (argc > 4 && args[4] != nullptr) {
        napi_get_value_int64(env, args[4], &uploadLimit);
    }
    int32_t connectionsLimit = 0;
    if (argc > 5 && args[5] != nullptr) {
        napi_get_value_int32(env, args[5], &connectionsLimit);
    }
    std::string userAgent = argc > 6 && args[6] != nullptr ? NapiUtf8(env, args[6]) : std::string();
    if (userAgent.empty()) {
        userAgent = "Honyomi/1.0";
    }

    std::lock_guard<std::mutex> lock(g_sessionLock);
    napi_value result = nullptr;
    if (g_session) {
        napi_get_boolean(env, true, &result); // idempotent
        return result;
    }

    lt::settings_pack settings = lt::default_settings();
    settings.set_str(lt::settings_pack::listen_interfaces, listen);
    settings.set_str(lt::settings_pack::user_agent, userAgent);
    settings.set_int(lt::settings_pack::alert_queue_size, 4000);
    if (downloadLimit > 0) {
        settings.set_int(lt::settings_pack::download_rate_limit, static_cast<int>(downloadLimit));
    }
    if (uploadLimit > 0) {
        settings.set_int(lt::settings_pack::upload_rate_limit, static_cast<int>(uploadLimit));
    }
    if (connectionsLimit > 0) {
        settings.set_int(lt::settings_pack::connections_limit, connectionsLimit);
    }
    settings.set_int(lt::settings_pack::enable_dht, enableDht ? 1 : 0);
    settings.set_bool(lt::settings_pack::enable_lsd, true);
    if (enableDht) {
        // 显式 bootstrap 节点：纯 infohash magnet（animegarden 无 tr=）完全依赖 DHT。
        settings.set_str(lt::settings_pack::dht_bootstrap_nodes,
            "dht.libtorrent.org:25401,"
            "router.bittorrent.com:6881,"
            "dht.transmissionbt.com:6881,"
            "router.utorrent.com:6881,"
            "router.bitcomet.com:6881");
    }
    // peer 发现与传输调优（对齐 anitorrent / animeko 生产配置，libtorrent 2.0.x）：
    // 内置 tracker 列表全部注入 tier 0，必须允许并行向同层所有 tracker 上报，
    // 否则多 tracker 规范语义下每层只会选中一个 tracker，其余形同虚设。
    settings.set_bool(lt::settings_pack::announce_to_all_trackers, true);
    settings.set_int(lt::settings_pack::active_downloads, 8);
    settings.set_int(lt::settings_pack::active_limit, 2000);
    settings.set_int(lt::settings_pack::piece_timeout, 5);
    settings.set_int(lt::settings_pack::request_timeout, 1);
    settings.set_int(lt::settings_pack::max_out_request_queue, 2000);
    settings.set_int(lt::settings_pack::torrent_connect_boost, 200);
    settings.set_int(lt::settings_pack::peer_connect_timeout, 5);
    settings.set_int(lt::settings_pack::max_failcount, 2);
    settings.set_int(lt::settings_pack::max_peer_recv_buffer_size, 5 * 1024 * 1024);
    settings.set_int(lt::settings_pack::aio_threads, 8);
    settings.set_int(lt::settings_pack::checking_mem_usage, 2048);
    const lt::alert_category_t alertMask = lt::alert_category::error
        | lt::alert_category::status
        | lt::alert_category::storage
        | lt::alert_category::tracker
        | lt::alert_category::dht
        | lt::alert_category::connect;
    // ABI 3 makes bitfield_flag's conversion operator explicit.
    settings.set_int(lt::settings_pack::alert_mask,
        static_cast<int>(static_cast<std::uint32_t>(alertMask)));

    lt::session_params params(settings);
    g_session = std::make_unique<lt::session>(params);
    g_dhtNodes = 0;
    for (const lt::stats_metric &metric : lt::session_stats_metrics()) {
        if (std::strcmp(metric.name, "dht.dht_nodes") == 0) {
            g_dhtNodesMetricIdx = metric.value_index;
            break;
        }
    }
    g_session->post_session_stats();
    g_downloadDir = downloadDir;
    g_torrents.clear();
    OH_LOG_Print(LOG_APP, LOG_INFO, TORRENT_LOG_DOMAIN, TORRENT_LOG_TAG,
        "session started (listen=%{public}s dht=%{public}d)", listen.c_str(), enableDht ? 1 : 0);
    napi_get_boolean(env, true, &result);
    return result;
}

// torrentShutdown() -> boolean. Session threads are joined synchronously.
napi_value TorrentShutdown(napi_env env, napi_callback_info info)
{
    (void)info;
    std::lock_guard<std::mutex> lock(g_sessionLock);
    napi_value result = nullptr;
    if (!g_session) {
        napi_get_boolean(env, true, &result);
        return result;
    }
    g_torrents.clear();
    g_session.reset();
    g_downloadDir.clear();
    OH_LOG_Print(LOG_APP, LOG_INFO, TORRENT_LOG_DOMAIN, TORRENT_LOG_TAG, "session stopped");
    napi_get_boolean(env, true, &result);
    return result;
}

// Resolves the save path for a new torrent: explicit param, else the session
// default. Never returns an empty string (torrentStart enforces downloadDir).
std::string ResolveSavePath(napi_env env, napi_value arg)
{
    const std::string path = arg != nullptr ? NapiUtf8(env, arg) : std::string();
    if (!path.empty()) {
        return path;
    }
    return g_downloadDir;
}

// Shared tail of torrentAddMagnet / torrentAddTorrentFile.
napi_value AddTorrentParamsToKey(napi_env env, lt::add_torrent_params params)
{
    // 调用方传入的每任务 save_path 优先（TorrentAddMagnet/TorrentAddTorrentFile
    // 已从参数写入）；仅在其为空时回退到会话默认目录。
    if (params.save_path.empty()) {
        params.save_path = ResolveSavePath(env, nullptr);
    }
    // Explicit control: pause/resume are never overridden by the auto
    // manager, and torrents download immediately after being added.
    params.flags &= ~lt::torrent_flags::auto_managed;
    params.flags &= ~lt::torrent_flags::paused;

    uint32_t key = 0;
    {
        std::lock_guard<std::mutex> lock(g_sessionLock);
        if (!g_session) {
            return Throw(env, "torrent session not started");
        }
        lt::error_code ec;
        lt::torrent_handle handle = g_session->add_torrent(params, ec);
        if (ec) {
            return Throw(env, "add torrent failed: " + ec.message());
        }
        key = g_nextTorrentId.fetch_add(1);
        TorrentEntry entry;
        entry.handle = handle;
        entry.savePath = params.save_path;
        g_torrents[key] = entry;
    }
    napi_value result = nullptr;
    napi_create_uint32(env, key, &result);
    return result;
}

// torrentAddMagnet(magnetUri, savePath?) -> key
napi_value TorrentAddMagnet(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string uri = argc > 0 ? NapiUtf8(env, args[0]) : std::string();
    if (uri.rfind("magnet:?", 0) != 0) {
        return Throw(env, "not a magnet URI");
    }
    lt::add_torrent_params params;
    try {
        params = lt::parse_magnet_uri(uri);
    } catch (const lt::system_error &err) {
        return Throw(env, std::string("invalid magnet URI: ") + err.what());
    }
    if (argc > 1 && args[1] != nullptr) {
        const std::string path = NapiUtf8(env, args[1]);
        if (!path.empty()) {
            params.save_path = path;
        }
    }
    return AddTorrentParamsToKey(env, std::move(params));
}

// torrentAddTorrentFile(bytes: ArrayBuffer, savePath?) -> key
napi_value TorrentAddTorrentFile(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool isAb = false;
    if (argc < 1 || args[0] == nullptr || napi_is_arraybuffer(env, args[0], &isAb) != napi_ok || !isAb) {
        return Throw(env, "torrentAddTorrentFile: expected ArrayBuffer with .torrent content");
    }
    void *data = nullptr;
    size_t length = 0;
    if (napi_get_arraybuffer_info(env, args[0], &data, &length) != napi_ok || data == nullptr || length == 0) {
        return Throw(env, "torrentAddTorrentFile: empty buffer");
    }
    std::vector<char> buffer(static_cast<char *>(data), static_cast<char *>(data) + length);
    lt::add_torrent_params params;
    try {
        params = lt::load_torrent_buffer(buffer);
    } catch (const lt::system_error &err) {
        return Throw(env, std::string("invalid .torrent file: ") + err.what());
    }
    if (argc > 1 && args[1] != nullptr) {
        const std::string path = NapiUtf8(env, args[1]);
        if (!path.empty()) {
            params.save_path = path;
        }
    }
    return AddTorrentParamsToKey(env, std::move(params));
}

// torrentStatus(key) -> JSON string (see BuildStatusJson).
napi_value TorrentStatus(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t key = 0;
    if (argc < 1 || napi_get_value_uint32(env, args[0], &key) != napi_ok) {
        return Throw(env, "torrentStatus: invalid key");
    }
    std::lock_guard<std::mutex> lock(g_sessionLock);
    auto it = g_torrents.find(key);
    if (it == g_torrents.end()) {
        return Throw(env, "torrent not found");
    }
    return NapiString(env, BuildStatusJson(key, it->second));
}

// torrentFiles(key) -> JSON array of file entries with byte progress.
napi_value TorrentFiles(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t key = 0;
    if (argc < 1 || napi_get_value_uint32(env, args[0], &key) != napi_ok) {
        return Throw(env, "torrentFiles: invalid key");
    }
    std::lock_guard<std::mutex> lock(g_sessionLock);
    auto it = g_torrents.find(key);
    if (it == g_torrents.end()) {
        return Throw(env, "torrent not found");
    }
    const lt::torrent_handle &handle = it->second.handle;
    const std::shared_ptr<const lt::torrent_info> meta = handle.torrent_file();
    if (meta == nullptr) {
        return NapiString(env, "[]"); // metadata not yet received
    }
    std::vector<std::int64_t> progress;
    handle.file_progress(progress);
    const lt::file_storage &files = meta->files();
    std::string json = "[";
    for (lt::file_index_t i : files.file_range()) {
        const std::int64_t size = files.file_size(i);
        const int idx = static_cast<int>(i);
        const std::int64_t done = idx < static_cast<int>(progress.size()) ? progress[idx] : 0;
        // Dynamic assembly: file paths from torrent metadata are unbounded.
        json += (json.size() > 1 ? "," : "");
        json += "{\"index\":";
        json += std::to_string(idx);
        json += ",\"path\":\"";
        json += JsonEscape(files.file_path(i));
        char tail[64];
        std::snprintf(tail, sizeof(tail), "\",\"size\":%lld,\"done\":%lld,\"progress\":%.4f}",
            static_cast<long long>(size), static_cast<long long>(done),
            size > 0 ? static_cast<double>(done) / static_cast<double>(size) : 1.0);
        json += tail;
    }
    json += "]";
    return NapiString(env, json);
}

// torrentSetSequential(key, enabled) -> boolean
napi_value TorrentSetSequential(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t key = 0;
    if (argc < 1 || napi_get_value_uint32(env, args[0], &key) != napi_ok) {
        return Throw(env, "torrentSetSequential: invalid key");
    }
    const bool enabled = argc > 1 ? NapiBool(env, args[1], true) : true;
    std::lock_guard<std::mutex> lock(g_sessionLock);
    auto it = g_torrents.find(key);
    if (it == g_torrents.end()) {
        return Throw(env, "torrent not found");
    }
    if (enabled) {
        it->second.handle.set_flags(lt::torrent_flags::sequential_download);
    } else {
        it->second.handle.unset_flags(lt::torrent_flags::sequential_download);
    }
    napi_value result = nullptr;
    napi_get_boolean(env, true, &result);
    return result;
}

// torrentPause(key) / torrentResume(key) -> boolean
napi_value TorrentPauseResume(napi_env env, napi_callback_info info, bool pause)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t key = 0;
    if (argc < 1 || napi_get_value_uint32(env, args[0], &key) != napi_ok) {
        return Throw(env, pause ? "torrentPause: invalid key" : "torrentResume: invalid key");
    }
    std::lock_guard<std::mutex> lock(g_sessionLock);
    auto it = g_torrents.find(key);
    if (it == g_torrents.end()) {
        return Throw(env, "torrent not found");
    }
    if (pause) {
        it->second.handle.pause();
    } else {
        it->second.handle.resume();
    }
    napi_value result = nullptr;
    napi_get_boolean(env, true, &result);
    return result;
}

napi_value TorrentPause(napi_env env, napi_callback_info info)
{
    return TorrentPauseResume(env, info, true);
}

napi_value TorrentResume(napi_env env, napi_callback_info info)
{
    return TorrentPauseResume(env, info, false);
}

// torrentRemove(key, deleteFiles) -> boolean
napi_value TorrentRemove(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t key = 0;
    if (argc < 1 || napi_get_value_uint32(env, args[0], &key) != napi_ok) {
        return Throw(env, "torrentRemove: invalid key");
    }
    const bool deleteFiles = argc > 1 ? NapiBool(env, args[1], false) : false;
    std::lock_guard<std::mutex> lock(g_sessionLock);
    auto it = g_torrents.find(key);
    if (it == g_torrents.end()) {
        return Throw(env, "torrent not found");
    }
    if (g_session) {
        g_session->remove_torrent(it->second.handle,
            deleteFiles ? lt::session_handle::delete_files : lt::remove_flags_t{});
    }
    g_torrents.erase(it);
    napi_value result = nullptr;
    napi_get_boolean(env, true, &result);
    return result;
}

// torrentSetLimits(downloadLimit, uploadLimit) -> boolean (session-wide,
// bytes/s; 0 = unlimited)
napi_value TorrentSetLimits(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int64_t downloadLimit = 0;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_int64(env, args[0], &downloadLimit);
    }
    int64_t uploadLimit = 0;
    if (argc > 1 && args[1] != nullptr) {
        napi_get_value_int64(env, args[1], &uploadLimit);
    }
    std::lock_guard<std::mutex> lock(g_sessionLock);
    if (!g_session) {
        return Throw(env, "torrent session not started");
    }
    lt::settings_pack settings;
    settings.set_int(lt::settings_pack::download_rate_limit, downloadLimit > 0 ? static_cast<int>(downloadLimit) : 0);
    settings.set_int(lt::settings_pack::upload_rate_limit, uploadLimit > 0 ? static_cast<int>(uploadLimit) : 0);
    g_session->apply_settings(settings);
    napi_value result = nullptr;
    napi_get_boolean(env, true, &result);
    return result;
}

// Parses a JSON array of strings, e.g. ["udp://a/announce","http://b/announce"].
// The input comes from our own ArkTS (JSON.stringify of a string array), but
// the parser still validates structure and escapes; on any malformed element
// it returns false and the caller adds nothing (all-or-nothing).
bool ParseJsonStringArray(const std::string &json, std::vector<std::string> &out)
{
    size_t i = 0;
    const size_t n = json.size();
    auto skipWs = [&]() {
        while (i < n && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) {
            i++;
        }
    };
    skipWs();
    if (i >= n || json[i] != '[') {
        return false;
    }
    i++;
    skipWs();
    if (i < n && json[i] == ']') {
        return true; // empty array
    }
    while (true) {
        skipWs();
        if (i >= n || json[i] != '"') {
            return false;
        }
        i++;
        std::string text;
        bool ok = true;
        while (i < n && json[i] != '"') {
            const char c = json[i];
            if (static_cast<unsigned char>(c) < 0x20) {
                return false; // raw control characters are invalid JSON
            }
            if (c != '\\') {
                text += c;
                i++;
                continue;
            }
            i++;
            if (i >= n) {
                ok = false;
                break;
            }
            const char e = json[i];
            switch (e) {
                case '"': text += '"'; break;
                case '\\': text += '\\'; break;
                case '/': text += '/'; break;
                case 'b': text += '\b'; break;
                case 'f': text += '\f'; break;
                case 'n': text += '\n'; break;
                case 'r': text += '\r'; break;
                case 't': text += '\t'; break;
                case 'u': {
                    if (i + 4 >= n) {
                        ok = false;
                        break;
                    }
                    uint32_t cp = 0;
                    for (int k = 1; k <= 4 && ok; k++) {
                        const char h = json[i + static_cast<size_t>(k)];
                        uint32_t v = 0;
                        if (h >= '0' && h <= '9') {
                            v = static_cast<uint32_t>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            v = static_cast<uint32_t>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            v = static_cast<uint32_t>(h - 'A' + 10);
                        } else {
                            ok = false;
                            break;
                        }
                        cp = (cp << 4) | v;
                    }
                    if (!ok) {
                        break;
                    }
                    // Tracker URLs are ASCII; encode anything else as UTF-8.
                    if (cp < 0x80) {
                        text += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        text += static_cast<char>(0xC0 | (cp >> 6));
                        text += static_cast<char>(0x80 | (cp & 0x3Fu));
                    } else {
                        text += static_cast<char>(0xE0 | (cp >> 12));
                        text += static_cast<char>(0x80 | ((cp >> 6) & 0x3Fu));
                        text += static_cast<char>(0x80 | (cp & 0x3Fu));
                    }
                    i += 4;
                    break;
                }
                default:
                    ok = false;
                    break;
            }
            if (!ok) {
                break;
            }
            i++;
        }
        if (!ok || i >= n) {
            return false;
        }
        i++; // closing quote
        out.push_back(text);
        skipWs();
        if (i < n && json[i] == ',') {
            i++;
            continue;
        }
        return i < n && json[i] == ']';
    }
}

// torrentAddTrackers(key, trackersJson) -> number of trackers added.
// Mirrors animeko/anitorrent: appended trackers land in tier 0 with no fail
// limit, and the session announces to every tracker of a tier in parallel
// (announce_to_all_trackers, set at torrentStart). Trackers the torrent
// already knows (e.g. the magnet's own tr= entries) are skipped.
napi_value TorrentAddTrackers(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t key = 0;
    if (argc < 1 || napi_get_value_uint32(env, args[0], &key) != napi_ok) {
        return Throw(env, "torrentAddTrackers: invalid key");
    }
    const std::string json = argc > 1 ? NapiUtf8(env, args[1]) : std::string();
    std::vector<std::string> trackers;
    if (!json.empty() && !ParseJsonStringArray(json, trackers)) {
        return Throw(env, "torrentAddTrackers: expected JSON array of tracker URLs");
    }
    std::lock_guard<std::mutex> lock(g_sessionLock);
    auto it = g_torrents.find(key);
    if (it == g_torrents.end()) {
        return Throw(env, "torrent not found");
    }
    lt::torrent_handle &handle = it->second.handle;
    std::set<std::string> existing;
    for (const lt::announce_entry &entry : handle.trackers()) {
        existing.insert(entry.url);
    }
    int added = 0;
    for (const std::string &url : trackers) {
        if (url.empty() || existing.find(url) != existing.end()) {
            continue;
        }
        lt::announce_entry entry(url);
        entry.tier = 0;
        entry.fail_limit = 0;
        handle.add_tracker(entry);
        existing.insert(url);
        added++;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, TORRENT_LOG_DOMAIN, TORRENT_LOG_TAG,
        "addTrackers: key=%{public}u requested=%{public}zu added=%{public}d",
        key, trackers.size(), added);
    napi_value result = nullptr;
    napi_create_int32(env, added, &result);
    return result;
}

// torrentSetShareRatioLimit(ratio) -> boolean (session-wide). The ratio is
// upload/download as a double (2.0 = twice as much uploaded as downloaded);
// libtorrent stores it as a percentage (200 = 2.0). When a seeding torrent
// reaches the ratio it is considered done and yields its active slot.
// A huge value means effectively unlimited.
napi_value TorrentSetShareRatioLimit(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double ratio = 2.0;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_double(env, args[0], &ratio);
    }
    if (!(ratio >= 1.0)) {
        ratio = 1.0;
    }
    if (ratio > 1000.0) {
        ratio = 1000.0;
    }
    std::lock_guard<std::mutex> lock(g_sessionLock);
    if (!g_session) {
        return Throw(env, "torrent session not started");
    }
    lt::settings_pack settings;
    settings.set_int(lt::settings_pack::share_ratio_limit,
        static_cast<int>(ratio * 100.0));
    g_session->apply_settings(settings);
    OH_LOG_Print(LOG_APP, LOG_INFO, TORRENT_LOG_DOMAIN, TORRENT_LOG_TAG,
        "setShareRatioLimit: %{public}d", static_cast<int>(ratio * 100.0));
    napi_value result = nullptr;
    napi_get_boolean(env, true, &result);
    return result;
}

// torrentSetIpFilter(rulesJson) -> number of blocked ranges applied
// (session-wide). Input is a JSON array of strings, each "from-to" (inclusive
// range) or a single address ("ip" blocks exactly that address). Mixed
// v4/v6 endpoints in one rule are rejected. Replaces the previous filter; an
// empty array clears it (all peers allowed).
napi_value TorrentSetIpFilter(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string json = (argc > 0 && args[0] != nullptr) ? NapiUtf8(env, args[0]) : std::string();
    std::vector<std::string> rules;
    if (!json.empty() && !ParseJsonStringArray(json, rules)) {
        return Throw(env, "torrentSetIpFilter: expected JSON array of ip rules");
    }
    std::lock_guard<std::mutex> lock(g_sessionLock);
    if (!g_session) {
        return Throw(env, "torrent session not started");
    }
    lt::ip_filter filter;
    int applied = 0;
    for (const std::string &rule : rules) {
        const size_t dash = rule.find('-');
        std::string fromText = rule;
        std::string toText = rule;
        if (dash != std::string::npos) {
            fromText = rule.substr(0, dash);
            toText = rule.substr(dash + 1);
        }
        boost::system::error_code ec;
        const lt::address from = lt::make_address(fromText, ec);
        if (ec) {
            return Throw(env, "torrentSetIpFilter: invalid address");
        }
        const lt::address to = lt::make_address(toText, ec);
        if (ec) {
            return Throw(env, "torrentSetIpFilter: invalid address");
        }
        if (from.is_v4() != to.is_v4()) {
            return Throw(env, "torrentSetIpFilter: mixed address families");
        }
        filter.add_rule(from, to, lt::ip_filter::blocked);
        applied++;
    }
    g_session->set_ip_filter(filter);
    OH_LOG_Print(LOG_APP, LOG_INFO, TORRENT_LOG_DOMAIN, TORRENT_LOG_TAG,
        "setIpFilter: rules=%{public}d applied=%{public}d", static_cast<int>(rules.size()), applied);
    napi_value result = nullptr;
    napi_create_int32(env, applied, &result);
    return result;
}

// torrentPollAlerts(maxCount?) -> JSON array of events. Event shapes:
//   {"type":"metadata","key":N,"name":"...","numFiles":N}
//   {"type":"finished","key":N}
//   {"type":"state","key":N,"state":"..."}
//   {"type":"error","key":N,"message":"..."}
//   {"type":"fileError","key":N,"file":"...","message":"..."}
//   {"type":"removed","key":N}
//   {"type":"trackerError","key":N,"url":"...","message":"...","timesInRow":N}
//   {"type":"trackerReply","key":N,"url":"...","peers":N}
//   {"type":"peerConnect","key":N,"endpoint":"ip:port"}
//   {"type":"peerDisconnect","key":N,"message":"..."}
//   {"type":"listen","address":"...","port":N,"socket":"tcp|udp|..."}
//   {"type":"listenFailed","interface":"...","port":N,"message":"..."}
//   {"type":"dhtReady"}
//   {"type":"sessionError","message":"..."}
//   {"type":"alert","message":"..."}   (catch-all for any other alert type)
napi_value TorrentPollAlerts(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t maxCount = 64;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &maxCount);
    }
    if (maxCount <= 0) {
        maxCount = 64;
    }
    if (maxCount > 512) {
        maxCount = 512;
    }

    std::lock_guard<std::mutex> lock(g_sessionLock);
    napi_value result = nullptr;
    if (!g_session) {
        napi_create_array(env, &result);
        return result;
    }

    // Alerts carry torrent handles; find the key by matching handle identity.
    auto keyOf = [](const lt::torrent_handle &handle) -> uint32_t {
        for (const auto &item : g_torrents) {
            if (item.second.handle == handle) {
                return item.first;
            }
        }
        return 0;
    };
    // torrent_removed_alert carries no valid handle; match by info hashes.
    auto keyOfHashes = [](const lt::info_hash_t &hashes) -> uint32_t {
        for (const auto &item : g_torrents) {
            if (item.second.handle.info_hashes() == hashes) {
                return item.first;
            }
        }
        return 0;
    };

    std::vector<lt::alert *> alerts;
    g_session->pop_alerts(&alerts);
    std::string json = "[";
    int emitted = 0;
    char buf[768];
    for (const lt::alert *alert : alerts) {
        if (alert == nullptr || emitted >= maxCount) {
            break;
        }
    std::string event;
    if (auto *a = lt::alert_cast<lt::metadata_received_alert>(alert)) {
            const uint32_t key = keyOf(a->handle);
            std::string name;
            int numFiles = 0;
            const std::shared_ptr<const lt::torrent_info> meta = a->handle.torrent_file();
            if (meta != nullptr) {
                name = meta->name();
                numFiles = static_cast<int>(meta->num_files());
            }
            // Dynamic assembly: long metadata names would truncate a fixed buffer.
            event = "{\"type\":\"metadata\",\"key\":";
            event += std::to_string(key);
            event += ",\"name\":\"";
            event += JsonEscape(name);
            event += "\",\"numFiles\":";
            event += std::to_string(numFiles);
            event += "}";
        } else if (lt::alert_cast<lt::torrent_finished_alert>(alert) != nullptr) {
            const auto *a = lt::alert_cast<lt::torrent_finished_alert>(alert);
            std::snprintf(buf, sizeof(buf), "{\"type\":\"finished\",\"key\":%u}", keyOf(a->handle));
            event = buf;
        } else if (const auto *a = lt::alert_cast<lt::state_changed_alert>(alert)) {
            std::snprintf(buf, sizeof(buf),
                "{\"type\":\"state\",\"key\":%u,\"state\":\"%s\"}",
                keyOf(a->handle), StateName(a->state));
            event = buf;
        } else if (const auto *a = lt::alert_cast<lt::torrent_error_alert>(alert)) {
            event = "{\"type\":\"error\",\"key\":";
            event += std::to_string(keyOf(a->handle));
            event += ",\"message\":\"";
            event += JsonEscape(a->error.message());
            event += "\"}";
        } else if (const auto *a = lt::alert_cast<lt::file_error_alert>(alert)) {
            event = "{\"type\":\"fileError\",\"key\":";
            event += std::to_string(keyOf(a->handle));
            event += ",\"file\":\"";
            event += JsonEscape(a->filename());
            event += "\",\"message\":\"";
            event += JsonEscape(a->error.message());
            event += "\"}";
        } else if (const auto *a = lt::alert_cast<lt::torrent_removed_alert>(alert)) {
            // Removal may originate from this bridge (map entry already gone)
            // or from libtorrent itself; report key 0 when unknown.
            std::snprintf(buf, sizeof(buf), "{\"type\":\"removed\",\"key\":%u}",
                keyOfHashes(a->info_hashes));
            event = buf;
        } else if (const auto *a = lt::alert_cast<lt::tracker_error_alert>(alert)) {
            event = "{\"type\":\"trackerError\",\"key\":";
            event += std::to_string(keyOf(a->handle));
            event += ",\"url\":\"";
            event += JsonEscape(a->tracker_url());
            event += "\",\"message\":\"";
            event += JsonEscape(a->error.message());
            event += "\",\"timesInRow\":";
            event += std::to_string(a->times_in_row);
            event += "}";
        } else if (const auto *a = lt::alert_cast<lt::tracker_reply_alert>(alert)) {
            event = "{\"type\":\"trackerReply\",\"key\":";
            event += std::to_string(keyOf(a->handle));
            event += ",\"url\":\"";
            event += JsonEscape(a->tracker_url());
            event += "\",\"peers\":";
            event += std::to_string(a->num_peers);
            event += "}";
        } else if (const auto *a = lt::alert_cast<lt::peer_connect_alert>(alert)) {
            std::snprintf(buf, sizeof(buf), "{\"type\":\"peerConnect\",\"key\":%u,\"endpoint\":\"%s:%d\"}",
                keyOf(a->handle), a->endpoint.address().to_string().c_str(),
                a->endpoint.port());
            event = buf;
        } else if (const auto *a = lt::alert_cast<lt::peer_disconnected_alert>(alert)) {
            event = "{\"type\":\"peerDisconnect\",\"key\":";
            event += std::to_string(keyOf(a->handle));
            event += ",\"message\":\"";
            event += JsonEscape(a->error.message());
            event += "\"}";
        } else if (const auto *a = lt::alert_cast<lt::listen_succeeded_alert>(alert)) {
            std::snprintf(buf, sizeof(buf), "{\"type\":\"listen\",\"address\":\"%s\",\"port\":%d,\"socket\":\"%s\"}",
                a->address.to_string().c_str(), a->port, SocketTypeName(a->socket_type));
            event = buf;
        } else if (const auto *a = lt::alert_cast<lt::listen_failed_alert>(alert)) {
            event = "{\"type\":\"listenFailed\",\"interface\":\"";
            event += JsonEscape(a->listen_interface());
            event += "\",\"port\":";
            event += std::to_string(a->port);
            event += ",\"message\":\"";
            event += JsonEscape(a->error.message());
            event += "\"}";
        } else if (lt::alert_cast<lt::dht_bootstrap_alert>(alert) != nullptr) {
            event = "{\"type\":\"dhtReady\"}";
        } else if (const auto *a = lt::alert_cast<lt::session_error_alert>(alert)) {
            // wrap() converts any exception escaping the session's message pump
            // into this alert; surfacing it is the only way to see why the
            // engine (listen sockets / DHT / tracker announces) died.
            event = "{\"type\":\"sessionError\",\"message\":\"";
            event += JsonEscape(a->error.message());
            event += "\"}";
        } else if (const auto *a = lt::alert_cast<lt::session_stats_alert>(alert)) {
            // posted by post_session_stats(); refresh the cached DHT node count
            if (g_dhtNodesMetricIdx >= 0) {
                const lt::span<std::int64_t const> counters = a->counters();
                if (g_dhtNodesMetricIdx < static_cast<int>(counters.size())) {
                    g_dhtNodes = static_cast<int>(counters[g_dhtNodesMetricIdx]);
                }
            }
            event.clear();
        } else if (lt::alert_cast<lt::session_stats_header_alert>(alert) != nullptr) {
            event.clear();
        } else if (lt::alert_cast<lt::dht_outgoing_get_peers_alert>(alert) != nullptr) {
            // high-volume DHT lookup noise: intentionally not surfaced
            event.clear();
        } else {
            // Catch-all: an unhandled session_error_alert once hid the real
            // engine failure behind an empty event log — never drop silently.
            std::string text = alert->message();
            if (text.size() > 300) {
                text.resize(300);
            }
            event = "{\"type\":\"alert\",\"message\":\"";
            event += JsonEscape(text);
            event += "\"}";
        }
        if (!event.empty()) {
            json += (emitted > 0 ? "," : "") + event;
            emitted++;
        }
    }
    json += "]";
    napi_create_string_utf8(env, json.c_str(), json.size(), &result);
    return result;
}

// torrentSessionDhtNodes() -> number (0 when DHT is off/unready).
napi_value TorrentSessionDhtNodes(napi_env env, napi_callback_info info)
{
    (void)info;
    std::lock_guard<std::mutex> lock(g_sessionLock);
    napi_value result = nullptr;
    if (!g_session) {
        napi_create_int32(env, 0, &result);
        return result;
    }
    // Ask the session to publish a session_stats_alert; the cached count is
    // refreshed when TorrentPollAlerts consumes it (session::status() is
    // deprecated in libtorrent 2.x).
    g_session->post_session_stats();
    napi_create_int32(env, g_dhtNodes, &result);
    return result;
}

} // namespace

// ============================================================================
// Module registration — standalone NAPI module "torrent" (libtorrent.so)
// ============================================================================

static napi_value Export(napi_env env, napi_value exports)
{
    const napi_property_descriptor desc[] = {
        { "torrentStart", nullptr, TorrentStart, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentShutdown", nullptr, TorrentShutdown, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentAddMagnet", nullptr, TorrentAddMagnet, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentAddTorrentFile", nullptr, TorrentAddTorrentFile, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentAddTrackers", nullptr, TorrentAddTrackers, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentStatus", nullptr, TorrentStatus, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentFiles", nullptr, TorrentFiles, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentSetSequential", nullptr, TorrentSetSequential, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentPause", nullptr, TorrentPause, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentResume", nullptr, TorrentResume, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentRemove", nullptr, TorrentRemove, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentSetLimits", nullptr, TorrentSetLimits, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentSetShareRatioLimit", nullptr, TorrentSetShareRatioLimit, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentSetIpFilter", nullptr, TorrentSetIpFilter, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentPollAlerts", nullptr, TorrentPollAlerts, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "torrentSessionDhtNodes", nullptr, TorrentSessionDhtNodes, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

static napi_module g_torrentModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Export,
    .nm_modname = "torrent",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterTorrentModule(void)
{
    napi_module_register(&g_torrentModule);
}
