#pragma once

// Included by monolith_proxy.cpp after URL configuration and to_wide().
// Each thread owns its session: a slow tool request cannot hold up health polling.
struct MonolithHttpConnection
{
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    std::string endpoint;
    ~MonolithHttpConnection() { reset(); }
    void reset()
    {
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
        connection = nullptr;
        session = nullptr;
        endpoint.clear();
    }
    bool ensure()
    {
        if (endpoint != g_monolith_url) reset();
        if (connection) return true;
        session = WinHttpOpen(L"MonolithProxy/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        connection = WinHttpConnect(session, to_wide(g_monolith_host).c_str(),
            static_cast<INTERNET_PORT>(g_monolith_port), 0);
        if (!connection) { const DWORD error = GetLastError(); reset(); SetLastError(error); return false; }
        endpoint = g_monolith_url;
        return true;
    }
};

static thread_local MonolithHttpConnection g_http_connection;
static thread_local std::string g_http_error;

// One send attempt only. On failure discard the connection for the NEXT request;
// never replay a POST whose server-side effects may already have occurred.
static std::string monolith_http_request(const wchar_t* method, const std::string& path,
    const std::string& body, double timeout_sec, DWORD& status)
{
    g_http_error.clear();
    status = 0;
    auto fail = [&](const char* phase, DWORD error) -> std::string
    {
        g_http_error = std::string(phase) + " failed (WinHTTP " + std::to_string(error) + ")";
        g_http_connection.reset();
        return {};
    };
    if (!g_http_connection.ensure()) return fail("connection setup", GetLastError());
    const DWORD flags = g_monolith_url.rfind("https://", 0) == 0 ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(g_http_connection.connection, method,
        to_wide(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) return fail("request setup", GetLastError());
    auto request_fail = [&](const char* phase) -> std::string
    {
        const DWORD error = GetLastError();
        WinHttpCloseHandle(request);
        return fail(phase, error);
    };
    const int timeout_ms = static_cast<int>(timeout_sec * 1000);
    if (!WinHttpSetTimeouts(request, timeout_ms, timeout_ms, timeout_ms, timeout_ms))
        return request_fail("timeout setup");
    // Header reception has a separate WinHTTP timeout (otherwise defaults to
    // 90 seconds). This is still not a whole-request cancellation deadline.
    DWORD header_timeout_ms = static_cast<DWORD>(timeout_ms);
    if (!WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
        &header_timeout_ms, sizeof(header_timeout_ms)))
        return request_fail("response timeout setup");
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
        return request_fail("redirect policy setup");
    if (!WinHttpSendRequest(request, L"Content-Type: application/json", static_cast<DWORD>(-1),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0))
        return request_fail("send");
    if (!WinHttpReceiveResponse(request, nullptr)) return request_fail("receive headers");
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
        return request_fail("read status");
    // Fail promptly without forwarding HTML/errors or following redirects.
    if (status < 200 || status >= 300)
    {
        WinHttpCloseHandle(request);
        g_http_connection.reset();
        g_http_error = "HTTP status " + std::to_string(status);
        return {};
    }
    std::optional<unsigned long long> expected_length;
    wchar_t length_text[64] = {};
    DWORD length_size = sizeof(length_text);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
        WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size, WINHTTP_NO_HEADER_INDEX))
    {
        try { expected_length = std::stoull(length_text); }
        catch (...) { SetLastError(ERROR_INVALID_DATA); return request_fail("read content length"); }
    }
    std::string response;
    char buffer[16384];
    for (;;)
    {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read))
            return request_fail("read body");
        if (read == 0) break;
        response.append(buffer, read);
    }
    WinHttpCloseHandle(request);
    if (expected_length && response.size() != *expected_length)
        return fail("incomplete body", ERROR_HANDLE_EOF);
    return response;
}

static std::string post_monolith(const std::string& body, double timeout_sec = TIMEOUT)
{
    DWORD status = 0;
    std::string response = monolith_http_request(L"POST", g_monolith_path_mcp, body, timeout_sec, status);
    if (!g_http_error.empty()) return {};
    const json sent = json::parse(body, nullptr, false);
    // Notifications have no response. Consume their HTTP body but don't emit it.
    if (sent.is_object() && !sent.contains("id")) return {};
    const json received = json::parse(response, nullptr, false);
    if (!received.is_object() || !received.contains("jsonrpc") || received["jsonrpc"] != "2.0" ||
        !received.contains("id") || !sent.is_object() || !sent.contains("id") || received["id"] != sent["id"] ||
        received.contains("result") == received.contains("error"))
    {
        g_http_error = "Invalid or mismatched JSON-RPC response";
        g_http_connection.reset();
        return {};
    }
    const bool tool_request = sent.value("method", "") == "tools/call" || sent.value("method", "") == "tools/list";
    if ((received.contains("result") && tool_request && !received["result"].is_object()) ||
        (received.contains("error") && (!received["error"].is_object() ||
            !received["error"].contains("code") || !received["error"]["code"].is_number_integer() ||
            !received["error"].contains("message") || !received["error"]["message"].is_string())))
    {
        g_http_error = "Invalid JSON-RPC result or error object";
        g_http_connection.reset();
        return {};
    }
    return response;
}

static bool check_monolith_up()
{
    DWORD status = 0;
    monolith_http_request(L"GET", g_monolith_path_health, {}, 3.0, status);
    return g_http_error.empty() && status == 200;
}
