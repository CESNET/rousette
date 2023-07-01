/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#pragma once
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Subscription.hpp>
#include "http/EventStream.h"

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

bool allow_anonymous_read_for(const std::string& path);

std::optional<libyang::DataNode> getData(sysrepo::Session sess, const std::string& path, const std::string& nacmUser);

/** @short A RESTCONF-ish server */
class Server {
public:
    explicit Server(sysrepo::Connection conn);
    ~Server();
    void listen_and_serve(const std::string& address, const std::string& port, bool asynchronous);
    void stop();
private:
    std::unique_ptr<nghttp2::asio_http2::server::http2> server;
    std::optional<sysrepo::Subscription> nacmSub;
    std::unique_ptr<sr::OpticalEvents> dwdmEvents;
    using JsonDiffSignal = boost::signals2::signal<void(const std::string& json)>;
    JsonDiffSignal opticsChange;
};
}
}
