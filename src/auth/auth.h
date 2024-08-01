/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#pragma once

#include <nghttp2/asio_http2_server.h>

namespace sysrepo {
class Session;
}

namespace rousette::auth {
class Error : public std::runtime_error {
public:
    std::optional<std::chrono::microseconds> delay;

    Error(const std::string& message, std::optional<std::chrono::microseconds> delay=std::nullopt)
        : std::runtime_error{message}
        , delay(delay)
    {
    }
};

class Nacm;

void authorizeRequest(const Nacm& nacm, sysrepo::Session& sess, const nghttp2::asio_http2::server::request& req);
void processAuthError(const nghttp2::asio_http2::server::request& req, const nghttp2::asio_http2::server::response& res, const auth::Error& error, const std::function<void()>& errorResponseCb);
}
