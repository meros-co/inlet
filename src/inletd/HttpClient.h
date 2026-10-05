// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

#include <string>

namespace inlet
{

struct HttpResult
{
    bool connected = false;   ///< an answer came back at all
    int status = 0;
    std::string body;
};

/** One plain-HTTP request to an absolute "http://host:port/path" URL.
    HTTPS is not spoken: NMOS registries and peers on a media network are
    plain HTTP (IS-04's api_proto=http). */
HttpResult httpRequest(const std::string& method, const std::string& url, const std::string& body = {},
                       const std::string& contentType = "application/json", int timeoutMs = 3000);

} // namespace inlet
