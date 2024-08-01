/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#include <spdlog/spdlog.h>
#include <string>
#include "NacmIdentities.h"
#include "http/utils.hpp"
#include "auth/Nacm.h"
#include "auth/PAM.h"
#include "auth/auth.h"

namespace rousette::auth {

void authorizeRequest(const Nacm& nacm, sysrepo::Session& sess, const nghttp2::asio_http2::server::request& req)
{
    std::string nacmUser;
    if (auto authHeader = http::getHeaderValue(req.header(), "authorization")) {
        nacmUser = rousette::auth::authenticate_pam(*authHeader, http::peer_from_request(req));
    } else {
        nacmUser = ANONYMOUS_USER;
    }

    if (!nacm.authorize(sess, nacmUser)) {
        throw Error{"Access denied."};
    }
}

void processAuthError(const nghttp2::asio_http2::server::request& req, const nghttp2::asio_http2::server::response& res, const auth::Error& error, const std::function<void()>& errorResponseCb)
{
    if (error.delay) {
        spdlog::info("{}: Authentication failed (delay {}us): {}", http::peer_from_request(req), error.delay->count(), error.what());
        auto timer = std::make_shared<boost::asio::steady_timer>(res.io_service(), *error.delay);
        res.on_close([timer](uint32_t code) {
            (void)code;
            // Signal that the timer should be cancelled, so that its completion callback knows that
            // a connection is gone already.
            timer->cancel();
        });
        timer->async_wait([timer, errorResponseCb](const boost::system::error_code& ec) {
            if (ec.failed()) {
                // The `req` request has been already freed at this point and it's a dangling reference.
                // There's nothing else to do at this point.
            } else {
                errorResponseCb();
            }
        });
    } else {
        spdlog::error("{}: Authentication failed: {}", http::peer_from_request(req), error.what());
        errorResponseCb();
    }
}
}
