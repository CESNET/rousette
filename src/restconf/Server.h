/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#pragma once
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Subscription.hpp>
#include "auth/Nacm.h"
#include "http/EventStream.h"
#include "restconf/SubscribedNotifications.h"

namespace nghttp2::asio_http2::server {
class http2;
}

namespace rousette {
namespace sr {
class OpticalEvents;
}

/** @short RESTCONF protocol */
namespace restconf {

std::optional<std::string> as_subtree_path(const std::string& path);

/** @short A RESTCONF-ish server */
class Server {
public:
    explicit Server(sysrepo::Connection conn, const std::string& address, const std::string& port, const std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
    ~Server();

private:
    sysrepo::Session m_monitoringSession;
    std::optional<sysrepo::Subscription> m_monitoringOperSub;
    auth::Nacm nacm;
    SubscribedNotifications m_subscribedNotifications;
    std::unique_ptr<nghttp2::asio_http2::server::http2> server;
    std::unique_ptr<sr::OpticalEvents> dwdmEvents;
    using JsonDiffSignal = boost::signals2::signal<void(const std::string& json)>;
    JsonDiffSignal opticsChange;
};
}
}
