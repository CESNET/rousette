/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <boost/algorithm/string/predicate.hpp>
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

const std::initializer_list<std::string> allowedPrefixes {
    {"czechlight-roadm-device"s},
    {"czechlight-coherent-add-drop"s},
    {"czechlight-inline-amp"s},
    {"ietf-yang-library"s},
    {"ietf-hardware"s},
    {"ietf-interfaces"s},
    {"ietf-system"s},
    {"czechlight-lldp"s},
    {"czechlight-system:firmware"s},
    {"czechlight-system:networking"s},
};
}
}

using nghttp2::asio_http2::server::request;
using nghttp2::asio_http2::server::response;

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

bool allow_anonymous_read_for(const std::string& path)
{
    return std::any_of(std::begin(pattern::allowedPrefixes), std::end(pattern::allowedPrefixes),
            [path](const auto& prefix) {
                if (prefix.find(':') == std::string::npos) {
                    return boost::starts_with(path, prefix + ":");
                } else {
                    return boost::starts_with(path, prefix);
                }
            });
}

void rejectResponse(const request& req, const response& res, const int code, const std::string& message)
{
    spdlog::debug("{}: {}", http::peer_from_request(req), message);
    res.write_head(code, {{"content-type", {"text/plain", false}}});
    res.end("go away");
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
        client->activate(opticsChange, as_restconf_push_update(dwdmEvents->initialData()));
    });

    server->handle(restconfRoot,
        [conn](const auto& req, const auto& res) {
            const auto& peer = http::peer_from_request(req);
            spdlog::info("{}: {} {}", peer, req.method(), req.uri().raw_path);

            if (req.method() != "GET") {
                rejectResponse(req, res, 400, "nothing but GET works");
                return;
            }

            auto path = as_subtree_path(req.uri().path);
            if (!path) {
                rejectResponse(req, res, 400, "not a subtree path");
                return;
            }

            if (!allow_anonymous_read_for(*path)) {
                rejectResponse(req, res, 400, "module not allowed");
                return;
            }

            auto sess = std::make_shared<sysrepo::Session>(conn);
            sess->session_switch_ds(SR_DS_OPERATIONAL);
            auto data = sess->get_data(('/' + *path).c_str());

            if (!data) {
                rejectResponse(req, res, 404, "no data from sysrepo");
                return;
            }

            res.write_head(200, {{"content-type", {"application/yang-data+json", false}}});
            res.end(data->print_mem(LYD_JSON, 0));
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
