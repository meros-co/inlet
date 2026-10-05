// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "HttpClient.h"

#include <httplib.h>

namespace inlet
{

HttpResult httpRequest(const std::string& method, const std::string& url, const std::string& body,
                       const std::string& contentType, int timeoutMs)
{
    HttpResult r;
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0)
        return r;
    const auto slash = url.find('/', scheme.size());
    const std::string authority = url.substr(scheme.size(), slash == std::string::npos ? std::string::npos : slash - scheme.size());
    const std::string path = slash == std::string::npos ? "/" : url.substr(slash);
    std::string host = authority;
    int port = 80;
    if (const auto colon = authority.rfind(':'); colon != std::string::npos)
    {
        host = authority.substr(0, colon);
        port = std::atoi(authority.c_str() + colon + 1);
    }

    httplib::Client cli(host, port);
    const auto sec = timeoutMs / 1000, usec = (timeoutMs % 1000) * 1000;
    cli.set_connection_timeout(sec, usec);
    cli.set_read_timeout(sec, usec);
    cli.set_write_timeout(sec, usec);

    httplib::Result res;
    if (method == "GET")
        res = cli.Get(path);
    else if (method == "POST")
        res = cli.Post(path, body, contentType);
    else if (method == "PUT")
        res = cli.Put(path, body, contentType);
    else if (method == "PATCH")
        res = cli.Patch(path, body, contentType);
    else if (method == "DELETE")
        res = cli.Delete(path);
    else
        return r;
    if (!res)
        return r;
    r.connected = true;
    r.status = res->status;
    r.body = res->body;
    return r;
}

} // namespace inlet
