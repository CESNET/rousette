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
#include <sysrepo-cpp/Enum.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include "http/utils.hpp"
#include "restconf/Nacm.h"
#include "restconf/Server.h"
#include "restconf/utils.h"
#include "sr/OpticalEvents.h"

using namespace std::literals;

using nghttp2::asio_http2::server::request;
using nghttp2::asio_http2::server::response;

namespace rousette::restconf {

namespace {
constexpr auto notifPrefix = R"json({"ietf-restconf:notification":{"eventTime":")json";
constexpr auto notifMid = R"json(","ietf-yang-push:push-update":{"datastore-contents":)json";
constexpr auto notifSuffix = R"json(}}})json";

template <typename T>
auto as_restconf_push_update(const std::string& content, const T& time)
{
    return notifPrefix + yangDateTime<typename T::clock, std::chrono::nanoseconds>(time) + notifMid + content + notifSuffix;
}

constexpr auto restconfRoot = "/restconf/";

namespace pattern {
const auto atom = "[a-zA-Z_][a-zA-Z0-9_.-]*"s;
const std::regex moduleWildcard{"^"s + restconfRoot + "data/(" + atom + ":\\*)$"};
const std::regex subtree{"^"s + restconfRoot + "data/(" + atom + ":" + atom + "(/(" + atom + ":)?" + atom + ")*)$"};
}
}

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

bool hasModuleForPath(const sysrepo::Session& session, const std::string& path)
{
    std::string moduleName;
    if (auto pos = path.find(":"); pos != std::string::npos) {
        moduleName = path.substr(0, pos);
    } else {
        moduleName = path;
    }

    return session.getContext().getModuleImplemented(moduleName).has_value();
}

void rejectResponse(const request& req, const response& res, const int code, const std::string& message)
{
    spdlog::debug("{}: {}", http::peer_from_request(req), message);
    res.write_head(code, {{"content-type", {"text/plain", false}},
                          {"access-control-allow-origin", {"*", false}}});
    res.end("go away");
}

Server::~Server()
{
    // notification to stop has to go through the asio io_context
    for (const auto& service : server->io_services()) {
        boost::asio::deadline_timer t{*service, boost::posix_time::pos_infin};
        t.async_wait([server = this->server.get()](const boost::system::error_code&) {
                spdlog::trace("Stoping HTTP/2 server");
                server->stop();
                });
        t.cancel();
    }

    server->join();
}

Server::Server(sysrepo::Connection conn, const std::string& address, const std::string& port)
    : nacm(conn)
    , server{std::make_unique<nghttp2::asio_http2::server::http2>()}
    , dwdmEvents{std::make_unique<sr::OpticalEvents>(conn.sessionStart())}
{
    dwdmEvents->change.connect([this](const std::string& content) {
        opticsChange(as_restconf_push_update(content, std::chrono::system_clock::now()));
    });

    server->handle("/", [](const auto& req, const auto& res) {
        const auto& peer = http::peer_from_request(req);
        spdlog::info("{}: {} {}", peer, req.method(), req.uri().raw_path);
        rejectResponse(req, res, 404, "resource does not exist");
    });

    server->handle("/.well-known/host-meta", [](const auto& req, const auto& res) {
        const auto& peer = http::peer_from_request(req);
        spdlog::info("{}: {} {}", peer, req.method(), req.uri().raw_path);
        res.write_head(
                   200,
                   {
                       {"content-type", {"application/xrd+xml", false}},
                       {"access-control-allow-origin", {"*", false}},
                   });
        res.end("<XRD xmlns='http://docs.oasis-open.org/ns/xri/xrd-1.0'><Link rel='restconf' href='"s + restconfRoot + "'></XRD>"s);
    });

    server->handle("/telemetry/optics", [this](const auto& req, const auto& res) {
        auto client = std::make_shared<http::EventStream>(req, res);
        client->activate(opticsChange, as_restconf_push_update(dwdmEvents->currentData(), std::chrono::system_clock::now()));
    });

    server->handle(restconfRoot,
        [&](const auto& req, const auto& res) mutable {
            const auto& peer = http::peer_from_request(req);
            spdlog::info("{}: {} {}", peer, req.method(), req.uri().raw_path);

            std::string nacmUser;
            if (auto itUserHeader = req.header().find("x-remote-user"); itUserHeader != req.header().end() && !itUserHeader->second.value.empty()) {
                nacmUser = itUserHeader->second.value;
            } else {
                rejectResponse(req, res, 401, "HTTP header x-remote-user not found or empty");
                return;
            }

            auto sess = conn.sessionStart(sysrepo::Datastore::Operational);
            if (!nacm.authorize(sess, nacmUser)) {
                rejectResponse(req, res, 401, "access denied");
                return;
            }

            if (req.method() != "GET") {
                rejectResponse(req, res, 400, "nothing but GET works");
                return;
            }

            auto path = as_subtree_path(req.uri().path);
            if (!path) {
                rejectResponse(req, res, 400, "not a subtree path");
                return;
            }

            try {
                if (!hasModuleForPath(sess, *path)) {
                    rejectResponse(req, res, 404, "module not implemented");
                    return;
                }

                if (auto data = sess.getData('/' + *path); data) {
                    res.write_head(
                        200,
                        {
                            {"content-type", {"application/yang-data+json", false}},
                            {"access-control-allow-origin", {"*", false}},
                        });
                    res.end(*data->printStr(libyang::DataFormat::JSON,
                                            libyang::PrintFlags::WithSiblings));
                } else {
                    rejectResponse(req, res, 404, "no data from sysrepo");
                    return;
                }
            } catch (const sysrepo::ErrorWithCode& e) {
                spdlog::error("Sysrepo exception: {}", e.what());
                rejectResponse(req, res, 500, "sysrepo exception: internal server error");
            }
        });

    boost::system::error_code ec;
    if (server->listen_and_serve(ec, address, port, true)) {
        throw std::runtime_error{"Server error: " + ec.message()};
    }
    spdlog::debug("Listening at {} {}", address, port);
}
}
