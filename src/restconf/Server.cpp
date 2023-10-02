/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <boost/algorithm/string.hpp>
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

struct ErrorResponse : public std::exception {
    int code;
    std::string errorTag;
    std::string errorType;
    std::string errorMessage;

    ErrorResponse(int code, const std::string errorType, const std::string& errorTag, const std::string& errorMessage)
        : code(code)
        , errorTag(errorTag)
        , errorType(errorType)
        , errorMessage(errorMessage)
    {
    }
};

std::string asMimeType(libyang::DataFormat dataFormat)
{
    switch (dataFormat) {
    case libyang::DataFormat::JSON:
        return "application/yang-data+json";
    case libyang::DataFormat::XML:
        return "application/yang-data+xml";
    default:
        throw std::logic_error("Invalid data format");
    }
}

enum class MimeTypeWildcards { ALLOWED, FORBIDDEN };

bool mimeMatch(const std::string& providedMime, const std::string& applicationMime, MimeTypeWildcards wildcards)
{
    std::vector<std::string> tokensMime;
    std::vector<std::string> tokensApplicationMime;

    boost::split(tokensMime, providedMime, boost::is_any_of("/"));
    boost::split(tokensApplicationMime, applicationMime, boost::is_any_of("/"));

    if (wildcards == MimeTypeWildcards::ALLOWED) {
        if (tokensMime[0] == "*") {
            return true;
        }
        if (tokensMime[0] == tokensApplicationMime[0] && tokensMime[1] == "*") {
            return true;
        }
    }

    return tokensMime[0] == tokensApplicationMime[0] && tokensMime[1] == tokensApplicationMime[1];
}

std::optional<libyang::DataFormat> dataTypeFromMimeType(const std::string& mime, MimeTypeWildcards wildcards)
{
    if (mimeMatch(mime, asMimeType(libyang::DataFormat::JSON), wildcards)) {
        return libyang::DataFormat::JSON;
    } else if (mimeMatch(mime, asMimeType(libyang::DataFormat::XML), wildcards)) {
        return libyang::DataFormat::XML;
    }

    return std::nullopt;
}

void rejectWithError(libyang::Context ctx, const libyang::DataFormat& dataFormat, const request& req, const response& res, const int code, const std::string errorType, const std::string& errorTag, const std::string& errorMessage)
{
    spdlog::debug("{}: Rejected with {}: {}", http::peer_from_request(req), errorTag, errorMessage);

    auto ext = ctx.getModuleImplemented("ietf-restconf")->extensionInstance("yang-errors");

    auto errors = ctx.newExtPath("/ietf-restconf:errors", std::nullopt, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-type", errorType, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-tag", errorTag, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-message", errorMessage, ext);

    res.write_head(code, {{"content-type", {asMimeType(dataFormat), false}}, {"access-control-allow-origin", {"*", false}}});
    res.end(*errors->printStr(dataFormat, libyang::PrintFlags::WithSiblings));
}

/** @brief Chooses data response data format w.r.t. accept/content-type http header.
 * @throws ErrorResponse if invalid accept/content-type header found
 */
libyang::DataFormat chooseDataEncoding(const nghttp2::asio_http2::header_map& headers)
{
    std::vector<std::string> acceptTypes;
    std::optional<std::string> contentType;

    if (auto it = headers.find("accept"); it != headers.end()) {
        acceptTypes = http::parseAcceptHeader(it->second.value);
    }
    if (auto it = headers.find("content-type"); it != headers.end()) {
        auto contentTypes = http::parseAcceptHeader(it->second.value); // content type doesn't have the same syntax as accept but content-type is a singleton object similar to those in accept header (RFC 9110) so this should be fine

        if (contentTypes.size() > 1) {
            spdlog::trace("Multiple content-type entries found");
        }
        if (!contentTypes.empty()) {
            contentType = contentTypes.back(); // RFC 9110: Recipients often attempt to handle this error by using the last syntactically valid member of the list
        }
    }

    if (!acceptTypes.empty()) {
        for (const auto& mediaType : acceptTypes) {
            if (auto type = dataTypeFromMimeType(mediaType, MimeTypeWildcards::ALLOWED)) {
                return *type;
            }
        }

        throw ErrorResponse(406, "application", "operation-not-supported", "No requested format supported");
    }

    // If it (the types in the accept header) is not specified, the request input encoding format SHOULD be used, or the server MAY choose any supported content encoding format
    if (contentType) {
        if (auto type = dataTypeFromMimeType(*contentType, MimeTypeWildcards::FORBIDDEN)) {
            return *type;
        } else {
            // If the server does not support the requested input encoding for a request, then it MUST return an error response with a "415 Unsupported Media Type" status-line.
            throw ErrorResponse(415, "application", "operation-not-supported", "content-type format value not supported");
        }
    }

    // If there was no request input, then the default output encoding is XML or JSON, depending on server preference.
    return libyang::DataFormat::JSON;
}

bool dataExists(sysrepo::Session session, const std::string& path)
{
    if (auto data = session.getData(path)) {
        if (data->findPath(path)) {
            return true;
        }
    }
    return false;
}

struct RequestContext {
    const nghttp2::asio_http2::server::request& req;
    const nghttp2::asio_http2::server::response& res;
    libyang::DataFormat dataFormat;
    sysrepo::Session sess;
    std::string lyPath;
    std::string payload;
};

void processPut(std::shared_ptr<RequestContext> requestCtx)
{
    auto ctx = requestCtx->sess.getContext();
    requestCtx->sess.switchDatastore(sysrepo::Datastore::Running);

    bool nodeExisted = dataExists(requestCtx->sess, requestCtx->lyPath);
    auto [parent, node] = ctx.newPath2(requestCtx->lyPath, std::nullopt);

    if (node->parent()) {
        spdlog::error(".... {} {}", node->path(), node->parent()->path());

        node = node->parent();
        node->parseSubtree(requestCtx->payload, libyang::DataFormat::JSON, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);
        spdlog::error(".... {}", *node->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::WithSiblings));
    } else {
        spdlog::error(".... {}", node->path());
        parent = node = ctx.parseData(requestCtx->payload, libyang::DataFormat::JSON, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);
        spdlog::error(".... {}", *node->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::WithSiblings));
    }

    auto mod = ctx.getModuleImplemented("ietf-netconf");
    node->newMeta(*mod, "operation", "replace");

    try {
        requestCtx->sess.editBatch(*parent, sysrepo::DefaultOperation::Merge);
        requestCtx->sess.applyChanges();
    } catch (const sysrepo::ErrorWithCode& e) {
        // FIXME: handle this properly
        spdlog::error("sysrepo edit/apply boo boo: {}", e.what());
        throw;
    }

    requestCtx->res.write_head(nodeExisted ? 204 : 201,
                               {
                                   {"content-type", {asMimeType(requestCtx->dataFormat), false}},
                                   {"access-control-allow-origin", {"*", false}},
                               });
    requestCtx->res.end();
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

    m_runningSub = conn.sessionStart(sysrepo::Datastore::Running).onModuleChange("example", [&](auto, auto, auto, auto, auto, auto) {
        return sysrepo::ErrorCode::Ok;
    });

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
            libyang::DataFormat dataFormat;

            try {
                dataFormat = chooseDataEncoding(req.header());
            } catch (const ErrorResponse& e) {
                rejectWithError(sess.getContext(), libyang::DataFormat::JSON, req, res, e.code, e.errorType, e.errorTag, e.errorMessage);
                return;
            }

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

            try {
                auto lyPath = asLibyangPath(sess.getContext(), req.uri().path);

                if (req.method() == "GET") {
                    if (auto data = sess.getData(*lyPath); data) {
                        res.write_head(
                            200,
                            {
                                {"content-type", {asMimeType(dataFormat), false}},
                                {"access-control-allow-origin", {"*", false}},
                            });
                        res.end(*data->printStr(dataFormat, libyang::PrintFlags::WithSiblings));
                    } else {
                        rejectWithError(sess.getContext(), dataFormat, req, res, 404, "application", "invalid-value", "No data from sysrepo.");
                        return;
                    }
                } else if (req.method() == "PUT") {
                    auto requestCtx = std::make_shared<RequestContext>(req, res, dataFormat, sess, *lyPath);

                    req.on_data([requestCtx](const uint8_t* data, std::size_t length) {
                        if (length > 0) {
                            requestCtx->payload.append(reinterpret_cast<const char*>(data), length);
                        } else {
                            processPut(requestCtx);
                        }
                    });
                } else {
                    rejectWithError(sess.getContext(), dataFormat, req, res, 405, "application", "operation-not-supported", "Method not allowed.");
                    return;
                }
            } catch (const sysrepo::ErrorWithCode& e) {
                spdlog::error("Sysrepo exception: {}", e.what());
                rejectWithError(sess.getContext(), dataFormat, req, res, 500, "application", "operation-failed", "Internal server error due to sysrepo exception.");
            } catch (const InvalidURIException& e) {
                spdlog::error("URI parser exception: {}", e.what());
                rejectWithError(sess.getContext(), dataFormat, req, res, 400, "application", "operation-failed", e.what());
            }
        });

    boost::system::error_code ec;
    if (server->listen_and_serve(ec, address, port, true)) {
        throw std::runtime_error{"Server error: " + ec.message()};
    }
    spdlog::debug("Listening at {} {}", address, port);
}
}
