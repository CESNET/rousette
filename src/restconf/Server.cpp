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
#include "restconf/uri.h"
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
}

void rejectWithError(libyang::Context ctx, const request& req, const response& res, const int code, const std::string errorType, const std::string& errorTag, const std::string& errorMessage)
{
    spdlog::debug("{}: Rejected with {}: {}", http::peer_from_request(req), errorTag, errorMessage);

    auto ext = ctx.getModuleImplemented("ietf-restconf")->extensionInstance("yang-errors");

    auto errors = ctx.newExtPath("/ietf-restconf:errors", std::nullopt, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-type", errorType, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-tag", errorTag, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-message", errorMessage, ext);

    res.write_head(code, {{"content-type", {"application/yang-data+json", false}},
                          {"access-control-allow-origin", {"*", false}}});
    res.end(*errors->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::WithSiblings));
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
    if (!conn.sessionStart().getContext().getModuleImplemented("ietf-restconf")) {
        throw std::runtime_error("Module ietf-restconf@2017-01-26 is not implemented in sysrepo");
    }

    dwdmEvents->change.connect([this](const std::string& content) {
        opticsChange(as_restconf_push_update(content, std::chrono::system_clock::now()));
    });

    server->handle("/", [](const auto& req, const auto& res) {
        const auto& peer = http::peer_from_request(req);
        spdlog::info("{}: {} {}", peer, req.method(), req.uri().raw_path);
        res.write_head(404, {{"content-type", {"text/plain", false}},
                             {"access-control-allow-origin", {"*", false}}});
        res.end();
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
        [conn /* intentionally by value, otherwise conn gets destroyed when the ctor returns */, this](const auto& req, const auto& res) mutable {
            const auto& peer = http::peer_from_request(req);
            spdlog::info("{}: {} {}", peer, req.method(), req.uri().raw_path);

            auto sess = conn.sessionStart(sysrepo::Datastore::Operational);

            std::string nacmUser;
            if (auto itUserHeader = req.header().find("x-remote-user"); itUserHeader != req.header().end() && !itUserHeader->second.value.empty()) {
                nacmUser = itUserHeader->second.value;
            } else {
                rejectWithError(sess.getContext(), req, res, 401, "protocol", "access-denied", "HTTP header x-remote-user not found or empty.");
                return;
            }

            if (!nacm.authorize(sess, nacmUser)) {
                rejectWithError(sess.getContext(), req, res, 401, "protocol", "access-denied", "Access denied.");
                return;
            }

            if (req.method() != "GET") {
                rejectWithError(sess.getContext(), req, res, 405, "application", "operation-not-supported", "Method not allowed.");
                return;
            }

            try {
                auto lyPath = asLibyangPath(sess.getContext(), req.uri().path);
                if (!lyPath) {
                    rejectWithError(sess.getContext(), req, res, 400, "application", "operation-failed", "Not a subtree path."); // FIXME: Is this correct error? This is what Netopeer2 returns when invalid path is supplied.
                    return;
                }

                auto schNode = sess.getContext().findPath(*lyPath);
                if (schNode.nodeType() == libyang::NodeType::RPC || schNode.nodeType() == libyang::NodeType::Action) {
                    rejectWithError(sess.getContext(), req, res, 405, "protocol", "operation-not-supported", "Target resource is an operation resource.");
                    return;
                }

                if (auto data = sess.getData(*lyPath); data) {
                    res.write_head(
                        200,
                        {
                            {"content-type", {"application/yang-data+json", false}},
                            {"access-control-allow-origin", {"*", false}},
                        });
                    res.end(*data->printStr(libyang::DataFormat::JSON,
                                            libyang::PrintFlags::WithSiblings));
                } else {
                    rejectWithError(sess.getContext(), req, res, 404, "application", "invalid-value", "No data from sysrepo.");
                    return;
                }
            } catch (const std::runtime_error& e) {
                rejectWithError(sess.getContext(), req, res, 400, "application", "operation-failed", "Module not found.");
                return;
            } catch (const sysrepo::ErrorWithCode& e) {
                spdlog::error("Sysrepo exception: {}", e.what());
                rejectWithError(sess.getContext(), req, res, 500, "application", "operation-failed", "Internal server error due to sysrepo exception.");
            }
        });

    boost::system::error_code ec;
    if (server->listen_and_serve(ec, address, port, true)) {
        throw std::runtime_error{"Server error: " + ec.message()};
    }
    spdlog::debug("Listening at {} {}", address, port);
}
}
