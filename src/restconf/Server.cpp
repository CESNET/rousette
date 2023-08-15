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

struct DataFormat {
    libyang::DataFormat lyDataFormat;

    std::string asMimeType() const
    {
        switch (lyDataFormat) {
        case libyang::DataFormat::JSON:
            return "application/yang-data+json";
        case libyang::DataFormat::XML:
            return "application/yang-data+xml";
        default:
            throw std::logic_error("Invalid data format");
        }
    };
};

void rejectWithError(libyang::Context ctx, const DataFormat& dataFormat, const request& req, const response& res, const int code, const std::string errorType, const std::string& errorTag, const std::string& errorMessage)
{
    spdlog::debug("{}: Rejected with {}: {}", http::peer_from_request(req), errorTag, errorMessage);

    auto ext = ctx.getModuleImplemented("ietf-restconf")->extensionInstance("yang-errors");

    auto errors = ctx.newExtPath("/ietf-restconf:errors", std::nullopt, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-type", errorType, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-tag", errorTag, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-message", errorMessage, ext);

    res.write_head(code, {{"content-type", {dataFormat.asMimeType(), false}},
                          {"access-control-allow-origin", {"*", false}}});
    res.end(*errors->printStr(dataFormat.lyDataFormat, libyang::PrintFlags::WithSiblings));
}

std::variant<DataFormat, int> chooseDataFormat(const nghttp2::asio_http2::header_map& headers)
{
    static const auto yangJsonMime = "application/yang-data+json";
    static const auto yangXmlMime = "application/yang-data+xml";

    std::vector<std::string> acceptTypes;
    if (auto itHeader = headers.find("accept"); itHeader != headers.end()) {
        acceptTypes = http::parseAcceptHeader(itHeader->second.value);
    }

    // FIXME 415 jeste predtimhle returnem

    if (!acceptTypes.empty()) {
        for (const auto& mediaType : acceptTypes) {
            if (mediaType == yangXmlMime) {
                return DataFormat{libyang::DataFormat::XML};
            } else if (mediaType == yangJsonMime) {
                return DataFormat{libyang::DataFormat::JSON};
            }
        }

        return 406; // If the server does not support any of the requested output encodings for a request, then it MUST return an error response with a "406 Not Acceptable" status-line
    }

    // If it (the types in the accept header) is not specified, the request input encoding format SHOULD be used, or the server MAY choose any supported content encoding format
    if (auto itHeader = headers.find("content-type"); itHeader != headers.end()) {
        const auto& contentType = itHeader->second.value;
        if (contentType == yangXmlMime || boost::starts_with(contentType, yangXmlMime + ";"s)) {
            return DataFormat{libyang::DataFormat::XML};
        } else if (contentType == yangJsonMime || boost::starts_with(contentType, yangJsonMime + ";"s)) {
            return DataFormat{libyang::DataFormat::JSON};
        } else {
            return 415; // If the server does not support the requested input encoding for a request, then it MUST return an error response with a "415 Unsupported Media Type" status-line.
        }
    }

    // If there was no request input, then the default output encoding is XML or JSON, depending on server preference.
    return DataFormat{libyang::DataFormat::JSON};
}
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

            std::variant<DataFormat, int> dataFormatX = chooseDataFormat(req.header());
            if (auto* statusCode = std::get_if<int>(&dataFormatX)) {
                res.write_head(*statusCode, {
                                                {"access-control-allow-origin", {"*", false}},
                                            });
                res.end();
                return;
            }
            auto dataFormat = std::get<DataFormat>(dataFormatX);

            auto sess = conn.sessionStart(sysrepo::Datastore::Operational);

            std::string nacmUser;
            if (auto itUserHeader = req.header().find("x-remote-user"); itUserHeader != req.header().end() && !itUserHeader->second.value.empty()) {
                nacmUser = itUserHeader->second.value;
            } else {
                rejectWithError(sess.getContext(), dataFormat, req, res, 401, "protocol", "access-denied", "HTTP header x-remote-user not found or empty.");
                return;
            }

            if (!nacm.authorize(sess, nacmUser)) {
                rejectWithError(sess.getContext(), dataFormat, req, res, 401, "protocol", "access-denied", "Access denied.");
                return;
            }

            if (req.method() != "GET") {
                rejectWithError(sess.getContext(), dataFormat, req, res, 405, "application", "operation-not-supported", "Method not allowed.");
                return;
            }

            auto lyPath = asLibyangPath(sess.getContext(), req.uri().path);
            if (!lyPath) {
                rejectWithError(sess.getContext(), dataFormat, req, res, 400, "application", "operation-failed", "Not a subtree path."); // FIXME: Is this correct error? This is what Netopeer2 returns when invalid path is supplied.
                return;
            }

            try {
                auto schNode = sess.getContext().findPath(*lyPath);
                if (schNode.nodeType() == libyang::NodeType::RPC || schNode.nodeType() == libyang::NodeType::Action) {
                    rejectWithError(sess.getContext(), dataFormat, req, res, 405, "protocol", "operation-not-supported", "Target resource is an operation resource.");
                    return;
                }

                if (auto data = sess.getData(*lyPath); data) {
                    res.write_head(
                        200,
                        {
                            {"content-type", {dataFormat.asMimeType(), false}},
                            {"access-control-allow-origin", {"*", false}},
                        });
                    res.end(*data->printStr(dataFormat.lyDataFormat, libyang::PrintFlags::WithSiblings));
                } else {
                    rejectWithError(sess.getContext(), dataFormat, req, res, 404, "application", "invalid-value", "No data from sysrepo.");
                    return;
                }
            } catch (const sysrepo::ErrorWithCode& e) {
                spdlog::error("Sysrepo exception: {}", e.what());
                rejectWithError(sess.getContext(), dataFormat, req, res, 500, "application", "operation-failed", "Internal server error due to sysrepo exception.");
            }
        });

    boost::system::error_code ec;
    if (server->listen_and_serve(ec, address, port, true)) {
        throw std::runtime_error{"Server error: " + ec.message()};
    }
    spdlog::debug("Listening at {} {}", address, port);
}
}
