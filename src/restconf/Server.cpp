/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <libyang-cpp/Enum.hpp>
#include <nghttp2/asio_http2_server.h>
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
#include "restconf/utils/dataformat.h"
#include "restconf/utils/yang.h"
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

bool isSameNode(const libyang::DataNode& child, const PathSegment& lastPathSegment)
{
    return child.schema().module().name() == *lastPathSegment.apiIdent.prefix && child.schema().name() == lastPathSegment.apiIdent.identifier;
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
    RestconfRequest restconfRequest;
    std::string payload;
};

std::string yangInsertKey(const libyang::Context& ctx, const libyang::SchemaNode& listNode, const queryParams::QueryParams& queryParams)
{
    auto it = queryParams.find("point");
    const auto& pointParsed = std::get<queryParams::insert::PointParsed>(it->second);

    if (listNode != asLibyangSchemaNode(ctx, pointParsed)) {
        throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter 'point' contains path to a different list");
    }

    if (listNode.nodeType() == libyang::NodeType::List) {
        return listKeyPredicate(listNode.asList().keys(), pointParsed.back().keys);
    } else if (listNode.nodeType() == libyang::NodeType::Leaflist) {
        return pointParsed.back().keys[0];
    }

    throw std::logic_error("Node is neither a list nor a leaf-list");
}

void yangInsert(const RequestContext& requestCtx, libyang::DataNode& listEntryNode)
{
    auto modYang = requestCtx.sess.getContext().getModuleImplemented("yang");

    if (auto it = requestCtx.restconfRequest.queryParams.find("insert"); it != requestCtx.restconfRequest.queryParams.end()) {
        if (!isUserOrderedList(listEntryNode)) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter 'insert' is valid only for inserting into lists or leaf-lists that are 'ordered-by user'");
        }

        if (std::holds_alternative<queryParams::insert::First>(it->second)) {
            listEntryNode.newMeta(*modYang, "insert", "first");
        } else if (std::holds_alternative<queryParams::insert::Last>(it->second)) {
            listEntryNode.newMeta(*modYang, "insert", "last");
        } else if (auto hasBefore = std::holds_alternative<queryParams::insert::Before>(it->second); hasBefore || std::holds_alternative<queryParams::insert::After>(it->second)) {
            listEntryNode.newMeta(*modYang, "insert", hasBefore ? "before" : "after");
            listEntryNode.newMeta(*modYang,
                                  listEntryNode.schema().nodeType() == libyang::NodeType::List ? "key" : "value",
                                  yangInsertKey(requestCtx.sess.getContext(), listEntryNode.schema(), requestCtx.restconfRequest.queryParams));
        }
    }
}

void processActionOrRPC(std::shared_ptr<RequestContext> requestCtx)
{
    requestCtx->sess.switchDatastore(sysrepo::Datastore::Operational);
    auto ctx = requestCtx->sess.getContext();

    try {
        auto rpcSchemaNode = ctx.findPath(requestCtx->restconfRequest.path);
        if (!requestCtx->dataFormat.request && static_cast<bool>(rpcSchemaNode.asActionRpc().input().child())) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Content-type header missing.");
        }

        // validate if action node's parent is present
        if (rpcSchemaNode.nodeType() == libyang::NodeType::Action) {
            // FIXME: This is race-prone: we check for existing action data node but before we send the RPC the node may be gone
            auto [pathToParent, pathSegment] = asLibyangPathSplit(ctx, requestCtx->req.uri().path);
            if (!requestCtx->sess.getData(pathToParent)) {
                throw ErrorResponse(400, "application", "operation-failed", "Action data node '" + requestCtx->restconfRequest.path + "' does not exist.");
            }
        }

        auto [parent, rpcNode] = ctx.newPath2(requestCtx->restconfRequest.path);

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
        std::vector<libyang::DataNode> createdNodes;

        if (requestCtx->restconfRequest.path == "/") {
            node = edit = ctx.parseData(requestCtx->payload, *requestCtx->dataFormat.request, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);
            if (node) {
                const auto siblings = node->siblings();
                createdNodes = {siblings.begin(), siblings.end()};
            }
        } else {
            auto nodes = ctx.newPath2(requestCtx->restconfRequest.path, std::nullopt);
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

        auto modNetconf = ctx.getModuleImplemented("ietf-netconf");

        createdNodes.begin()->newMeta(*modNetconf, "operation", "create"); // FIXME: check no other nc:operations in the tree
        yangInsert(*requestCtx, *createdNodes.begin());

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
        } else if (e.code() == sysrepo::ErrorCode::NotFound) {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 400, "protocol", "invalid-value", e.what());
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
        if (requestCtx->restconfRequest.path == "/") {
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

        bool nodeExisted = !!requestCtx->sess.getData(requestCtx->restconfRequest.path);
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

        auto modNetconf = ctx.getModuleImplemented("ietf-netconf");

        replacementNode->newMeta(*modNetconf, "operation", "replace"); // FIXME: check no other nc:operations in the tree
        yangInsert(*requestCtx, *replacementNode);

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
        } else if (e.code() == sysrepo::ErrorCode::NotFound) {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 400, "protocol", "invalid-value", e.what());
        } else {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 500, "application", "operation-failed", "Internal server error due to sysrepo exception: "s + e.what());
        }
    }
}

void authorizeRequest(const Nacm& nacm, sysrepo::Session& sess, const request& req)
{
    std::string nacmUser;
    if (auto authHeader = http::getHeaderValue(req.header(), "authorization")) {
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

libyang::PrintFlags libyangPrintFlags(const libyang::DataNode& dataNode, const std::string& requestPath, const std::optional<queryParams::QueryParamValue>& withDefaults)
{
    std::optional<libyang::DataNode> node;

    /*
     * RFC 8040, sec. 3.5.4:
     *   If the target of a GET method is a data node that represents a leaf
     *   or leaf-list that has a default value and the leaf or leaf-list has
     *   not been instantiated yet, the server MUST return the default value
     *   or values that are in use by the server, In this case, the server
     *   MUST ignore its "basic-mode", described in Section 4.8.9, and return
     *   the default value.
     *
     * My interpretation is that this only applies when no with-defaults query parameter is set. The with-defaults can override this.
    */

    // Be careful, we can get something like /* which is not a valid path. In other cases, the node should be valid in the schema (we check that in the parser) but the actual data node might not be instantiated
    try {
        node = dataNode.findPath(requestPath);
    } catch(const libyang::Error& e) {
    }

    libyang::PrintFlags ret = libyang::PrintFlags::WithSiblings;

    if (!withDefaults && node && (node->schema().nodeType() == libyang::NodeType::Leaf || node->schema().nodeType() == libyang::NodeType::Leaflist) && node->asTerm().isImplicitDefault()) {
        return ret | libyang::PrintFlags::WithDefaultsAll;
    }

    // Explicit is our default mode
    if (!withDefaults || std::holds_alternative<queryParams::withDefaults::Explicit>(*withDefaults)) {
        return ret;
    } else if (std::holds_alternative<queryParams::withDefaults::Trim>(*withDefaults)) {
        return ret | libyang::PrintFlags::WithDefaultsTrim;
    } else if (std::holds_alternative<queryParams::withDefaults::ReportAll>(*withDefaults)) {
        return ret | libyang::PrintFlags::WithDefaultsAll;
    } else if (std::holds_alternative<queryParams::withDefaults::ReportAllTagged>(*withDefaults)) {
        return ret | libyang::PrintFlags::WithDefaultsAllTag;
    }

    throw std::logic_error("Invalid withDefaults query parameter value");
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
    : m_monitoringSession(conn.sessionStart(sysrepo::Datastore::Operational))
    , nacm(conn)
    , server{std::make_unique<nghttp2::asio_http2::server::http2>()}
    , dwdmEvents{std::make_unique<sr::OpticalEvents>(conn.sessionStart())}
{
    for (const auto& [module, version] : {std::pair<std::string, std::string>{"ietf-restconf", "2017-01-26"}, {"ietf-restconf-monitoring", "2017-01-26"}, {"ietf-netconf", ""}}) {
        if (!conn.sessionStart().getContext().getModuleImplemented(module)) {
            throw std::runtime_error("Module "s + module + "@" + version + " is not implemented in sysrepo");
        }
    }

    // RFC 8527, we must implement at least ietf-yang-library@2019-01-04 and support operational DS
    if (auto mod = conn.sessionStart().getContext().getModuleImplemented("ietf-yang-library"); !mod || mod->revision() < "2019-01-04") {
        throw std::runtime_error("Module ietf-yang-library of revision at least 2019-01-04 is not implemented");
    }

    // set capabilities
    m_monitoringSession.setItem("/ietf-restconf-monitoring:restconf-state/capabilities/capability[1]", "urn:ietf:params:restconf:capability:defaults:1.0?basic-mode=explicit");
    m_monitoringSession.setItem("/ietf-restconf-monitoring:restconf-state/capabilities/capability[2]", "urn:ietf:params:restconf:capability:depth:1.0");
    m_monitoringSession.setItem("/ietf-restconf-monitoring:restconf-state/capabilities/capability[3]", "urn:ietf:params:restconf:capability:with-defaults:1.0");
    m_monitoringSession.applyChanges();

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

                auto restconfRequest = asRestconfRequest(sess.getContext(), req.method(), req.uri().path, req.uri().raw_query);

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

                case RestconfRequest::Type::GetData: {
                    sess.switchDatastore(restconfRequest.datastore.value_or(sysrepo::Datastore::Operational));

                    int maxDepth = 0; /* unbounded depth is the RFC default, which in sysrepo terms is 0 */
                    if (auto it = restconfRequest.queryParams.find("depth"); it != restconfRequest.queryParams.end() && std::holds_alternative<unsigned int>(it->second)) {
                        maxDepth = std::get<unsigned int>(it->second);
                    }

                    std::optional<queryParams::QueryParamValue> withDefaults;
                    if (auto it = restconfRequest.queryParams.find("with-defaults"); it != restconfRequest.queryParams.end()) {
                        withDefaults = it->second;
                    }

                    sysrepo::GetOptions getOptions = sysrepo::GetOptions::Default; /* default get options: return all nodes */
                    if (auto it = restconfRequest.queryParams.find("content"); it != restconfRequest.queryParams.end()) {
                        if(std::holds_alternative<queryParams::content::OnlyNonConfigNodes>(it->second)) {
                            getOptions = sysrepo::GetOptions::OperNoConfig;
                        } else if(std::holds_alternative<queryParams::content::OnlyConfigNodes>(it->second)) {
                            getOptions = sysrepo::GetOptions::OperNoState;
                        }
                    }

                    if (auto data = sess.getData(restconfRequest.path, maxDepth, getOptions); data) {
                        res.write_head(
                            200,
                            {
                                {"content-type", {asMimeType(dataFormat.response), false}},
                                {"access-control-allow-origin", {"*", false}},
                            });

                        res.end(*replaceYangLibraryLocations(http::parseUrlPrefix(req.header()), yangSchemaRoot, *data).printStr(dataFormat.response, libyangPrintFlags(*data, restconfRequest.path, withDefaults)));
                    } else {
                        throw ErrorResponse(404, "application", "invalid-value", "No data from sysrepo.");
                    }
                    break;
                }

                case RestconfRequest::Type::CreateOrReplaceThisNode:
                case RestconfRequest::Type::CreateChildren: {
                    if (restconfRequest.datastore == sysrepo::Datastore::FactoryDefault || restconfRequest.datastore == sysrepo::Datastore::Operational) {
                        throw ErrorResponse(405, "application", "operation-not-supported", "Read-only datastore.");
                    }

                    sess.switchDatastore(restconfRequest.datastore.value_or(sysrepo::Datastore::Running));
                    if (!dataFormat.request) {
                        throw ErrorResponse(400, "protocol", "invalid-value", "Content-type header missing.");
                    }

                    auto requestCtx = std::make_shared<RequestContext>(req, res, dataFormat, sess, restconfRequest);

                    req.on_data([requestCtx, restconfRequest /* intentional copy */](const uint8_t* data, std::size_t length) {
                        if (length > 0) { // there are still some data to be read
                            requestCtx->payload.append(reinterpret_cast<const char*>(data), length);
                            return;
                        }

                        if (restconfRequest.type == RestconfRequest::Type::CreateOrReplaceThisNode) {
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
                            /* The RFC is not clear at all on the error-tag.
                             * See https://mailarchive.ietf.org/arch/msg/netconf/XcF9r3ek3LvZ4DjF-7_B8kxuiwA/
                             * Also, if we replace 403 with 404 in order not to reveal if the node does not exist or if the user is not authorized
                             * then we should return the error tag invalid-value. This clashes with the data-missing tag below and we reveal it anyway :(
                             */
                            throw ErrorResponse(404, "application", "data-missing", "Data is missing.", restconfRequest.path);
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
                    auto requestCtx = std::make_shared<RequestContext>(req, res, dataFormat, sess, restconfRequest);

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
