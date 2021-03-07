/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <nghttp2/asio_http2_server.h>
#include <regex>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Session.hpp>
#include "http/utils.hpp"
#include "restconf/Server.h"
#include "sr/OpticalEvents.h"

using namespace std::literals;

namespace {
constexpr auto notifPrefix = R"json({"ietf-restconf:notification":{"ietf-yang-push:push-update":{"datastore-contents":)json";
constexpr auto notifSuffix = R"json(}}})json";

auto as_restconf_push_update(const std::string& content)
{
    return notifPrefix + content + notifSuffix;
}

constexpr auto restconfRoot = "/restconf/";

namespace pattern {
const auto atom = "[a-zA-Z_][a-zA-Z_.-]*"s;
const std::regex moduleWildcard{"^"s + restconfRoot + "data/(" + atom + ":\\*)$"};
const std::regex subtree{"^"s + restconfRoot + "data/(" + atom + ":" + atom + "(/(" + atom + ":)?" + atom + ")*)$"};
}
}

namespace rousette::restconf {

std::optional<std::string> as_subtree_path(const std::string& path)
{
    std::smatch match;
    if (std::regex_match(path, match, pattern::moduleWildcard) && match.size() > 1) {
        return match[1].str();
    }
    if (std::regex_match(path, match, pattern::subtree) && match.size() > 1) {
        return match[1].str();
    }
    return std::nullopt;
}

Server::~Server() = default;

Server::Server(std::shared_ptr<sysrepo::Connection> conn)
    : server{std::make_unique<nghttp2::asio_http2::server::http2>()}
    , dwdmEvents{std::make_unique<sr::OpticalEvents>(std::make_shared<sysrepo::Session>(conn))}
{
    dwdmEvents->change.connect([this](const std::string& content) {
        opticsChange(as_restconf_push_update(content));
    });

    server->handle("/telemetry/optics", [this](const auto& req, const auto& res) {
        auto client = std::make_shared<http::EventStream>(req, res);
        client->activate(opticsChange, as_restconf_push_update(dwdmEvents->currentData()));
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
