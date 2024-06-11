/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

namespace libyang {
enum class DataFormat;
}

namespace sysrepo {
class Session;
class Subscription;
}

#include <vector>
#include "http/EventStream.h"

namespace rousette::restconf {

/** @brief Subscribes to NETCONF notifications and sends them via HTTP/2 Event stream. */
class NotificationStream : public rousette::http::EventStream {
    rousette::http::EventStream::Signal m_notificationSignal;
    std::vector<sysrepo::Subscription> m_notifSubs;

public:
    NotificationStream(const nghttp2::asio_http2::server::request& req, const nghttp2::asio_http2::server::response& res, sysrepo::Session sess, libyang::DataFormat dataFormat);
    void activate();
};
}
