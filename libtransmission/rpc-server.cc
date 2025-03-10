// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cerrno>
#include <cstring> /* memcpy */
#include <ctime>
#include <list>
#include <string>
#include <string_view>
#include <vector>

#include <libdeflate.h>

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h> /* TODO: eventually remove this */

#include "transmission.h"

#include "crypto-utils.h" /* tr_rand_buffer() */
#include "crypto.h" /* tr_ssha1_matches() */
#include "error.h"
#include "fdlimit.h"
#include "log.h"
#include "net.h"
#include "platform.h" /* tr_getWebClientDir() */
#include "quark.h"
#include "rpc-server.h"
#include "rpcimpl.h"
#include "session-id.h"
#include "session.h"
#include "tr-assert.h"
#include "trevent.h"
#include "utils.h"
#include "variant.h"
#include "web-utils.h"
#include "web.h"

using namespace std::literals;

/* session-id is used to make cross-site request forgery attacks difficult.
 * Don't disable this feature unless you really know what you're doing!
 * http://en.wikipedia.org/wiki/Cross-site_request_forgery
 * http://shiflett.org/articles/cross-site-request-forgeries
 * http://www.webappsec.org/lists/websecurity/archive/2008-04/msg00037.html */
#define REQUIRE_SESSION_ID

static char constexpr MyName[] = "RPC Server";

#define MY_REALM "Transmission"

#define dbgmsg(...) tr_logAddDeepNamed(MyName, __VA_ARGS__)

static int constexpr DeflateLevel = 6; // medium / default

/***
****
***/

static char const* get_current_session_id(tr_rpc_server* server)
{
    return tr_session_id_get_current(server->session->session_id);
}

/**
***
**/

static void send_simple_response(struct evhttp_request* req, int code, char const* text)
{
    char const* code_text = tr_webGetResponseStr(code);
    struct evbuffer* body = evbuffer_new();

    evbuffer_add_printf(body, "<h1>%d: %s</h1>", code, code_text);

    if (text != nullptr)
    {
        evbuffer_add_printf(body, "%s", text);
    }

    evhttp_send_reply(req, code, code_text, body);

    evbuffer_free(body);
}

/***
****
***/

static char const* mimetype_guess(char const* path)
{
    struct
    {
        char const* suffix;
        char const* mime_type;
    } const types[] = {
        /* these are the ones we need for serving the web client's files... */
        { "css", "text/css" },
        { "gif", "image/gif" },
        { "html", "text/html" },
        { "ico", "image/vnd.microsoft.icon" },
        { "js", "application/javascript" },
        { "png", "image/png" },
        { "svg", "image/svg+xml" },
    };
    char const* dot = strrchr(path, '.');

    for (unsigned int i = 0; dot != nullptr && i < TR_N_ELEMENTS(types); ++i)
    {
        if (strcmp(dot + 1, types[i].suffix) == 0)
        {
            return types[i].mime_type;
        }
    }

    return "application/octet-stream";
}

static void add_response(struct evhttp_request* req, tr_rpc_server* server, struct evbuffer* out, struct evbuffer* content)
{
    char const* key = "Accept-Encoding";
    char const* encoding = evhttp_find_header(req->input_headers, key);
    bool const do_compress = encoding != nullptr && strstr(encoding, "gzip") != nullptr;

    if (!do_compress)
    {
        evbuffer_add_buffer(out, content);
    }
    else
    {
        auto const* const content_ptr = evbuffer_pullup(content, -1);
        size_t const content_len = evbuffer_get_length(content);
        auto const max_compressed_len = libdeflate_deflate_compress_bound(server->compressor.get(), content_len);

        struct evbuffer_iovec iovec[1];
        evbuffer_reserve_space(out, std::max(content_len, max_compressed_len), iovec, 1);

        auto const compressed_len = libdeflate_gzip_compress(
            server->compressor.get(),
            content_ptr,
            content_len,
            iovec[0].iov_base,
            iovec[0].iov_len);
        if (0 < compressed_len && compressed_len < content_len)
        {
            iovec[0].iov_len = compressed_len;
            evhttp_add_header(req->output_headers, "Content-Encoding", "gzip");
        }
        else
        {
            std::copy_n(content_ptr, content_len, static_cast<char*>(iovec[0].iov_base));
            iovec[0].iov_len = content_len;
        }

        evbuffer_commit_space(out, iovec, 1);
    }
}

static void add_time_header(struct evkeyvalq* headers, char const* key, time_t value)
{
    char buf[128];
    struct tm tm;
    /* According to RFC 2616 this must follow RFC 1123's date format,
       so use gmtime instead of localtime... */
    tr_gmtime_r(&value, &tm);
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    evhttp_add_header(headers, key, buf);
}

static void evbuffer_ref_cleanup_tr_free(void const* /*data*/, size_t /*datalen*/, void* extra)
{
    tr_free(extra);
}

static void serve_file(struct evhttp_request* req, tr_rpc_server* server, char const* filename)
{
    if (req->type != EVHTTP_REQ_GET)
    {
        evhttp_add_header(req->output_headers, "Allow", "GET");
        send_simple_response(req, 405, nullptr);
    }
    else
    {
        auto file_len = size_t{};
        tr_error* error = nullptr;
        void* const file = tr_loadFile(filename, &file_len, &error);

        if (file == nullptr)
        {
            auto const tmp = tr_strvJoin(filename, " ("sv, error->message, ")"sv);
            send_simple_response(req, HTTP_NOTFOUND, tmp.c_str());
            tr_error_free(error);
        }
        else
        {
            auto const now = tr_time();

            auto* const content = evbuffer_new();
            evbuffer_add_reference(content, file, file_len, evbuffer_ref_cleanup_tr_free, file);

            auto* const out = evbuffer_new();
            evhttp_add_header(req->output_headers, "Content-Type", mimetype_guess(filename));
            add_time_header(req->output_headers, "Date", now);
            add_time_header(req->output_headers, "Expires", now + (24 * 60 * 60));
            add_response(req, server, out, content);
            evhttp_send_reply(req, HTTP_OK, "OK", out);

            evbuffer_free(out);
            evbuffer_free(content);
        }
    }
}

static void handle_web_client(struct evhttp_request* req, tr_rpc_server* server)
{
    char const* webClientDir = tr_getWebClientDir(server->session);

    if (tr_str_is_empty(webClientDir))
    {
        send_simple_response(
            req,
            HTTP_NOTFOUND,
            "<p>Couldn't find Transmission's web interface files!</p>"
            "<p>Users: to tell Transmission where to look, "
            "set the TRANSMISSION_WEB_HOME environment "
            "variable to the folder where the web interface's "
            "index.html is located.</p>"
            "<p>Package Builders: to set a custom default at compile time, "
            "#define PACKAGE_DATA_DIR in libtransmission/platform.c "
            "or tweak tr_getClutchDir() by hand.</p>");
    }
    else
    {
        // TODO: string_view
        char* const subpath = tr_strdup(req->uri + std::size(server->url) + 4);
        if (char* pch = strchr(subpath, '?'); pch != nullptr)
        {
            *pch = '\0';
        }

        if (strstr(subpath, "..") != nullptr)
        {
            send_simple_response(req, HTTP_NOTFOUND, "<p>Tsk, tsk.</p>");
        }
        else
        {
            auto const filename = tr_strvJoin(
                webClientDir,
                TR_PATH_DELIMITER_STR,
                tr_str_is_empty(subpath) ? "index.html" : subpath);
            serve_file(req, server, filename.c_str());
        }

        tr_free(subpath);
    }
}

struct rpc_response_data
{
    struct evhttp_request* req;
    tr_rpc_server* server;
};

static void rpc_response_func(tr_session* /*session*/, tr_variant* response, void* user_data)
{
    auto* data = static_cast<struct rpc_response_data*>(user_data);
    struct evbuffer* response_buf = tr_variantToBuf(response, TR_VARIANT_FMT_JSON_LEAN);
    struct evbuffer* buf = evbuffer_new();

    add_response(data->req, data->server, buf, response_buf);
    evhttp_add_header(data->req->output_headers, "Content-Type", "application/json; charset=UTF-8");
    evhttp_send_reply(data->req, HTTP_OK, "OK", buf);

    evbuffer_free(buf);
    evbuffer_free(response_buf);
    tr_free(data);
}

static void handle_rpc_from_json(struct evhttp_request* req, tr_rpc_server* server, std::string_view json)
{
    auto top = tr_variant{};
    auto const have_content = tr_variantFromBuf(&top, TR_VARIANT_PARSE_JSON | TR_VARIANT_PARSE_INPLACE, json);

    auto* const data = tr_new0(struct rpc_response_data, 1);
    data->req = req;
    data->server = server;

    tr_rpc_request_exec_json(server->session, have_content ? &top : nullptr, rpc_response_func, data);

    if (have_content)
    {
        tr_variantFree(&top);
    }
}

static void handle_rpc(struct evhttp_request* req, tr_rpc_server* server)
{
    if (req->type == EVHTTP_REQ_POST)
    {
        auto json = std::string_view{ reinterpret_cast<char const*>(evbuffer_pullup(req->input_buffer, -1)),
                                      evbuffer_get_length(req->input_buffer) };
        handle_rpc_from_json(req, server, json);
        return;
    }

    if (req->type == EVHTTP_REQ_GET)
    {
        char const* q = strchr(req->uri, '?');

        if (q != nullptr)
        {
            auto* const data = tr_new0(struct rpc_response_data, 1);
            data->req = req;
            data->server = server;
            tr_rpc_request_exec_uri(server->session, q + 1, rpc_response_func, data);
            return;
        }
    }

    send_simple_response(req, 405, nullptr);
}

static bool isAddressAllowed(tr_rpc_server const* server, char const* address)
{
    auto const& src = server->whitelist;

    return !server->isWhitelistEnabled ||
        std::any_of(std::begin(src), std::end(src), [&address](auto const& s) { return tr_wildmat(address, s.c_str()); });
}

static bool isIPAddressWithOptionalPort(char const* host)
{
    struct sockaddr_storage address;
    int address_len = sizeof(address);

    /* TODO: move to net.{c,h} */
    return evutil_parse_sockaddr_port(host, (struct sockaddr*)&address, &address_len) != -1;
}

static bool isHostnameAllowed(tr_rpc_server const* server, struct evhttp_request* req)
{
    /* If password auth is enabled, any hostname is permitted. */
    if (server->isPasswordEnabled)
    {
        return true;
    }

    /* If whitelist is disabled, no restrictions. */
    if (!server->isHostWhitelistEnabled)
    {
        return true;
    }

    char const* const host = evhttp_find_header(req->input_headers, "Host");

    /* No host header, invalid request. */
    if (host == nullptr)
    {
        return false;
    }

    /* IP address is always acceptable. */
    if (isIPAddressWithOptionalPort(host))
    {
        return true;
    }

    /* Host header might include the port. */
    auto const hostname = std::string(host, strcspn(host, ":"));

    /* localhost is always acceptable. */
    if (hostname == "localhost" || hostname == "localhost.")
    {
        return true;
    }

    auto const& src = server->hostWhitelist;
    return std::any_of(
        std::begin(src),
        std::end(src),
        [&hostname](auto const& str) { return tr_wildmat(hostname.c_str(), str.c_str()); });
}

static bool test_session_id(tr_rpc_server* server, evhttp_request const* req)
{
    char const* ours = get_current_session_id(server);
    char const* theirs = evhttp_find_header(req->input_headers, TR_RPC_SESSION_ID_HEADER);
    bool const success = theirs != nullptr && strcmp(theirs, ours) == 0;
    return success;
}

static bool isAuthorized(tr_rpc_server const* server, char const* auth_header)
{
    if (!server->isPasswordEnabled)
    {
        return true;
    }

    // https://datatracker.ietf.org/doc/html/rfc7617
    // `Basic ${base64(username)}:${base64(password)}`

    auto constexpr Prefix = "Basic "sv;
    auto auth = std::string_view{ auth_header != nullptr ? auth_header : "" };
    if (!tr_strvStartsWith(auth, Prefix))
    {
        return false;
    }

    auth.remove_prefix(std::size(Prefix));
    auto const decoded_str = tr_base64_decode(auth);
    auto decoded = std::string_view{ decoded_str };
    auto const username = tr_strvSep(&decoded, ':');
    auto const password = decoded;
    return server->username == username && tr_ssha1_matches(server->salted_password, password);
}

static void handle_request(struct evhttp_request* req, void* arg)
{
    auto* server = static_cast<tr_rpc_server*>(arg);

    if (req != nullptr && req->evcon != nullptr)
    {
        evhttp_add_header(req->output_headers, "Server", MY_REALM);

        if (server->isAntiBruteForceEnabled && server->loginattempts >= server->antiBruteForceThreshold)
        {
            send_simple_response(req, 403, "<p>Too many unsuccessful login attempts. Please restart transmission-daemon.</p>");
            return;
        }

        if (!isAddressAllowed(server, req->remote_host))
        {
            send_simple_response(
                req,
                403,
                "<p>Unauthorized IP Address.</p>"
                "<p>Either disable the IP address whitelist or add your address to it.</p>"
                "<p>If you're editing settings.json, see the 'rpc-whitelist' and 'rpc-whitelist-enabled' entries.</p>"
                "<p>If you're still using ACLs, use a whitelist instead. See the transmission-daemon manpage for details.</p>");
            return;
        }

        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");

        if (req->type == EVHTTP_REQ_OPTIONS)
        {
            char const* headers = evhttp_find_header(req->input_headers, "Access-Control-Request-Headers");
            if (headers != nullptr)
            {
                evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", headers);
            }

            evhttp_add_header(req->output_headers, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            send_simple_response(req, 200, "");
            return;
        }

        if (!isAuthorized(server, evhttp_find_header(req->input_headers, "Authorization")))
        {
            evhttp_add_header(req->output_headers, "WWW-Authenticate", "Basic realm=\"" MY_REALM "\"");
            if (server->isAntiBruteForceEnabled)
            {
                ++server->loginattempts;
            }

            auto const unauthuser = tr_strvJoin(
                "<p>Unauthorized User. "sv,
                std::to_string(server->loginattempts),
                " unsuccessful login attempts.</p>"sv);
            send_simple_response(req, 401, unauthuser.c_str());
            return;
        }

        server->loginattempts = 0;

        auto uri = std::string_view{ req->uri };
        auto const location = tr_strvStartsWith(uri, server->url) ? uri.substr(std::size(server->url)) : ""sv;

        if (std::empty(location) || location == "web"sv)
        {
            auto const new_location = tr_strvJoin(server->url, "web/");
            evhttp_add_header(req->output_headers, "Location", new_location.c_str());
            send_simple_response(req, HTTP_MOVEPERM, nullptr);
        }
        else if (tr_strvStartsWith(location, "web/"sv))
        {
            handle_web_client(req, server);
        }
        else if (!isHostnameAllowed(server, req))
        {
            char const* const tmp =
                "<p>Transmission received your request, but the hostname was unrecognized.</p>"
                "<p>To fix this, choose one of the following options:"
                "<ul>"
                "<li>Enable password authentication, then any hostname is allowed.</li>"
                "<li>Add the hostname you want to use to the whitelist in settings.</li>"
                "</ul></p>"
                "<p>If you're editing settings.json, see the 'rpc-host-whitelist' and 'rpc-host-whitelist-enabled' entries.</p>"
                "<p>This requirement has been added to help prevent "
                "<a href=\"https://en.wikipedia.org/wiki/DNS_rebinding\">DNS Rebinding</a> "
                "attacks.</p>";
            send_simple_response(req, 421, tmp);
        }
#ifdef REQUIRE_SESSION_ID
        else if (!test_session_id(server, req))
        {
            char const* sessionId = get_current_session_id(server);
            auto const tmp = tr_strvJoin(
                "<p>Your request had an invalid session-id header.</p>"
                "<p>To fix this, follow these steps:"
                "<ol><li> When reading a response, get its X-Transmission-Session-Id header and remember it"
                "<li> Add the updated header to your outgoing requests"
                "<li> When you get this 409 error message, resend your request with the updated header"
                "</ol></p>"
                "<p>This requirement has been added to help prevent "
                "<a href=\"https://en.wikipedia.org/wiki/Cross-site_request_forgery\">CSRF</a> "
                "attacks.</p>"
                "<p><code>" TR_RPC_SESSION_ID_HEADER,
                ": "sv,
                sessionId,
                "</code></p>");
            evhttp_add_header(req->output_headers, TR_RPC_SESSION_ID_HEADER, sessionId);
            evhttp_add_header(req->output_headers, "Access-Control-Expose-Headers", TR_RPC_SESSION_ID_HEADER);
            send_simple_response(req, 409, tmp.c_str());
        }
#endif
        else if (tr_strvStartsWith(location, "rpc"sv))
        {
            handle_rpc(req, server);
        }
        else
        {
            send_simple_response(req, HTTP_NOTFOUND, req->uri);
        }
    }
}

static auto constexpr ServerStartRetryCount = int{ 10 };
static auto constexpr ServerStartRetryDelayIncrement = int{ 5 };
static auto constexpr ServerStartRetryDelayStep = int{ 3 };
static auto constexpr ServerStartRetryMaxDelay = int{ 60 };

static void startServer(void* vserver);

static void rpc_server_on_start_retry(evutil_socket_t /*fd*/, short /*type*/, void* context)
{
    startServer(context);
}

static int rpc_server_start_retry(tr_rpc_server* server)
{
    int retry_delay = (server->start_retry_counter / ServerStartRetryDelayStep + 1) * ServerStartRetryDelayIncrement;
    retry_delay = std::min(retry_delay, int{ ServerStartRetryMaxDelay });

    if (server->start_retry_timer == nullptr)
    {
        server->start_retry_timer = evtimer_new(server->session->event_base, rpc_server_on_start_retry, server);
    }

    tr_timerAdd(server->start_retry_timer, retry_delay, 0);
    ++server->start_retry_counter;

    return retry_delay;
}

static void rpc_server_start_retry_cancel(tr_rpc_server* server)
{
    if (server->start_retry_timer != nullptr)
    {
        event_free(server->start_retry_timer);
        server->start_retry_timer = nullptr;
    }

    server->start_retry_counter = 0;
}

static void startServer(void* vserver)
{
    auto* server = static_cast<tr_rpc_server*>(vserver);

    if (server->httpd != nullptr)
    {
        return;
    }

    struct evhttp* httpd = evhttp_new(server->session->event_base);
    evhttp_set_allowed_methods(httpd, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_OPTIONS);

    char const* address = tr_rpcGetBindAddress(server);

    tr_port const port = server->port;

    if (evhttp_bind_socket(httpd, address, port) == -1)
    {
        evhttp_free(httpd);

        if (server->start_retry_counter < ServerStartRetryCount)
        {
            int const retry_delay = rpc_server_start_retry(server);

            tr_logAddNamedDbg(MyName, "Unable to bind to %s:%d, retrying in %d seconds", address, port, retry_delay);
            return;
        }

        tr_logAddNamedError(
            MyName,
            "Unable to bind to %s:%d after %d attempts, giving up",
            address,
            port,
            ServerStartRetryCount);
    }
    else
    {
        evhttp_set_gencb(httpd, handle_request, server);
        server->httpd = httpd;

        tr_logAddNamedDbg(MyName, "Started listening on %s:%d", address, port);
    }

    rpc_server_start_retry_cancel(server);
}

static void stopServer(tr_rpc_server* server)
{
    TR_ASSERT(tr_amInEventThread(server->session));

    rpc_server_start_retry_cancel(server);

    struct evhttp* httpd = server->httpd;

    if (httpd == nullptr)
    {
        return;
    }

    char const* address = tr_rpcGetBindAddress(server);
    int const port = server->port;

    server->httpd = nullptr;
    evhttp_free(httpd);

    tr_logAddNamedDbg(MyName, "Stopped listening on %s:%d", address, port);
}

static void onEnabledChanged(void* vserver)
{
    auto* server = static_cast<tr_rpc_server*>(vserver);

    if (!server->isEnabled)
    {
        stopServer(server);
    }
    else
    {
        startServer(server);
    }
}

void tr_rpcSetEnabled(tr_rpc_server* server, bool isEnabled)
{
    server->isEnabled = isEnabled;

    tr_runInEventThread(server->session, onEnabledChanged, server);
}

bool tr_rpcIsEnabled(tr_rpc_server const* server)
{
    return server->isEnabled;
}

static void restartServer(void* vserver)
{
    auto* server = static_cast<tr_rpc_server*>(vserver);

    if (server->isEnabled)
    {
        stopServer(server);
        startServer(server);
    }
}

void tr_rpcSetPort(tr_rpc_server* server, tr_port port)
{
    TR_ASSERT(server != nullptr);

    if (server->port != port)
    {
        server->port = port;

        if (server->isEnabled)
        {
            tr_runInEventThread(server->session, restartServer, server);
        }
    }
}

tr_port tr_rpcGetPort(tr_rpc_server const* server)
{
    return server->port;
}

void tr_rpcSetUrl(tr_rpc_server* server, std::string_view url)
{
    server->url = url;
    dbgmsg("setting our URL to [%s]", server->url.c_str());
}

std::string const& tr_rpcGetUrl(tr_rpc_server const* server)
{
    return server->url;
}

static auto parseWhitelist(std::string_view whitelist)
{
    auto list = std::list<std::string>{};

    while (!std::empty(whitelist))
    {
        auto const pos = whitelist.find_first_of(" ,;"sv);
        auto const token = tr_strvStrip(whitelist.substr(0, pos));
        list.emplace_back(token);
        whitelist = pos == std::string_view::npos ? ""sv : whitelist.substr(pos + 1);

        if (token.find_first_of("+-"sv) != std::string_view::npos)
        {
            tr_logAddNamedInfo(
                MyName,
                "Adding address to whitelist: %" TR_PRIsv " (And it has a '+' or '-'!  Are you using an old ACL by mistake?)",
                TR_PRIsv_ARG(token));
        }
        else
        {
            tr_logAddNamedInfo(MyName, "Adding address to whitelist: %" TR_PRIsv, TR_PRIsv_ARG(token));
        }
    }

    return list;
}

static void tr_rpcSetHostWhitelist(tr_rpc_server* server, std::string_view whitelist)
{
    server->hostWhitelist = parseWhitelist(whitelist);
}

void tr_rpcSetWhitelist(tr_rpc_server* server, std::string_view whitelist)
{
    server->whitelistStr = whitelist;
    server->whitelist = parseWhitelist(whitelist);
}

std::string const& tr_rpcGetWhitelist(tr_rpc_server const* server)
{
    return server->whitelistStr;
}

void tr_rpcSetWhitelistEnabled(tr_rpc_server* server, bool isEnabled)
{
    server->isWhitelistEnabled = isEnabled;
}

bool tr_rpcGetWhitelistEnabled(tr_rpc_server const* server)
{
    return server->isWhitelistEnabled;
}

static void tr_rpcSetHostWhitelistEnabled(tr_rpc_server* server, bool isEnabled)
{
    server->isHostWhitelistEnabled = isEnabled;
}

/****
*****  PASSWORD
****/

void tr_rpcSetUsername(tr_rpc_server* server, std::string_view username)
{
    server->username = username;
    dbgmsg("setting our Username to [%s]", server->username.c_str());
}

std::string const& tr_rpcGetUsername(tr_rpc_server const* server)
{
    return server->username;
}

static bool isSalted(std::string_view password)
{
    return tr_ssha1_test(password);
}

void tr_rpcSetPassword(tr_rpc_server* server, std::string_view password)
{
    server->salted_password = isSalted(password) ? password : tr_ssha1(password);

    dbgmsg("setting our salted password to [%s]", server->salted_password.c_str());
}

std::string const& tr_rpcGetPassword(tr_rpc_server const* server)
{
    return server->salted_password;
}

void tr_rpcSetPasswordEnabled(tr_rpc_server* server, bool isEnabled)
{
    server->isPasswordEnabled = isEnabled;
    dbgmsg("setting 'password enabled' to %d", (int)isEnabled);
}

bool tr_rpcIsPasswordEnabled(tr_rpc_server const* server)
{
    return server->isPasswordEnabled;
}

char const* tr_rpcGetBindAddress(tr_rpc_server const* server)
{
    return tr_address_to_string(&server->bindAddress);
}

bool tr_rpcGetAntiBruteForceEnabled(tr_rpc_server const* server)
{
    return server->isAntiBruteForceEnabled;
}

void tr_rpcSetAntiBruteForceEnabled(tr_rpc_server* server, bool isEnabled)
{
    server->isAntiBruteForceEnabled = isEnabled;
    if (!isEnabled)
    {
        server->loginattempts = 0;
    }
}

int tr_rpcGetAntiBruteForceThreshold(tr_rpc_server const* server)
{
    return server->antiBruteForceThreshold;
}

void tr_rpcSetAntiBruteForceThreshold(tr_rpc_server* server, int badRequests)
{
    server->antiBruteForceThreshold = badRequests;
}

/****
*****  LIFE CYCLE
****/

static void missing_settings_key(tr_quark const q)
{
    char const* str = tr_quark_get_string(q);
    tr_logAddNamedError(MyName, _("Couldn't find settings key \"%s\""), str);
}

tr_rpc_server::tr_rpc_server(tr_session* session_in, tr_variant* settings)
    : compressor{ libdeflate_alloc_compressor(DeflateLevel), libdeflate_free_compressor }
    , session{ session_in }
{
    auto address = tr_address{};
    auto boolVal = bool{};
    auto i = int64_t{};
    auto sv = std::string_view{};

    auto key = TR_KEY_rpc_enabled;

    if (!tr_variantDictFindBool(settings, key, &boolVal))
    {
        missing_settings_key(key);
    }
    else
    {
        this->isEnabled = boolVal;
    }

    key = TR_KEY_rpc_port;

    if (!tr_variantDictFindInt(settings, key, &i))
    {
        missing_settings_key(key);
    }
    else
    {
        this->port = (tr_port)i;
    }

    key = TR_KEY_rpc_url;

    if (!tr_variantDictFindStrView(settings, key, &sv))
    {
        missing_settings_key(key);
    }
    else if (std::empty(sv) || sv.back() != '/')
    {
        this->url = tr_strvJoin(sv, "/"sv);
    }
    else
    {
        this->url = sv;
    }

    key = TR_KEY_rpc_whitelist_enabled;

    if (!tr_variantDictFindBool(settings, key, &boolVal))
    {
        missing_settings_key(key);
    }
    else
    {
        tr_rpcSetWhitelistEnabled(this, boolVal);
    }

    key = TR_KEY_rpc_host_whitelist_enabled;

    if (!tr_variantDictFindBool(settings, key, &boolVal))
    {
        missing_settings_key(key);
    }
    else
    {
        tr_rpcSetHostWhitelistEnabled(this, boolVal);
    }

    key = TR_KEY_rpc_host_whitelist;

    if (!tr_variantDictFindStrView(settings, key, &sv) && !std::empty(sv))
    {
        missing_settings_key(key);
    }
    else
    {
        tr_rpcSetHostWhitelist(this, sv);
    }

    key = TR_KEY_rpc_authentication_required;

    if (!tr_variantDictFindBool(settings, key, &boolVal))
    {
        missing_settings_key(key);
    }
    else
    {
        tr_rpcSetPasswordEnabled(this, boolVal);
    }

    key = TR_KEY_rpc_whitelist;

    if (!tr_variantDictFindStrView(settings, key, &sv) && !std::empty(sv))
    {
        missing_settings_key(key);
    }
    else
    {
        tr_rpcSetWhitelist(this, sv);
    }

    key = TR_KEY_rpc_username;

    if (!tr_variantDictFindStrView(settings, key, &sv))
    {
        missing_settings_key(key);
    }
    else
    {
        tr_rpcSetUsername(this, sv);
    }

    key = TR_KEY_rpc_password;

    if (!tr_variantDictFindStrView(settings, key, &sv))
    {
        missing_settings_key(key);
    }
    else
    {
        tr_rpcSetPassword(this, sv);
    }

    key = TR_KEY_anti_brute_force_enabled;

    if (!tr_variantDictFindBool(settings, key, &boolVal))
    {
        missing_settings_key(key);
    }
    else
    {
        tr_rpcSetAntiBruteForceEnabled(this, boolVal);
    }

    key = TR_KEY_anti_brute_force_threshold;

    if (!tr_variantDictFindInt(settings, key, &i))
    {
        missing_settings_key(key);
    }
    else
    {
        tr_rpcSetAntiBruteForceThreshold(this, i);
    }

    key = TR_KEY_rpc_bind_address;

    if (!tr_variantDictFindStrView(settings, key, &sv))
    {
        missing_settings_key(key);
        address = tr_inaddr_any;
    }
    else
    {
        if (!tr_address_from_string(&address, std::string{ sv }.c_str()))
        {
            tr_logAddNamedError(MyName, _("%" TR_PRIsv " is not a valid address"), TR_PRIsv_ARG(sv));
            address = tr_inaddr_any;
        }
        else if (address.type != TR_AF_INET && address.type != TR_AF_INET6)
        {
            tr_logAddNamedError(
                MyName,
                _("%" TR_PRIsv " is not an IPv4 or IPv6 address. RPC listeners must be IPv4 or IPv6"),
                TR_PRIsv_ARG(sv));
            address = tr_inaddr_any;
        }
    }

    this->bindAddress = address;

    if (this->isEnabled)
    {
        tr_logAddNamedInfo(
            MyName,
            _("Serving RPC and Web requests on %s:%d%s"),
            tr_rpcGetBindAddress(this),
            (int)this->port,
            this->url.c_str());
        tr_runInEventThread(session, startServer, this);

        if (this->isWhitelistEnabled)
        {
            tr_logAddNamedInfo(MyName, "%s", _("Whitelist enabled"));
        }

        if (this->isPasswordEnabled)
        {
            tr_logAddNamedInfo(MyName, "%s", _("Password required"));
        }
    }

    char const* webClientDir = tr_getWebClientDir(this->session);
    if (!tr_str_is_empty(webClientDir))
    {
        tr_logAddNamedInfo(MyName, _("Serving RPC and Web requests from directory '%s'"), webClientDir);
    }
}

tr_rpc_server::~tr_rpc_server()
{
    TR_ASSERT(tr_amInEventThread(this->session));

    stopServer(this);
}
