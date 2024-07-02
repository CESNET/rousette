/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <optional>
#include <sysrepo-cpp/Subscription.hpp>
#include "http/EventStream.h"

namespace libyang {
enum class DataFormat;
}

namespace sysrepo {
class Session;
}

namespace rousette::restconf {

/** @brief Subscribes to NETCONF notifications and sends them via HTTP/2 Event stream.
 *
 * The class must be instantiated as a shared_ptr. Once the instance is created
 * and activate() is called, a self-referencing shared_ptr instance is bound to
 * HTTP-level callbacks which keep an instance around as long as the HTTP
 * request is alive. This magic is performed by the base class (EventStream).
 *
 * The notification signal is required to be passed from the outside context because
 * we have to pass already constructed signal to parent class.
 *
 * @see rousette::http::EventStream
 * */
class NotificationStream : public rousette::http::EventStream {
    std::shared_ptr<rousette::http::EventStream::Signal> m_notificationSignal;
    sysrepo::Session m_session;
    libyang::DataFormat m_dataFormat;
    std::optional<std::string> m_filter;
    std::optional<sysrepo::Subscription> m_notifSubs;

public:
    NotificationStream(
        const nghttp2::asio_http2::server::request& req,
        const nghttp2::asio_http2::server::response& res,
        std::shared_ptr<rousette::http::EventStream::Signal> signal,
        sysrepo::Session sess,
        libyang::DataFormat dataFormat,
        const std::optional<std::string>& filter);
    void activate();
};

void notificationStreamList(sysrepo::Session& session, std::optional<libyang::DataNode>& parent, const std::string& streamsPrefix);
libyang::DataNode replaceStreamLocations(const std::optional<std::string>& schemeAndHost, libyang::DataNode& node);
}
