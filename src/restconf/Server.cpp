/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <boost/algorithm/string.hpp>
#include <libyang-cpp/Enum.hpp>
#include <nghttp2/asio_http2_server.h>
#include <regex>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Enum.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include "http/utils.hpp"
#include "restconf/Exceptions.h"
#include "restconf/Nacm.h"
#include "restconf/PAM.h"
#include "restconf/Server.h"
#include "restconf/YangSchemaLocations.h"
#include "restconf/uri.h"
#include "restconf/utils.h"
#include "sr/OpticalEvents.h"
#include "NacmIdentities.h"

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
constexpr auto yangSchemaRoot = "/yang/";

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

bool isSameNode(const libyang::DataNode& child, const PathSegment& lastPathSegment)
{
    return child.schema().module().name() == *lastPathSegment.apiIdent.prefix && child.schema().name() == lastPathSegment.apiIdent.identifier;
}

bool isSameNode(const libyang::DataNode& a, const libyang::SchemaNode& b)
{
    return a.schema() == b;
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

void rejectWithError(libyang::Context ctx, const libyang::DataFormat& dataFormat, const request& req, const response& res, const int code, const std::string errorType, const std::string& errorTag, const std::string& errorMessage, const std::optional<std::string>& errorPath = std::nullopt)
{
    spdlog::debug("{}: Rejected with {}: {}", http::peer_from_request(req), errorTag, errorMessage);

    auto ext = ctx.getModuleImplemented("ietf-restconf")->extensionInstance("yang-errors");

    auto errors = ctx.newExtPath("/ietf-restconf:errors", std::nullopt, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-type", errorType, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-tag", errorTag, ext);
    errors->newExtPath("/ietf-restconf:errors/error[1]/error-message", errorMessage, ext);

    if (errorPath) {
        errors->newExtPath("/ietf-restconf:errors/error[1]/error-path", *errorPath, ext);
    }

    res.write_head(code, {{"content-type", {asMimeType(dataFormat), false}}, {"access-control-allow-origin", {"*", false}}});
    res.end(*errors->printStr(dataFormat, libyang::PrintFlags::WithSiblings));
}

std::optional<std::string> getHeaderValue(const nghttp2::asio_http2::header_map& headers, const std::string& header)
{
    auto it = headers.find(header);
    if (it == headers.end()) {
        return std::nullopt;
    }

    return it->second.value;
}

std::optional<std::string> parseUrlPrefix(const nghttp2::asio_http2::header_map& headers) {
    if (auto forward = getHeaderValue(headers, "forward")) {
        auto [proto, host] = http::parseForwardedHeader(*forward);
        if (!proto || !host) {
            return std::nullopt;
        }

        return *proto + "://" + *host;
    }

    return std::nullopt;
}

struct DataFormat {
    std::optional<libyang::DataFormat> request; // request encoding is not always needed (e.g. GET)
    libyang::DataFormat response;
};

/** @brief Chooses request and response data format w.r.t. accept/content-type http headers.
 * @throws ErrorResponse if invalid accept/content-type header found
 */
DataFormat chooseDataEncoding(const nghttp2::asio_http2::header_map& headers)
{
    std::vector<std::string> acceptTypes;
    std::optional<std::string> contentType;

    if (auto value = getHeaderValue(headers, "accept")) {
        acceptTypes = http::parseAcceptHeader(*value);
    }
    if (auto value = getHeaderValue(headers, "content-type")) {
        auto contentTypes = http::parseAcceptHeader(*value); // content type doesn't have the same syntax as accept but content-type is a singleton object similar to those in accept header (RFC 9110) so this should be fine

        if (contentTypes.size() > 1) {
            spdlog::trace("Multiple content-type entries found");
        }
        if (!contentTypes.empty()) {
            contentType = contentTypes.back(); // RFC 9110: Recipients often attempt to handle this error by using the last syntactically valid member of the list
        }
    }

    std::optional<libyang::DataFormat> resAccept;
    std::optional<libyang::DataFormat> resContentType;

    if (!acceptTypes.empty()) {
        for (const auto& mediaType : acceptTypes) {
            if (auto type = dataTypeFromMimeType(mediaType, MimeTypeWildcards::ALLOWED)) {
                resAccept = *type;
                break;
            }
        }

        if (!resAccept) {
            throw ErrorResponse(406, "application", "operation-not-supported", "No requested format supported");
        }
    }

    // If it (the types in the accept header) is not specified, the request input encoding format SHOULD be used, or the server MAY choose any supported content encoding format
    if (contentType) {
        if (auto type = dataTypeFromMimeType(*contentType, MimeTypeWildcards::FORBIDDEN)) {
            resContentType = *type;
        } else {
            // If the server does not support the requested input encoding for a request, then it MUST return an error response with a "415 Unsupported Media Type" status-line.
            throw ErrorResponse(415, "application", "operation-not-supported", "content-type format value not supported");
        }
    }

    if (!resAccept) {
        resAccept = resContentType;
    }

    // If there was no request input, then the default output encoding is XML or JSON, depending on server preference.
    return {resContentType, resAccept ? *resAccept : libyang::DataFormat::JSON};
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

/** @brief Checks if node is a key node in a maybeList node list */
bool isKeyNode(const auto& maybeList, const auto& node)
{
    if (maybeList.schema().nodeType() == libyang::NodeType::List) {
        auto listKeys = maybeList.schema().asList().keys();
        return std::any_of(listKeys.begin(), listKeys.end(), [&node](const auto& key) {
            return isSameNode(node, key);
        });
    }
    return false;
}

/** @brief In case node is a (leaf-)list check if the key values are the same as the keys specified in the lastPathSegment.
 * @return The node where the mismatch occurs */
std::optional<libyang::DataNode> checkKeysMismatch(const libyang::DataNode& node, const PathSegment& lastPathSegment)
{
    if (node.schema().nodeType() == libyang::NodeType::List) {
        const auto& listKeys = node.schema().asList().keys();
        for (size_t i = 0; i < listKeys.size(); ++i) {
            const auto& keyValueURI = lastPathSegment.keys[i];
            auto keyNodeData = node.findPath(std::string{listKeys[i].module().name()} + ":" + std::string{listKeys[i].name()});
            if (!keyNodeData) {
                return node;
            }

            const auto& keyValueData = keyNodeData->asTerm().valueStr();

            if (keyValueURI != keyValueData) {
                return keyNodeData;
            }
        }
    } else if (node.schema().nodeType() == libyang::NodeType::Leaflist) {
        if (lastPathSegment.keys[0] != node.asTerm().valueStr()) {
            return node;
        }
    }
    return std::nullopt;
}


struct RequestContext {
    const nghttp2::asio_http2::server::request& req;
    const nghttp2::asio_http2::server::response& res;
    DataFormat dataFormat;
    sysrepo::Session sess;
    std::string lyPathOriginal;
    std::string payload;
};

void processActionOrRPC(std::shared_ptr<RequestContext> requestCtx)
{
    requestCtx->sess.switchDatastore(sysrepo::Datastore::Operational);
    auto ctx = requestCtx->sess.getContext();

    try {
        auto rpcSchemaNode = ctx.findPath(requestCtx->lyPathOriginal);
        if (!requestCtx->dataFormat.request && static_cast<bool>(rpcSchemaNode.asActionRpc().input().child())) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Content-type header missing.");
        }

        // validate if action node's parent is present
        if (rpcSchemaNode.nodeType() == libyang::NodeType::Action) {
            // FIXME: This is race-prone: we check for existing action data node but before we send the RPC the node may be gone
            auto [pathToParent, pathSegment] = asLibyangPathSplit(ctx, requestCtx->req.uri().path);
            if (!dataExists(requestCtx->sess, pathToParent)) {
                throw ErrorResponse(400, "application", "operation-failed", "Action data node '" + requestCtx->lyPathOriginal + "' does not exist.");
            }
        }

        auto [parent, rpcNode] = ctx.newPath2(requestCtx->lyPathOriginal);

        if (!requestCtx->payload.empty()) {
            rpcNode->parseOp(requestCtx->payload, *requestCtx->dataFormat.request, libyang::OperationType::RpcRestconf);
        }

        auto rpcReply = requestCtx->sess.sendRPC(*rpcNode);

        if (rpcReply.immediateChildren().empty()) {
            requestCtx->res.write_head(204, {{"access-control-allow-origin", {"*", false}}});
            requestCtx->res.end();
            return;
        }

        auto responseNode = rpcReply.child();
        responseNode->unlinkWithSiblings();

        auto envelope = ctx.newOpaqueJSON(std::string{rpcNode->schema().module().name()}, "output", std::nullopt);
        envelope->insertChild(*responseNode);

        requestCtx->res.write_head(200, {
                                            {"content-type", {asMimeType(requestCtx->dataFormat.response), false}},
                                            {"access-control-allow-origin", {"*", false}},
                                        });
        requestCtx->res.end(*envelope->printStr(requestCtx->dataFormat.response, libyang::PrintFlags::WithSiblings));
    } catch (const ErrorResponse& e) {
        rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, e.code, e.errorType, e.errorTag, e.errorMessage, e.errorPath);
    } catch (const libyang::ErrorWithCode& e) {
        if (e.code() == libyang::ErrorCode::ValidationFailure) {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 400, "protocol", "invalid-value", "Validation failure: "s + e.what());
        } else {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 500, "application", "operation-failed", "Internal server error due to libyang exception: "s + e.what());
        }
    } catch (const sysrepo::ErrorWithCode& e) {
        if (e.code() == sysrepo::ErrorCode::Unauthorized) {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 403, "application", "access-denied", "Access denied.");
        } else if (e.code() == sysrepo::ErrorCode::ValidationFailed) {
            /*
             * FIXME: This happens on invalid input data (e.g., missing mandatory nodes) or missing action data node.
             * The former input should probably be validated by libyang's parseOp but it only parses. Is there better way? At least somehow extract logs?
             * We check on the missing action data node, but it is racy.
             */
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 400, "application", "operation-failed", "Input data validation failed");
        } else {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 500, "application", "operation-failed", "Internal server error due to sysrepo exception: "s + e.what());
        }
    }
}

void processPost(std::shared_ptr<RequestContext> requestCtx)
{
    try {
        auto ctx = requestCtx->sess.getContext();

        std::optional<libyang::DataNode> edit;
        std::optional<libyang::DataNode> node;
        std::list<libyang::DataNode> createdNodes;

        if (requestCtx->lyPathOriginal == "/") {
            node = edit = ctx.parseData(requestCtx->payload, *requestCtx->dataFormat.request, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);
            if (node) {
                const auto siblings = node->siblings();
                createdNodes = {siblings.begin(), siblings.end()};
            }
        } else {
            auto nodes = ctx.newPath2(requestCtx->lyPathOriginal, std::nullopt);
            edit = nodes.createdParent;
            node = nodes.createdNode;

            node->parseSubtree(requestCtx->payload, *requestCtx->dataFormat.request, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);
            if (node) {
                const auto children = node->immediateChildren();
                createdNodes = {children.begin(), children.end()};
            }
        }

        // filter out list key nodes, they can appear automatically when creating path that corresponds to a libyang list node
        for (auto it = createdNodes.begin(); it != createdNodes.end();) {
            if (node->schema().nodeType() == libyang::NodeType::List && it->schema().nodeType() == libyang::NodeType::Leaf && it->schema().asLeaf().isKey()) {
                it = createdNodes.erase(it);
            } else {
                ++it;
            }
        }

        if (createdNodes.size() != 1) {
            throw ErrorResponse(400, "protocol", "invalid-value", "The message body MUST contain exactly one instance of the expected data resource.");
        }

        auto mod = ctx.getModuleImplemented("ietf-netconf");
        createdNodes.begin()->newMeta(*mod, "operation", "create"); // FIXME: check no other nc:operations in the tree

        requestCtx->sess.editBatch(*edit, sysrepo::DefaultOperation::Merge);
        requestCtx->sess.applyChanges();

        requestCtx->res.write_head(201,
                                   {
                                       {"content-type", {asMimeType(requestCtx->dataFormat.response), false}},
                                       {"access-control-allow-origin", {"*", false}},
                                       // FIXME: POST data operation MUST return Location header
                                   });
        requestCtx->res.end();
    } catch (const ErrorResponse& e) {
        rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, e.code, e.errorType, e.errorTag, e.errorMessage, e.errorPath);
    } catch (libyang::ErrorWithCode& e) {
        if (e.code() == libyang::ErrorCode::ValidationFailure) {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 400, "protocol", "invalid-value", "Validation failure: "s + e.what());
        } else {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 500, "application", "operation-failed", "Internal server error due to libyang exception: "s + e.what());
        }
    } catch (sysrepo::ErrorWithCode& e) {
        if (e.code() == sysrepo::ErrorCode::Unauthorized) {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 403, "application", "access-denied", "Access denied.");
        } else if (e.code() == sysrepo::ErrorCode::ItemAlreadyExists) {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 409, "application", "resource-denied", "Resource already exists.");
        } else {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 500, "application", "operation-failed", "Internal server error due to sysrepo exception: "s + e.what());
        }
    }
}

void processPut(std::shared_ptr<RequestContext> requestCtx)
{
    try {
        auto ctx = requestCtx->sess.getContext();

        // PUT / means replace everything
        if (requestCtx->lyPathOriginal == "/") {
            auto edit = ctx.parseData(requestCtx->payload, *requestCtx->dataFormat.request, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);
            requestCtx->sess.replaceConfig(edit);

            requestCtx->res.write_head(edit ? 201 : 204,
                                       {
                                           {"content-type", {asMimeType(requestCtx->dataFormat.response), false}},
                                           {"access-control-allow-origin", {"*", false}},
                                       });
            requestCtx->res.end();
            return;
        }

        auto mod = ctx.getModuleImplemented("ietf-netconf");
        bool nodeExisted = dataExists(requestCtx->sess, requestCtx->lyPathOriginal);
        std::optional<libyang::DataNode> edit;
        std::optional<libyang::DataNode> replacementNode;

        auto [lyParentPath, lastPathSegment] = asLibyangPathSplit(requestCtx->sess.getContext(), requestCtx->req.uri().path);

        if (!lyParentPath.empty()) {
            auto [parent, node] = ctx.newPath2(lyParentPath, std::nullopt);
            node->parseSubtree(requestCtx->payload, *requestCtx->dataFormat.request, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);

            for (const auto& child : node->immediateChildren()) {
                /* everything that is under node is either
                 *  - a list key that was created by newPath2 call
                 *  - a single child that is created by parseSubtree with the name of lastPathSegment (which can be a list, then we need to check if the keys in provided data match the keys in URI)
                 * anything else is an error (either too many children provided or invalid name)
                 */
                if (isSameNode(child, lastPathSegment)) {
                    if (auto offendingNode = checkKeysMismatch(child, lastPathSegment)) {
                        throw ErrorResponse(400, "protocol", "invalid-value", "Invalid data for PUT (list key mismatch between URI path and data).", offendingNode->path());
                    }
                    replacementNode = child;
                } else if (isKeyNode(*node, child)) {
                    // do nothing here; key values are checked elsewhere
                } else {
                    throw ErrorResponse(400, "protocol", "invalid-value", "Invalid data for PUT (data contains invalid node).", child.path());
                }
            }

            edit = parent;
        } else {
            if (auto parent = ctx.parseData(requestCtx->payload, *requestCtx->dataFormat.request, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly); parent) {
                edit = parent;
                replacementNode = parent;
                if (!isSameNode(*replacementNode, lastPathSegment)) {
                    throw ErrorResponse(400, "protocol", "invalid-value", "Invalid data for PUT (data contains invalid node).", replacementNode->path());
                }
                if (auto offendingNode = checkKeysMismatch(*parent, lastPathSegment)) {
                    throw ErrorResponse(400, "protocol", "invalid-value", "Invalid data for PUT (list key mismatch between URI path and data).", offendingNode->path());
                }
            }
        }

        if (!replacementNode) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Invalid data for PUT (node indicated by URI is missing).");
        }

        replacementNode->newMeta(*mod, "operation", "replace"); // FIXME: check no other nc:operations in the tree

        requestCtx->sess.editBatch(*edit, sysrepo::DefaultOperation::Merge);
        requestCtx->sess.applyChanges();

        requestCtx->res.write_head(nodeExisted ? 204 : 201,
                                   {
                                       {"content-type", {asMimeType(requestCtx->dataFormat.response), false}},
                                       {"access-control-allow-origin", {"*", false}},
                                   });
        requestCtx->res.end();
    } catch (const ErrorResponse& e) {
        rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, e.code, e.errorType, e.errorTag, e.errorMessage, e.errorPath);
    } catch (libyang::ErrorWithCode& e) {
        if (e.code() == libyang::ErrorCode::ValidationFailure) {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 400, "protocol", "invalid-value", "Validation failure: "s + e.what());
        } else {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 500, "application", "operation-failed", "Internal server error due to libyang exception: "s + e.what());
        }
    } catch (sysrepo::ErrorWithCode& e) {
        if (e.code() == sysrepo::ErrorCode::Unauthorized) {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 403, "application", "access-denied", "Access denied.");
        } else {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 500, "application", "operation-failed", "Internal server error due to sysrepo exception: "s + e.what());
        }
    }
}

void authorizeRequest(const Nacm& nacm, sysrepo::Session& sess, const request& req)
{
    std::string nacmUser;
    if (auto authHeader = getHeaderValue(req.header(), "authorization")) {
        nacmUser = rousette::auth::authenticate_pam(*authHeader, http::peer_from_request(req));
    } else {
        nacmUser = ANONYMOUS_USER;
    }

    if (!nacm.authorize(sess, nacmUser)) {
        throw ErrorResponse(401, "protocol", "access-denied", "Access denied.");
    }
}

void processAuthError(const request& req, const response& res, const auth::Error& error, const std::function<void()>& errorResponseCb)
{
    if (error.delay) {
        spdlog::info("{}: Authentication failed (delay {}us): {}", http::peer_from_request(req), error.delay->count(), error.what());
        auto timer = std::make_shared<boost::asio::steady_timer>(res.io_service(), *error.delay);
        res.on_close([timer](uint32_t code) {
            (void)code;
            // Signal that the timer should be cancelled, so that its completion callback knows that
            // a conneciton is gone already.
            timer->cancel();
        });
        timer->async_wait([timer, errorResponseCb](const boost::system::error_code& ec) {
            if (ec.failed()) {
                // The `req` request has been already freed at this point and it's a dangling reference.
                // There's nothing else to do at this point.
            } else {
                errorResponseCb();
            }
        });
    } else {
        spdlog::error("{}: Authentication failed: {}", http::peer_from_request(req), error.what());
        errorResponseCb();
    }
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
    for (const auto& [module, version] : {std::pair<std::string, std::string>{"ietf-restconf", "2017-01-26"}, {"ietf-netconf", ""}}) {
        if (!conn.sessionStart().getContext().getModuleImplemented(module)) {
            throw std::runtime_error("Module "s + module + "@" + version + " is not implemented in sysrepo");
        }
    }

    // RFC 8527, we must implement at least ietf-yang-library@2019-01-04 and support operational DS
    if (auto mod = conn.sessionStart().getContext().getModuleImplemented("ietf-yang-library"); !mod || mod->revision() < "2019-01-04") {
        throw std::runtime_error("Module ietf-yang-library of revision at least 2019-01-04 is not implemented");
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

    server->handle(yangSchemaRoot, [conn /* intentional copy */](const auto& req, const auto& res) mutable {
        const auto& peer = http::peer_from_request(req);
        spdlog::info("{}: {} {}", peer, req.method(), req.uri().raw_path);

        if (req.method() != "GET") {
            res.write_head(405, {{"access-control-allow-origin", {"*", false}}});
            res.end();
            return;
        }

        auto sess = conn.sessionStart(sysrepo::Datastore::Operational);

        // TODO: Perhaps authorize users before providing the schemas so they could not scan for "known vulnerabilities"?

        auto mod = asYangModule(sess.getContext(), req.uri().path);
        if (!mod) {
            res.write_head(404, {{"access-control-allow-origin", {"*", false}}});
            res.end();
            return;
        }

        res.write_head(
            200,
            {
                {"content-type", {"application/yang", false}},
                {"access-control-allow-origin", {"*", false}},
            });
        res.end(std::visit([](auto&& arg) { return arg.printStr(libyang::SchemaOutputFormat::Yang); }, *mod));
    });

    server->handle(restconfRoot,
        [conn /* intentionally by value, otherwise conn gets destroyed when the ctor returns */, this](const auto& req, const auto& res) mutable {
            const auto& peer = http::peer_from_request(req);
            spdlog::info("{}: {} {}", peer, req.method(), req.uri().raw_path);

            auto sess = conn.sessionStart(sysrepo::Datastore::Operational);
            DataFormat dataFormat;
            // default for "early exceptions" when the MIME type detection fails
            dataFormat.response = libyang::DataFormat::JSON;

            try {
                dataFormat = chooseDataEncoding(req.header());
                authorizeRequest(nacm, sess, req);

                auto restconfRequest = asRestconfRequest(sess.getContext(), req.method(), req.uri().path);

                switch (restconfRequest.type) {
                case RestconfRequest::Type::YangLibraryVersion: {
                    auto ctx = sess.getContext();

                    if (auto mod = ctx.getModuleLatest("ietf-yang-library"); mod && mod->revision()) {
                        auto yangExt = ctx.getModuleImplemented("ietf-restconf")->extensionInstance("yang-api");
                        auto data = ctx.newExtPath("/ietf-restconf:restconf/yang-library-version", std::string{mod->revision().value()}, yangExt);
                        res.write_head(
                            200,
                            {
                                {"content-type", {asMimeType(dataFormat.response), false}},
                                {"access-control-allow-origin", {"*", false}},
                            });
                        res.end(*data->child()->printStr(dataFormat.response, libyang::PrintFlags::WithSiblings));
                    } else {
                        // this should be unreachable as ietf-yang-library should always be there; just to be sure...
                        throw ErrorResponse(500, "application", "operation-failed", "Module ietf-yang-library not implemented or has no revision.");
                    }
                    break;
                }

                case RestconfRequest::Type::GetData:
                    sess.switchDatastore(restconfRequest.datastore.value_or(sysrepo::Datastore::Operational));
                    if (auto data = sess.getData(restconfRequest.path); data) {
                        res.write_head(
                            200,
                            {
                                {"content-type", {asMimeType(dataFormat.response), false}},
                                {"access-control-allow-origin", {"*", false}},
                            });

                        res.end(*replaceYangLibraryLocations(parseUrlPrefix(req.header()), yangSchemaRoot, *data).printStr(dataFormat.response, libyang::PrintFlags::WithSiblings));
                    } else {
                        throw ErrorResponse(404, "application", "invalid-value", "No data from sysrepo.");
                    }
                    break;

                case RestconfRequest::Type::CreateOrReplaceThisNode:
                case RestconfRequest::Type::CreateChildren: {
                    if (restconfRequest.datastore == sysrepo::Datastore::FactoryDefault || restconfRequest.datastore == sysrepo::Datastore::Operational) {
                        throw ErrorResponse(405, "application", "operation-not-supported", "Read-only datastore.");
                    }

                    sess.switchDatastore(restconfRequest.datastore.value_or(sysrepo::Datastore::Running));
                    if (!dataFormat.request) {
                        throw ErrorResponse(400, "protocol", "invalid-value", "Content-type header missing.");
                    }

                    auto requestCtx = std::make_shared<RequestContext>(req, res, dataFormat, sess, restconfRequest.path);

                    req.on_data([requestCtx, restconfRequest /* intentional copy */](const uint8_t* data, std::size_t length) {
                        if (length > 0) {
                            requestCtx->payload.append(reinterpret_cast<const char*>(data), length);
                        } else if (restconfRequest.type == RestconfRequest::Type::CreateOrReplaceThisNode) {
                            processPut(requestCtx);
                        } else {
                            processPost(requestCtx);
                        }
                    });
                    break;
                }

                case RestconfRequest::Type::DeleteNode:
                    if (restconfRequest.datastore == sysrepo::Datastore::FactoryDefault || restconfRequest.datastore == sysrepo::Datastore::Operational) {
                        throw ErrorResponse(405, "application", "operation-not-supported", "Read-only datastore.");
                    }

                    sess.switchDatastore(restconfRequest.datastore.value_or(sysrepo::Datastore::Running));

                    try {
                        auto [edit, deletedNode] = sess.getContext().newPath2(restconfRequest.path, std::nullopt, libyang::CreationOptions::Opaque);
                        // If the node could be created, it will not be opaque. However, setting meta attributes to opaque and standard nodes is a different process.
                        if (deletedNode->isOpaque()) {
                            deletedNode->newAttrOpaqueJSON("ietf-netconf", "operation", "delete");
                        } else {
                            auto netconf = sess.getContext().getModuleLatest("ietf-netconf");
                            deletedNode->newMeta(*netconf, "operation", "delete");
                        }

                        sess.editBatch(*edit, sysrepo::DefaultOperation::Merge);
                        sess.applyChanges();
                    } catch (const sysrepo::ErrorWithCode& e) {
                        if (e.code() == sysrepo::ErrorCode::Unauthorized) {
                            throw ErrorResponse(403, "application", "access-denied", "Access denied.", restconfRequest.path);
                        } else if (e.code() == sysrepo::ErrorCode::NotFound) {
                            throw ErrorResponse(404, "protocol", "invalid-value", "Data resource not found.", restconfRequest.path);
                        }

                        throw;
                    }

                    res.write_head(
                        204,
                        {
                            {"access-control-allow-origin", {"*", false}},
                        });
                    res.end();
                    break;

                case RestconfRequest::Type::Execute: {
                    auto requestCtx = std::make_shared<RequestContext>(req, res, dataFormat, sess, restconfRequest.path);

                    req.on_data([requestCtx](const uint8_t* data, std::size_t length) {
                        if (length > 0) {
                            requestCtx->payload.append(reinterpret_cast<const char*>(data), length);
                        } else {
                            processActionOrRPC(requestCtx);
                        }
                    });
                    break;
                }
                }
            } catch (const auth::Error& e) {
                processAuthError(req, res, e, [sess, dataFormat, &req, &res]() {
                    rejectWithError(sess.getContext(), dataFormat.response, req, res, 401, "protocol", "access-denied", "Access denied.", std::nullopt);
                });
            } catch (const ErrorResponse& e) {
                rejectWithError(sess.getContext(), dataFormat.response, req, res, e.code, e.errorType, e.errorTag, e.errorMessage, e.errorPath);
            } catch (const sysrepo::ErrorWithCode& e) {
                spdlog::error("Sysrepo exception: {}", e.what());
                rejectWithError(sess.getContext(), dataFormat.response, req, res, 500, "application", "operation-failed", "Internal server error due to sysrepo exception.");
            }
        });

    boost::system::error_code ec;
    if (server->listen_and_serve(ec, address, port, true)) {
        throw std::runtime_error{"Server error: " + ec.message()};
    }
    spdlog::debug("Listening at {} {}", address, port);
}
}
