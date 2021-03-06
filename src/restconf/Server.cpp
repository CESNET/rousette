/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <nghttp2/asio_http2_server.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Session.hpp>
#include "http/utils.hpp"
#include "restconf/Server.h"
#include "sr/OpticalEvents.h"

namespace {
constexpr auto notifPrefix = R"json({"ietf-restconf:notification":{"ietf-yang-push:push-update":{"datastore-contents":)json";
constexpr auto notifSuffix = R"json(}}})json";
}

namespace rousette::restconf {

Server::~Server() = default;

Server::Server(std::shared_ptr<sysrepo::Connection> conn)
    : server{std::make_unique<nghttp2::asio_http2::server::http2>()}
    , dwdmEvents{std::make_unique<sr::OpticalEvents>(std::make_shared<sysrepo::Session>(conn))}
{
    dwdmEvents->change.connect([this](const std::string& diff) {
        opticsChange(notifPrefix + diff + notifSuffix);
    });

    server->handle("/telemetry/optics", [this](const auto& req, const auto& res) {
        auto client = std::make_shared<http::EventStream>(req, res);
        client->activate(opticsChange);
    });
}

void Server::listen_and_serve(const std::string& address, const std::string& port)
{
    spdlog::debug("Listening at {} {}", address, port);
    boost::system::error_code ec;
    if (server->listen_and_serve(ec, address, port)) {
        throw std::runtime_error{"Server error: " + ec.message()};
    }
}
}
