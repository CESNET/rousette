/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <experimental/iterator>
#include <libyang-cpp/Enum.hpp>
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2_server.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Enum.hpp>
#include <sysrepo-cpp/Subscription.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include "http/utils.hpp"
#include "restconf/Exceptions.h"
#include "restconf/NotificationStream.h"
#include "auth/Http.h"
#include "restconf/Server.h"
#include "restconf/YangSchemaLocations.h"
#include "restconf/uri.h"
#include "restconf/utils/dataformat.h"
#include "restconf/utils/yang.h"
#include "sr/OpticalEvents.h"

using namespace std::literals;

using nghttp2::asio_http2::server::request;
using nghttp2::asio_http2::server::response;

#define CORS {"access-control-allow-origin", {"*", false}}
#define TEXT_PLAIN contentType("text/plain")
#define ALLOW_GET_HEAD_OPTIONS {"allow", {"GET, HEAD, OPTIONS", false}}

namespace rousette::restconf {

namespace {
constexpr auto notifPrefix = R"json({"ietf-restconf:notification":{"eventTime":")json";
constexpr auto notifMid = R"json(","ietf-yang-push:push-update":{"datastore-contents":)json";
constexpr auto notifSuffix = R"json(}}})json";

void logRequest(const auto& request)
{
    const auto& peer = http::peer_from_request(request);
    spdlog::info("{}: {} {}", peer, request.method(), request.uri().raw_path);
    for (const auto& hdr: request.header()) {
        spdlog::trace("{}: header: {}: {}", peer, hdr.first, hdr.second.sensitive ? "<sensitive>"s : hdr.second.value);
    }
}

template <typename T>
auto as_restconf_push_update(const std::string& content, const T& time)
{
    return notifPrefix + libyang::yangTimeFormat(time, libyang::TimezoneInterpretation::Local) + notifMid + content + notifSuffix;
}

constexpr auto restconfRoot = "/restconf/";
constexpr auto yangSchemaRoot = "/yang/";
constexpr auto netconfStreamRoot = "/streams/";

bool isSameNode(const libyang::DataNode& child, const PathSegment& lastPathSegment)
{
    return child.schema().module().name() == *lastPathSegment.apiIdent.prefix && child.schema().name() == lastPathSegment.apiIdent.identifier;
}

/** @brief Construct HTTP headers related to responses to OPTIONS requests */
nghttp2::asio_http2::header_map httpOptionsHeaders(const std::set<std::string>& allowedHttpMethods)
{
    nghttp2::asio_http2::header_map headers;
    std::ostringstream oss;
    std::copy(std::begin(allowedHttpMethods), std::end(allowedHttpMethods), std::experimental::make_ostream_joiner(oss, ", "));
    headers.emplace("allow", nghttp2::asio_http2::header_value{oss.str(), false});

    if (allowedHttpMethods.contains("PATCH")) {
        headers.emplace("accept-patch", nghttp2::asio_http2::header_value{"application/yang-data+json, application/yang-data+xml, application/yang-patch+xml, application/yang-patch+json", false});
    }

    return headers;
}

constexpr nghttp2::asio_http2::header_map::value_type contentType(const std::string& mimeType)
{
    return {"content-type", {mimeType, false}};
}

auto contentType(const libyang::DataFormat dataFormat)
{
    return contentType(asMimeType(dataFormat));
}

/** @brief Rejects the request with an error response and sends the HTTP response. Recommend to use rejectWithError which has more convenient API.
 * @pre The error errorContainer must be a node from ietf-restconf module, grouping "errors", container "errors".
 * */
void rejectWithErrorImpl(libyang::Context ctx, const libyang::DataFormat& dataFormat, const libyang::DataNode& parent, libyang::DataNode& errorContainer, const request& req, const response& res, const int code, const std::string errorType, const std::string& errorTag, const std::string& errorMessage, const std::optional<std::string>& errorPath)
{
    spdlog::debug("{}: Rejected with {}: {}", http::peer_from_request(req), errorTag, errorMessage);

    errorContainer.newPath("error[1]/error-type", errorType);
    errorContainer.newPath("error[1]/error-tag", errorTag);
    errorContainer.newPath("error[1]/error-message", errorMessage);

    if (errorPath) {
        errorContainer.newPath("error[1]/error-path", *errorPath);
    }

    nghttp2::asio_http2::header_map headers = {contentType(dataFormat), CORS};

    if (code == 405) {
        headers.merge(httpOptionsHeaders(allowedHttpMethodsForUri(ctx, req.uri().path)));
    }

    res.write_head(code, headers);
    res.end(*parent.printStr(dataFormat, libyang::PrintFlags::WithSiblings));
}

void rejectWithError(libyang::Context ctx, const libyang::DataFormat& dataFormat, const request& req, const response& res, const int code, const std::string errorType, const std::string& errorTag, const std::string& errorMessage, const std::optional<std::string>& errorPath)
{
    auto ext = ctx.getModuleImplemented("ietf-restconf")->extensionInstance("yang-errors");
    auto errors = *ctx.newExtPath(ext, "/ietf-restconf:errors", std::nullopt);
    rejectWithErrorImpl(ctx, dataFormat, errors, errors, req, res, code, errorType, errorTag, errorMessage, errorPath);
}

/** @short RFC 8072, the request was complete enough to read the patch-id, but there's no known edit-id */
auto rejectYangPatch(const std::string& patchId)
{
    return [patchId](libyang::Context ctx,
                     const libyang::DataFormat& dataFormat,
                     const request& req,
                     const response& res,
                     const int code,
                     const std::string errorType,
                     const std::string& errorTag,
                     const std::string& errorMessage,
                     const std::optional<std::string>& errorPath) {
        auto ext = ctx.getModuleImplemented("ietf-yang-patch")->extensionInstance("yang-patch-status");
        auto errorsTree = *ctx.newExtPath(ext, "/ietf-yang-patch:yang-patch-status/errors", std::nullopt);
        auto errorsContainer = *errorsTree.findXPath("/ietf-yang-patch:yang-patch-status/errors").begin();
        errorsTree.newPath("patch-id", patchId);
        rejectWithErrorImpl(ctx, dataFormat, errorsTree, errorsContainer, req, res, code, errorType, errorTag, errorMessage, errorPath);
    };
}

/** @short RFC 8072, both patch-id and edit-id are known */
auto rejectYangPatch(const std::string& patchId, const std::string& editId)
{
    return [patchId, editId](libyang::Context ctx,
                             const libyang::DataFormat& dataFormat,
                             const request& req,
                             const response& res,
                             const int code,
                             const std::string errorType,
                             const std::string& errorTag,
                             const std::string& errorMessage,
                             const std::optional<std::string>& errorPath) {
        const auto errorContainerXPath = "/ietf-yang-patch:yang-patch-status/edit-status/edit[edit-id='" + editId + "']/errors";
        auto ext = ctx.getModuleImplemented("ietf-yang-patch")->extensionInstance("yang-patch-status");
        auto errorsTree = *ctx.newExtPath(ext, errorContainerXPath, std::nullopt);
        auto errorsContainer = *errorsTree.findXPath(errorContainerXPath).begin();
        errorsTree.newPath("patch-id", patchId);
        rejectWithErrorImpl(ctx, dataFormat, errorsTree, errorsContainer, req, res, code, errorType, errorTag, errorMessage, errorPath);
    };
}

/** @brief Check if these two paths compare the same after path canonicalization */
bool compareKeyValue(const libyang::Context& ctx, const std::string& pathA, const std::string& pathB)
{
    auto [parentA, nodeA] = ctx.newPath2(pathA, std::nullopt);
    auto [parentB, nodeB] = ctx.newPath2(pathB, std::nullopt);

    return nodeA->asTerm().valueStr() == nodeB->asTerm().valueStr();
}

struct KeyMismatch {
    libyang::DataNode offendingNode;
    std::optional<std::string> uriKeyValue;

    std::string message() const {
        if (uriKeyValue) {
            return "List key mismatch between URI path ('"s + *uriKeyValue + "') and data ('" + offendingNode.asTerm().valueStr() + "').";
        } else {
            return "List key mismatch (key missing in the data).";
        }
    }
};

/** @brief In case node is a (leaf-)list check if the key values are the same as the keys specified in the lastPathSegment.
 * @return The node where the mismatch occurs */
std::optional<KeyMismatch> checkKeysMismatch(libyang::Context& ctx, const libyang::DataNode& node, const std::string& lyParentPath, const PathSegment& lastPathSegment)
{
    const auto pathPrefix = (lyParentPath.empty() ? "" : lyParentPath) + "/" + lastPathSegment.apiIdent.name();

    if (node.schema().nodeType() == libyang::NodeType::List) {
        const auto& listKeys = node.schema().asList().keys();
        for (size_t i = 0; i < listKeys.size(); ++i) {
            auto keyNodeData = node.findPath(listKeys[i].module().name() + ':' + listKeys[i].name());
            if (!keyNodeData) {
                return KeyMismatch{node, std::nullopt};
            }

            /*
             * If the key's value has a canonical form then libyang makes the value canonical
             * but there is no guarantee that the client provided the value in the canonical form.
             *
             * Let libyang do the work. Create two data nodes, one with the key value from the data and the other
             * with the key value from the URI. Then compare the values from the two nodes. If they are different,
             * they certainly mismatch.
             *
             * This can happen in cases like
             *  * The key's type is identityref and the client provided the key value as a string without the module name. Libyang will canonicalize the value by adding the module name.
             *  * The key's type is decimal64 with fractional-digits 2; then the client can provide the value as 1.0 or 1.00 and they should be the same. Libyang will canonicalize the value.
             */

            auto keysWithValueFromData = lastPathSegment.keys;
            keysWithValueFromData[i] = keyNodeData->asTerm().valueStr();

            const auto suffix = "/" + listKeys[i].name();
            const auto pathFromData = pathPrefix + listKeyPredicate(listKeys, keysWithValueFromData) + suffix;
            const auto pathFromURI = pathPrefix + listKeyPredicate(listKeys, lastPathSegment.keys) + suffix;

            if (!compareKeyValue(ctx, pathFromData, pathFromURI)) {
                return KeyMismatch{*keyNodeData, lastPathSegment.keys[i]};
            }
        }
    } else if (node.schema().nodeType() == libyang::NodeType::Leaflist) {
        const auto pathFromData = pathPrefix + leaflistKeyPredicate(node.asTerm().valueStr());
        const auto pathFromURI = pathPrefix + leaflistKeyPredicate(lastPathSegment.keys[0]);
        if (!compareKeyValue(ctx, pathFromData, pathFromURI)) {
            return KeyMismatch{node, lastPathSegment.keys[0]};
        }
    }
    return std::nullopt;
}

auto childLeafValue(const libyang::DataNode& node, const std::string& childName)
{
    if (auto child = node.findPath(childName); child && child->isTerm()) {
        return child->asTerm().valueStr();
    }
    throw ErrorResponse(400, "protocol", "invalid-value", "Expected data node '" + childName + "' not found.");
}

struct RequestContext {
    const nghttp2::asio_http2::server::request& req;
    const nghttp2::asio_http2::server::response& res;
    DataFormat dataFormat;
    sysrepo::Session sess;
    RestconfRequest restconfRequest;
    std::string payload;
};

void yangInsert(const libyang::Context& ctx, libyang::DataNode& listEntryNode, const std::string& where, const std::optional<queryParams::insert::PointParsed>& point)
{
    auto modYang = *ctx.getModuleImplemented("yang");

    if (!isUserOrderedList(listEntryNode)) {
        throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter 'insert' is valid only for inserting into lists or leaf-lists that are 'ordered-by user'");
    }

    listEntryNode.newMeta(modYang, "insert", where);

    if (point) {
        const auto listEntrySchema = listEntryNode.schema();
        std::string key;

        if (listEntrySchema != asLibyangSchemaNode(ctx, *point)) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter 'point' contains path to a different list", listEntryNode.path());
        }

        if (listEntrySchema.nodeType() == libyang::NodeType::List) {
            key = listKeyPredicate(listEntrySchema.asList().keys(), point->back().keys);
        } else if (listEntrySchema.nodeType() == libyang::NodeType::Leaflist) {
            key = point->back().keys[0];
        } else {
            throw std::logic_error("Node is neither a list nor a leaf-list");
        }

        listEntryNode.newMeta(modYang, listEntryNode.schema().nodeType() == libyang::NodeType::List ? "key" : "value", key);
    }
}

void yangInsert(const RequestContext& requestCtx, libyang::DataNode& listEntryNode)
{
    auto it = requestCtx.restconfRequest.queryParams.find("insert");
    if (it == requestCtx.restconfRequest.queryParams.end()) {
        return;
    }

    std::string where;
    std::optional<queryParams::insert::PointParsed> point;

    if (std::holds_alternative<queryParams::insert::First>(it->second)) {
        where = "first";
    } else if (std::holds_alternative<queryParams::insert::Last>(it->second)) {
        where = "last";
    } else if (auto hasBefore = std::holds_alternative<queryParams::insert::Before>(it->second); hasBefore || std::holds_alternative<queryParams::insert::After>(it->second)) {
        where = hasBefore ? "before" : "after";
        point = std::get<queryParams::insert::PointParsed>(requestCtx.restconfRequest.queryParams.find("point")->second);
    }

    yangInsert(requestCtx.sess.getContext(), listEntryNode, where, point);
}

void yangInsert(const libyang::Context& ctx, libyang::DataNode& listEntryNode, std::string& where, const std::optional<std::string>& point)
{
    std::optional<std::vector<PathSegment>> pointParsed;
    if (point) {
        pointParsed = asPathSegments(*point);
    }

    yangInsert(ctx, listEntryNode, where, pointParsed);
}

/** @brief Rejects the edit if any edit node has meta attributes that could possibly alter sysrepo's behaviour. */
void validateInputMetaAttributes(const libyang::Context& ctx, const libyang::DataNode& tree)
{
    const auto modNetconf = *ctx.getModuleLatest("ietf-netconf");
    const auto modYang = *ctx.getModuleLatest("yang");
    const auto modSysrepo = *ctx.getModuleLatest("sysrepo");

    for (const auto& node : tree.childrenDfs()) {
        for (const auto& meta : node.meta()) {
            if (!meta.isInternal() && (meta.module() == modNetconf || meta.module() == modYang || meta.module() == modSysrepo)) {
                throw ErrorResponse(400, "application", "invalid-value", "Meta attribute '" + meta.module().name() + ":" + meta.name() + "' not allowed.", node.path());
            }
        }
    }
}

template<typename T, typename U>
constexpr auto withRestconfExceptions(T func, U rejectWithError)
{
    return [=](std::shared_ptr<RequestContext> requestCtx, auto&& ...args)
    {
        try {
            func(requestCtx, std::forward<decltype(args)>(args)...);
        } catch (const ErrorResponse& e) {
            rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, e.code, e.errorType, e.errorTag, e.errorMessage, e.errorPath);
        } catch (const libyang::ErrorWithCode& e) {
            if (e.code() == libyang::ErrorCode::ValidationFailure) {
                rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 400, "protocol", "invalid-value", "Validation failure: "s + e.what(), std::nullopt);
            } else {
                rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 500, "application", "operation-failed", "Internal server error due to libyang exception: "s + e.what(), std::nullopt);
            }
        } catch (const sysrepo::ErrorWithCode& e) {
            if (e.code() == sysrepo::ErrorCode::Unauthorized) {
                rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 403, "application", "access-denied", "Access denied.", std::nullopt);
            } else if (e.code() == sysrepo::ErrorCode::NotFound) {
                rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 400, "protocol", "invalid-value", e.what(), std::nullopt);
            } else if (e.code() == sysrepo::ErrorCode::ItemAlreadyExists) {
                rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 409, "application", "resource-denied", "Resource already exists.", std::nullopt);
            } else if (e.code() == sysrepo::ErrorCode::ValidationFailed) {
                bool isAction = requestCtx->restconfRequest.path != "/" && requestCtx->sess.getContext().findPath(requestCtx->restconfRequest.path).nodeType() == libyang::NodeType::Action;
                /*
                 * FIXME: This happens on invalid input data (e.g., missing mandatory nodes) or missing action data node.
                 * The former (invalid input data) should probably be validated by libyang's parseOp but it only parses.
                 * Is there better way? At least somehow extract logs? We can check if the action node exists before
                 * sending the RPC but that is racy because two sysrepo operations must be done (query + rpc) and
                 * operational DS cannot be locked.
                 */
                rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 400, "application", "operation-failed",
                        "Validation failed. Invalid input data"s + (isAction ? " or the action node is not present" : "") + ".", std::nullopt);
            } else {
                rejectWithError(requestCtx->sess.getContext(), requestCtx->dataFormat.response, requestCtx->req, requestCtx->res, 500, "application", "operation-failed",
                        "Internal server error due to sysrepo exception: "s + e.what(), std::nullopt);
            }
        }
    };
}

#define WITH_RESTCONF_EXCEPTIONS(FUNC, REJECT_FUNC) withRestconfExceptions<decltype(FUNC)>(FUNC, REJECT_FUNC)

/** @brief Prepare sysrepo edit for PUT and PATCH (both PLAIN and YANG) requests from uri and string data.
 *
 * @return A pair of the edit tree and a node that should be replaced (i.e., the NETCONF operation is set on it).
 */
libyang::CreatedNodes createEditForPutAndPatch(libyang::Context& ctx, const std::string& uriPath, const std::optional<std::string>& valueStr, const libyang::DataFormat& dataFormat)
{
    std::optional<libyang::DataNode> editNode;
    std::optional<libyang::DataNode> replacementNode;

    /* PUT and PATCH requests replace the node indicated by the URI path with the tree provided in the request body.
     * The tree starts with the node indicated by the URI.
     * This means that in libyang, we must create the parent node of the URI path and parse the data into it.
     */
    auto [lyParentPath, lastPathSegment] = asLibyangPathSplit(ctx, uriPath);

    if (!valueStr) {
        // Some YANG patch operations do not have a value node, e.g., delete or move
        auto lyFullPath = asRestconfRequest(ctx, "PATCH", uriPath).path;
        auto [parent, node] = ctx.newPath2(lyFullPath);
        editNode = parent;
        replacementNode = node;
    } else if (!lyParentPath.empty()) {
        // the node that we're working on has a parent, i.e., the URI path is at least two levels deep
        auto [parent, node] = ctx.newPath2(lyParentPath);
        node->parseSubtree(*valueStr, dataFormat, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);

        for (const auto& child : node->immediateChildren()) {
            // Anything directly below `node` is either:
            if (isSameNode(child, lastPathSegment)) {
                // 1) a single child that is created by parseSubtree(), its name is the same as `lastPathSegment`.
                // It could be a list; then we need to check if the keys in provided data match the keys in URI.
                if (auto keyMismatch = checkKeysMismatch(ctx, child, lyParentPath, lastPathSegment)) {
                    throw ErrorResponse(400, "protocol", "invalid-value", keyMismatch->message(), keyMismatch->offendingNode.path());
                }
                replacementNode = child;
            } else if (isKeyNode(*node, child)) {
                // 2) or a list key (of the lyParentPath) that was created by newPath2 call.
                // Do nothing here; key values are checked elsewhere
            } else {
                // 3) Anything else is an error (either too many children provided or invalid name)
                throw ErrorResponse(400, "protocol", "invalid-value", "Data contains invalid node.", child.path());
            }
        }

        editNode = parent;
    } else {
        // URI path points to a top-level node
        if (auto parent = ctx.parseData(*valueStr, dataFormat, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly); parent) {
            editNode = parent;
            replacementNode = parent;

            if (!isSameNode(*replacementNode, lastPathSegment)) {
                throw ErrorResponse(400, "protocol", "invalid-value", "Data contains invalid node.", replacementNode->path());
            }

            if (auto keyMismatch = checkKeysMismatch(ctx, *parent, lyParentPath, lastPathSegment)) {
                throw ErrorResponse(400, "protocol", "invalid-value", keyMismatch->message(), keyMismatch->offendingNode.path());
            }
        }
    }

    if (!replacementNode) {
        throw ErrorResponse(400, "protocol", "invalid-value", "Node indicated by URI is missing.");
    }

    return {editNode, replacementNode};
}

std::optional<libyang::DataNode> processInternalRPC(sysrepo::Session&, const libyang::DataNode&)
{
    // TODO: Implement internal RPCs
    throw ErrorResponse(501, "application", "operation-not-supported", "Internal RPCs are not yet supported.");
}

void processActionOrRPC(std::shared_ptr<RequestContext> requestCtx, const std::chrono::milliseconds timeout)
{
    requestCtx->sess.switchDatastore(sysrepo::Datastore::Operational);
    auto ctx = requestCtx->sess.getContext();

    auto rpcSchemaNode = ctx.findPath(requestCtx->restconfRequest.path);
    if (!requestCtx->dataFormat.request && static_cast<bool>(rpcSchemaNode.asActionRpc().input().child())) {
        throw ErrorResponse(400, "protocol", "invalid-value", "Content-type header missing.");
    }

    // check if action node's parent is present
    if (rpcSchemaNode.nodeType() == libyang::NodeType::Action) {
        /*
         * This is race-prone:
         *  - The data node exists but might get deleted right after this check: Sysrepo throws an error when this happens.
         *  - The data node does not exist but might get created right after this check: The node was not there when the request was issues so it should not be a problem
         */
        auto [pathToParent, pathSegment] = asLibyangPathSplit(ctx, requestCtx->req.uri().raw_path);
        if (!requestCtx->sess.getData(pathToParent, 0, sysrepo::GetOptions::Default, timeout)) {
            throw ErrorResponse(400, "application", "operation-failed", "Action data node '" + requestCtx->restconfRequest.path + "' does not exist.");
        }
    }


    auto [parent, rpcNode] = ctx.newPath2(requestCtx->restconfRequest.path);

    if (!requestCtx->payload.empty()) {
        rpcNode->parseOp(requestCtx->payload, *requestCtx->dataFormat.request, libyang::OperationType::RpcRestconf);
    }

    std::optional<libyang::DataNode> rpcReply;
    if (requestCtx->restconfRequest.type == RestconfRequest::Type::Execute) {
        rpcReply = requestCtx->sess.sendRPC(*rpcNode, timeout);
    } else if (requestCtx->restconfRequest.type == RestconfRequest::Type::ExecuteInternal) {
        rpcReply = processInternalRPC(requestCtx->sess, *rpcNode);
    }

    if (!rpcReply || rpcReply->immediateChildren().empty()) {
        requestCtx->res.write_head(204, {CORS});
        requestCtx->res.end();
        return;
    }

    auto responseNode = rpcReply->child();
    responseNode->unlinkWithSiblings();

    // libyang auto-resolves the XML namespace when the result is printed into XML
    auto envelope = ctx.newOpaqueJSON({rpcNode->schema().module().name(), rpcNode->schema().module().name(), "output"}, std::nullopt);
    envelope->insertChild(*responseNode);

    requestCtx->res.write_head(200, {
                                        contentType(requestCtx->dataFormat.response),
                                        CORS,
                                    });
    requestCtx->res.end(*envelope->printStr(requestCtx->dataFormat.response, libyang::PrintFlags::WithSiblings | libyang::PrintFlags::KeepEmptyCont));
}

void processPost(std::shared_ptr<RequestContext> requestCtx, const std::chrono::milliseconds timeout)
{
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

    if (edit) {
        validateInputMetaAttributes(ctx, *edit);
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

    createdNodes.begin()->newMeta(*modNetconf, "operation", "create");
    yangInsert(*requestCtx, *createdNodes.begin());

    requestCtx->sess.editBatch(*edit, sysrepo::DefaultOperation::Merge);
    requestCtx->sess.applyChanges(timeout);

    requestCtx->res.write_head(201,
                               {
                                   contentType(requestCtx->dataFormat.response),
                                   CORS,
                                   // FIXME: POST data operation MUST return Location header
                               });
    requestCtx->res.end();
}

/** @brief Return the JSON serialization of the value node
 *
 * Parsed ext data (e.g., yang-data container) are opaque nodes. However, in yang-patch we know that these data
 * should conform to a schema. Libyang can not "promote" such nodes to standard data nodes, so we need to serialize
 * them and parse them again.
 */
std::optional<std::string> yangPatchValueAsJSON(const libyang::DataNode& editContainer)
{
    if (auto valueAnyNode = editContainer.findPath("value")) {
        try {
            // if the value is present, we expect it to be a DataNode, not JSON/XML or any other stuff
            auto valueDataNode = std::get<libyang::DataNode>(valueAnyNode->asAny().releaseValue().value());
            return *valueDataNode.printStr(libyang::DataFormat::JSON, libyang::PrintFlags::Shrink);
        } catch (const std::bad_variant_access&) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Not a data node", valueAnyNode->path());
        }
    }

    return std::nullopt;
}

void processYangPatchEdit(const std::shared_ptr<RequestContext>& requestCtx, const libyang::DataNode& editContainer, std::optional<libyang::DataNode>& mergedEdits)
{
    auto ctx = requestCtx->sess.getContext();
    auto netconfMod = *ctx.getModuleImplemented("ietf-netconf");

    auto target = childLeafValue(editContainer, "target");
    auto operation = childLeafValue(editContainer, "operation");

    auto [singleEdit, replacementNode] = createEditForPutAndPatch(ctx, requestCtx->req.uri().raw_path + target, yangPatchValueAsJSON(editContainer), libyang::DataFormat::JSON);
    validateInputMetaAttributes(ctx, *singleEdit);

    // insert and move are not defined in RFC6241. sec 7.3 and sysrepo does not support them directly
    if (operation == "insert" || operation == "move") {
        std::string where = childLeafValue(editContainer, "where");
        std::optional<std::string> point;

        auto pointNode = editContainer.findPath("point");

        if (where == "before" || where == "after") {
            if (!pointNode) {
                throw ErrorResponse(400, "protocol", "invalid-value", "Required leaf 'point' not set.");
            }

            point = requestCtx->req.uri().path + pointNode->asTerm().valueStr();
        } else if (pointNode) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Leaf 'point' must always come with leaf 'where' set to 'before' or 'after'");
        }

        replacementNode->newMeta(netconfMod, "operation", operation == "insert" ? "create" : "merge");
        yangInsert(ctx, *replacementNode, where, point);
    } else {
        replacementNode->newMeta(netconfMod, "operation", operation);
    }

    if (!mergedEdits) {
        mergedEdits = *singleEdit;
    } else {
        mergedEdits = mergedEdits->insertSibling(*singleEdit); // make sure we point to the first sibling, sysrepo::editBatch requires that
    }
}

/** @short RFC 8072 "YANG patch" processing once the patch-id is known */
void processYangPatchImpl(const std::shared_ptr<RequestContext>& requestCtx, const libyang::DataNode& patch, const std::string& patchId, const std::chrono::milliseconds timeout)
{
    // create one big edit from all the edits because we need to apply all at once.
    std::optional<libyang::DataNode> mergedEdits;

    for (const auto& editContainer : patch.findXPath("edit")) {
        auto editId = childLeafValue(editContainer, "edit-id");

        // errors while processing a single edit are reported in the edit-status container
        WITH_RESTCONF_EXCEPTIONS(processYangPatchEdit, rejectYangPatch(patchId, editId))(requestCtx, editContainer, mergedEdits);
    }

    if (mergedEdits) {
        requestCtx->sess.editBatch(*mergedEdits, sysrepo::DefaultOperation::Merge);
        requestCtx->sess.applyChanges(timeout);
    }
}

void processYangPatch(std::shared_ptr<RequestContext> requestCtx, const std::chrono::milliseconds timeout)
{
    auto ctx = requestCtx->sess.getContext();
    auto yangPatchMod = *ctx.getModule("ietf-yang-patch", "2017-02-22");
    auto yangPatchExt = yangPatchMod.extensionInstance("yang-patch");
    auto yangPatchStatusExt = yangPatchMod.extensionInstance("yang-patch-status");

    auto patch = ctx.parseExtData(yangPatchExt, requestCtx->payload, *requestCtx->dataFormat.request, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);
    if (!patch) {
        throw ErrorResponse(400, "protocol", "invalid-value", "Empty patch.");
    }

    // now we have patch-id so we can respond to errors with yang-patch-status
    auto patchId = childLeafValue(*patch, "patch-id");
    WITH_RESTCONF_EXCEPTIONS(processYangPatchImpl, rejectYangPatch(patchId))(requestCtx, *patch, patchId, timeout);

    // everything went well
    auto yangPatchStatus = ctx.newExtPath(yangPatchStatusExt, "/ietf-yang-patch:yang-patch-status", std::nullopt);
    yangPatchStatus->newExtPath(yangPatchStatusExt, "/ietf-yang-patch:yang-patch-status/patch-id", patchId);
    yangPatchStatus->newExtPath(yangPatchStatusExt, "/ietf-yang-patch:yang-patch-status/ok", std::nullopt);

    requestCtx->res.write_head(200, {contentType(requestCtx->dataFormat.response), CORS});
    requestCtx->res.end(*yangPatchStatus->printStr(requestCtx->dataFormat.response, libyang::PrintFlags::WithSiblings));
}

void processPutOrPlainPatch(std::shared_ptr<RequestContext> requestCtx, const std::chrono::milliseconds timeout)
{
    auto ctx = requestCtx->sess.getContext();

    // PUT / means replace everything. PATCH / means merge into datastore. Also, asLibyangPathSplit() won't do the right thing on "/".
    if (requestCtx->restconfRequest.path == "/") {
        auto edit = ctx.parseData(requestCtx->payload, *requestCtx->dataFormat.request, libyang::ParseOptions::Strict | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);
        if (!edit) {
            throw ErrorResponse(400, "protocol", "malformed-message", "Empty data tree received.");
        }

        validateInputMetaAttributes(ctx, *edit);

        if (requestCtx->req.method() == "PUT") {
            requestCtx->sess.replaceConfig(edit, std::nullopt, timeout);

            requestCtx->res.write_head(edit ? 201 : 204, {CORS});
        } else {
            requestCtx->sess.editBatch(*edit, sysrepo::DefaultOperation::Merge);
            requestCtx->sess.applyChanges(timeout);
            requestCtx->res.write_head(204, {CORS});
        }
        requestCtx->res.end();
        return;
    }

    // The HTTP status code for PUT depends on whether the node already existed before the operation.
    // To prevent a race when someone else creates the node while this request is being processed,
    // this needs locking.
    std::unique_ptr<sysrepo::Lock> lock;
    if (requestCtx->sess.activeDatastore() != sysrepo::Datastore::Candidate) {
        // ...except that the candidate DS in sysrepo rolls back on unlock, so we cannot take that lock.
        // So, there's a race when modifying the candidate DS.
        lock = std::make_unique<sysrepo::Lock>(requestCtx->sess);
    }

    bool nodeExisted = !!requestCtx->sess.getData(requestCtx->restconfRequest.path, 0, sysrepo::GetOptions::Default, timeout);

    if (requestCtx->req.method() == "PATCH" && !nodeExisted) {
        throw ErrorResponse(400, "protocol", "invalid-value", "Target resource does not exist");
    }

    auto [edit, replacementNode] = createEditForPutAndPatch(ctx, requestCtx->req.uri().raw_path, requestCtx->payload, *requestCtx->dataFormat.request /* caller checks if the dataFormat.request is present */);
    validateInputMetaAttributes(ctx, *edit);

    if (requestCtx->req.method() == "PUT") {
        auto modNetconf = ctx.getModuleImplemented("ietf-netconf");
        replacementNode->newMeta(*modNetconf, "operation", "replace");
        yangInsert(*requestCtx, *replacementNode);
    }

    requestCtx->sess.editBatch(*edit, sysrepo::DefaultOperation::Merge);
    requestCtx->sess.applyChanges(timeout);

    if (requestCtx->req.method() == "PUT") {
        requestCtx->res.write_head(nodeExisted ? 204 : 201, {CORS});
    } else {
        requestCtx->res.write_head(204, {CORS});
    }

    requestCtx->res.end();
}

/** @brief Build data trees for endpoints returning ietf-restconf:restconf data */
libyang::DataNode apiResource(const libyang::Context& ctx, const RestconfRequest::Type& type, libyang::DataFormat dataFormat)
{
    const auto yangLib = *ctx.getModuleLatest("ietf-yang-library");
    const auto yangApiExt = ctx.getModuleImplemented("ietf-restconf")->extensionInstance("yang-api");
    auto parent = *ctx.newExtPath(yangApiExt, "/ietf-restconf:restconf", std::nullopt);

    if (type == RestconfRequest::Type::RestconfRoot || type == RestconfRequest::Type::YangLibraryVersion) {
        parent.newPath("yang-library-version", yangLib.revision());
    }

    if (type == RestconfRequest::Type::YangLibraryVersion) {
        // direct request at /restconf/yang-library-version return ONLY the yang-library-version node and nothing else (RFC 8040, sec. 3.3.3)
        return *parent.findPath("yang-library-version");
    } else if (type == RestconfRequest::Type::RestconfRoot) {
        parent.newPath("data");
        parent.newPath("operations");
    } else if (type == RestconfRequest::Type::ListRPC) {
        auto operations = *parent.newPath("operations");
        for (const auto& mod : ctx.modules()) {
            if (!mod.implemented()) {
                continue;
            }

            for (const auto& rpc : mod.actionRpcs()) {
                switch (dataFormat) {
                case libyang::DataFormat::JSON:
                    operations.insertChild(*ctx.newOpaqueJSON({rpc.module().name(), rpc.module().name(), rpc.name()}, libyang::JSON{"[null]"}));
                    break;
                case libyang::DataFormat::XML:
                    operations.insertChild(*ctx.newOpaqueXML({rpc.module().ns(), rpc.module().name(), rpc.name()}, std::nullopt));
                    break;
                default:
                    throw std::logic_error{"Invalid data format for apiResource()"};
                }
            }
        }
    } else {
        throw std::logic_error("Invalid restconf request type for handling within apiResource()");
    }

    return parent;
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

    // Be careful, we can get something like /* which is not a valid path. In other cases, the node should be valid
    // in the schema (we check that in the parser) but the actual data node might not be instantiated.
    try {
        node = dataNode.findPath(requestPath);
    } catch(const libyang::Error& e) {
    }

    libyang::PrintFlags ret = libyang::PrintFlags::WithSiblings | libyang::PrintFlags::KeepEmptyCont;

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

/* @brief Returns if the request should be treated as a YANG patch request */
bool isYangPatch(const nghttp2::asio_http2::server::request& req)
{
    auto it = req.header().find("content-type");
    return it != req.header().end() && (it->second.value == "application/yang-patch+xml" || it->second.value == "application/yang-patch+json");
}
}

Server::~Server()
{
    stop();

    if (!joined) {
        server->join();
    }
}

void Server::stop()
{
    // notification to stop has to go through the asio io_context
    boost::asio::post(*server->io_services().front(), [server = this->server.get()]() {
        spdlog::trace("Stoping HTTP/2 server");
        server->stop();
    });

    shutdownRequested();
}

void Server::join()
{
    /* FIXME: nghttp2-asio is calling io.run() wrapped in std::async.
     * In case the handler function throws, the exception is not propagated to the main thread *until* someone calls server.join() which calls future.get() on all io.run() wrappers.
     * Thankfully, we have only one thread, so we can just call join() right away. Underlying future.get() blocks until io.run() finishes, either gracefully or upon uncaught exception.
     *
     * !!! Will not work server uses multiple threads !!!
     */

    // main thread waits here
    server->join();
    joined = true;
}

std::vector<std::shared_ptr<boost::asio::io_context>> Server::io_services() const
{
    return server->io_services();
}

Server::Server(sysrepo::Connection conn, const std::string& address, const std::string& port, const std::chrono::milliseconds timeout)
    : m_monitoringSession(conn.sessionStart(sysrepo::Datastore::Operational))
    , nacm(conn)
    , server{std::make_unique<nghttp2::asio_http2::server::http2>()}
    , dwdmEvents{std::make_unique<sr::OpticalEvents>(conn.sessionStart())}
{
    server->num_threads(1); // we only use one thread for the server, so we can call join() right away

    for (const auto& [module, version, features] : {
             std::tuple<std::string, std::string, std::vector<std::string>>{"ietf-restconf", "2017-01-26", {}},
             {"ietf-restconf-monitoring", "2017-01-26", {}},
             {"ietf-netconf", "", {}},
             {"ietf-yang-library", "2019-01-04", {}},
             {"ietf-yang-patch", "2017-02-22", {}},
             {"ietf-subscribed-notifications", "2019-09-09", {}},
             {"ietf-restconf-subscribed-notifications", "2019-11-17", {}},
         }) {
        if (auto mod = m_monitoringSession.getContext().getModuleImplemented(module)) {
            for (const auto& feature : features) {
                if (!mod->featureEnabled(feature)) {
                    throw std::runtime_error("Module "s + module + "@" + version + " does not implement feature " + feature);
                }
            }
        } else {
            throw std::runtime_error("Module "s + module + "@" + version + " is not implemented in sysrepo");
        }
    }

    // set capabilities
    m_monitoringSession.setItem("/ietf-restconf-monitoring:restconf-state/capabilities/capability[1]", "urn:ietf:params:restconf:capability:defaults:1.0?basic-mode=explicit");
    m_monitoringSession.setItem("/ietf-restconf-monitoring:restconf-state/capabilities/capability[2]", "urn:ietf:params:restconf:capability:depth:1.0");
    m_monitoringSession.setItem("/ietf-restconf-monitoring:restconf-state/capabilities/capability[3]", "urn:ietf:params:restconf:capability:with-defaults:1.0");
    m_monitoringSession.setItem("/ietf-restconf-monitoring:restconf-state/capabilities/capability[4]", "urn:ietf:params:restconf:capability:filter:1.0");
    m_monitoringSession.setItem("/ietf-restconf-monitoring:restconf-state/capabilities/capability[5]", "urn:ietf:params:restconf:capability:fields:1.0");
    m_monitoringSession.applyChanges();

    m_monitoringOperSub = m_monitoringSession.onOperGet(
        "ietf-restconf-monitoring", [](auto session, auto, auto, auto, auto, auto, auto& parent) {
            notificationStreamList(session, parent, netconfStreamRoot);
            return sysrepo::ErrorCode::Ok;
        },
        "/ietf-restconf-monitoring:restconf-state/streams/stream");

    dwdmEvents->change.connect([this](const std::string& content) {
        opticsChange(as_restconf_push_update(content, std::chrono::system_clock::now()));
    });

    server->handle("/", [](const auto& req, const auto& res) {
        logRequest(req);

        res.write_head(404, {TEXT_PLAIN, CORS});
        res.end();
    });

    server->handle("/.well-known/host-meta", [](const auto& req, const auto& res) {
        logRequest(req);

        res.write_head(
                   200,
                   {
                       contentType("application/xrd+xml"),
                       CORS,
                   });
        res.end("<XRD xmlns='http://docs.oasis-open.org/ns/xri/xrd-1.0'><Link rel='restconf' href='"s + restconfRoot + "'></XRD>"s);
    });

    server->handle("/telemetry/optics", [this](const auto& req, const auto& res) {
        logRequest(req);

        auto client = std::make_shared<http::EventStream>(req, res, shutdownRequested, opticsChange, as_restconf_push_update(dwdmEvents->currentData(), std::chrono::system_clock::now()));
        client->activate();
    });

    server->handle(netconfStreamRoot, [this, conn](const auto& req, const auto& res) mutable {
        logRequest(req);

        std::optional<std::string> xpathFilter;
        std::optional<sysrepo::NotificationTimeStamp> startTime;
        std::optional<sysrepo::NotificationTimeStamp> stopTime;

        if (req.method() == "OPTIONS") {
            res.write_head(200, {CORS, ALLOW_GET_HEAD_OPTIONS});
            res.end();
            return;
        }

        try {
            auto sess = conn.sessionStart();
            authorizeRequest(nacm, sess, req);

            auto streamRequest = asRestconfStreamRequest(req.method(), req.uri().path, req.uri().raw_query);

            if (auto it = streamRequest.queryParams.find("filter"); it != streamRequest.queryParams.end()) {
                xpathFilter = std::get<std::string>(it->second);
            }

            if (auto it = streamRequest.queryParams.find("start-time"); it != streamRequest.queryParams.end()) {
                startTime = libyang::fromYangTimeFormat<std::chrono::system_clock>(std::get<std::string>(it->second));
            }
            if (auto it = streamRequest.queryParams.find("stop-time"); it != streamRequest.queryParams.end()) {
                stopTime = libyang::fromYangTimeFormat<std::chrono::system_clock>(std::get<std::string>(it->second));
            }

            // The signal is constructed outside NotificationStream class because it is required to be passed to
            // NotificationStream's parent (EventStream) constructor where it already must be constructed
            // Yes, this is a hack.
            auto client = std::make_shared<NotificationStream>(req, res, shutdownRequested, std::make_shared<rousette::http::EventStream::EventSignal>(), sess, streamRequest.type.encoding, xpathFilter, startTime, stopTime);
            client->activate();
        } catch (const auth::Error& e) {
            processAuthError(req, res, e, [&res]() {
                res.write_head(401, {TEXT_PLAIN, CORS});
                res.end("Access denied.");
            });
        } catch (const ErrorResponse& e) {
            // RFC does not specify how the errors should look like so let's just report the HTTP code and print the error message
            nghttp2::asio_http2::header_map headers = {TEXT_PLAIN, CORS};

            if (e.code == 405) {
                headers.emplace(decltype(headers)::value_type ALLOW_GET_HEAD_OPTIONS);
            }

            res.write_head(e.code, headers);
            res.end(e.errorMessage);
        }
    });

    server->handle(yangSchemaRoot, [this, conn /* intentional copy */](const auto& req, const auto& res) mutable {
        logRequest(req);

        if (req.method() == "OPTIONS" || (req.method() != "GET" && req.method() != "HEAD")) {
            res.write_head(req.method() == "OPTIONS" ? 200 : 405, {CORS, ALLOW_GET_HEAD_OPTIONS});
            res.end();
            return;
        }

        try {
            auto sess = conn.sessionStart(sysrepo::Datastore::Operational);
            authorizeRequest(nacm, sess, req);

            if (auto mod = asYangModule(sess.getContext(), req.uri().path); mod && hasAccessToYangSchema(sess, *mod)) {
                res.write_head(
                    200,
                    {
                        contentType("application/yang"),
                        CORS,
                    });
                res.end(std::visit([](auto&& arg) { return arg.printStr(libyang::SchemaOutputFormat::Yang); }, *mod));
                return;
            } else {
                res.write_head(404, {TEXT_PLAIN, CORS});
                res.end("YANG schema not found");
            }
        } catch (const auth::Error& e) {
            processAuthError(req, res, e, [&res]() {
                res.write_head(401, {TEXT_PLAIN, CORS});
                res.end("Access denied.");
            });
        }
    });

    server->handle(restconfRoot,
        [conn /* intentionally by value, otherwise conn gets destroyed when the ctor returns */, this, timeout](const auto& req, const auto& res) mutable {
            logRequest(req);

            auto sess = conn.sessionStart(sysrepo::Datastore::Operational);
            DataFormat dataFormat;
            // default for "early exceptions" when the MIME type detection fails
            dataFormat.response = libyang::DataFormat::JSON;

            try {
                dataFormat = chooseDataEncoding(req.header());
                authorizeRequest(nacm, sess, req);

                auto restconfRequest = asRestconfRequest(sess.getContext(), req.method(), req.uri().raw_path, req.uri().raw_query);

                switch (restconfRequest.type) {
                case RestconfRequest::Type::RestconfRoot:
                case RestconfRequest::Type::YangLibraryVersion:
                case RestconfRequest::Type::ListRPC:
                    res.write_head(200, {contentType(dataFormat.response), CORS});
                    res.end(*apiResource(sess.getContext(), restconfRequest.type, dataFormat.response)
                                 .printStr(dataFormat.response, libyang::PrintFlags::WithSiblings | libyang::PrintFlags::KeepEmptyCont));
                    break;

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

                    auto xpath = restconfRequest.path;
                    if (auto it = restconfRequest.queryParams.find("fields"); it != restconfRequest.queryParams.end()) {
                        auto fields = std::get<queryParams::fields::Expr>(it->second);
                        xpath = fieldsToXPath(sess.getContext(), xpath == "/*" ? "" : xpath, fields);
                    }

                    if (auto data = sess.getData(xpath, maxDepth, getOptions, timeout); data) {
                        res.write_head(
                            200,
                            {
                                contentType(dataFormat.response),
                                CORS,
                            });

                        auto urlPrefix = http::parseUrlPrefix(req.header());
                        data = replaceYangLibraryLocations(urlPrefix, yangSchemaRoot, *data);
                        data = replaceStreamLocations(urlPrefix, *data);
                        res.end(*data->printStr(dataFormat.response, libyangPrintFlags(*data, restconfRequest.path, withDefaults)));
                    } else {
                        throw ErrorResponse(404, "application", "invalid-value", "No data from sysrepo.");
                    }
                    break;
                }

                case RestconfRequest::Type::CreateOrReplaceThisNode:
                case RestconfRequest::Type::CreateChildren:
                case RestconfRequest::Type::MergeData: {
                    if (restconfRequest.datastore == sysrepo::Datastore::FactoryDefault || restconfRequest.datastore == sysrepo::Datastore::Operational) {
                        throw ErrorResponse(405, "application", "operation-not-supported", "Read-only datastore.");
                    }

                    sess.switchDatastore(restconfRequest.datastore.value_or(sysrepo::Datastore::Running));
                    if (!dataFormat.request) {
                        throw ErrorResponse(400, "protocol", "invalid-value", "Content-type header missing.");
                    }

                    auto requestCtx = std::make_shared<RequestContext>(req, res, dataFormat, sess, restconfRequest);

                    req.on_data([requestCtx, restconfRequest /* intentional copy */, timeout, peer=http::peer_from_request(req)](const uint8_t* data, std::size_t length) {
                        if (length > 0) { // there are still some data to be read
                            requestCtx->payload.append(reinterpret_cast<const char*>(data), length);
                            return;
                        }

                        spdlog::trace("{}: HTTP payload: {}", peer, requestCtx->payload);

                        if (restconfRequest.type == RestconfRequest::Type::CreateChildren) {
                            WITH_RESTCONF_EXCEPTIONS(processPost, rejectWithError)(requestCtx, timeout);
                        } else if (restconfRequest.type == RestconfRequest::Type::MergeData && isYangPatch(requestCtx->req)) {
                            WITH_RESTCONF_EXCEPTIONS(processYangPatch, rejectWithError)(requestCtx, timeout);
                        } else {
                            WITH_RESTCONF_EXCEPTIONS(processPutOrPlainPatch, rejectWithError)(requestCtx, timeout);
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

                        validateInputMetaAttributes(sess.getContext(), *edit);

                        // If the node could be created, it will not be opaque. However, setting meta attributes
                        // to opaque and standard nodes is a different process.
                        if (deletedNode->isOpaque()) {
                            deletedNode->newAttrOpaqueJSON("ietf-netconf", "operation", "delete");
                        } else {
                            auto netconf = sess.getContext().getModuleLatest("ietf-netconf");
                            deletedNode->newMeta(*netconf, "operation", "delete");
                        }

                        sess.editBatch(*edit, sysrepo::DefaultOperation::Merge);
                        sess.applyChanges(timeout);
                    } catch (const sysrepo::ErrorWithCode& e) {
                        if (e.code() == sysrepo::ErrorCode::Unauthorized) {
                            throw ErrorResponse(403, "application", "access-denied", "Access denied.", restconfRequest.path);
                        } else if (e.code() == sysrepo::ErrorCode::NotFound) {
                            /* The RFC is not clear at all on the error-tag.
                             * See https://mailarchive.ietf.org/arch/msg/netconf/XcF9r3ek3LvZ4DjF-7_B8kxuiwA/
                             * Also, if we replace 403 with 404 in order not to reveal if the node does not exist or
                             * if the user is not authorized then we should return the error tag invalid-value.
                             * This clashes with the data-missing tag below and we reveal it anyway :(
                             */
                            throw ErrorResponse(404, "application", "data-missing", "Data is missing.", restconfRequest.path);
                        }

                        throw;
                    }

                    res.write_head(204, {CORS});
                    res.end();
                    break;

                case RestconfRequest::Type::Execute:
                case RestconfRequest::Type::ExecuteInternal: {
                    auto requestCtx = std::make_shared<RequestContext>(req, res, dataFormat, sess, restconfRequest);

                    req.on_data([requestCtx, timeout, peer=http::peer_from_request(req)](const uint8_t* data, std::size_t length) {
                        if (length > 0) {
                            requestCtx->payload.append(reinterpret_cast<const char*>(data), length);
                        } else {
                            spdlog::trace("{}: HTTP payload: {}", peer, requestCtx->payload);
                            WITH_RESTCONF_EXCEPTIONS(processActionOrRPC, rejectWithError)(requestCtx, timeout);
                        }
                    });
                    break;
                }

                case RestconfRequest::Type::OptionsQuery: {
                    nghttp2::asio_http2::header_map headers{CORS};

                    /* Just try to call this function with all possible HTTP methods and return those which do not fail */
                    if (auto optionsHeaders = allowedHttpMethodsForUri(sess.getContext(), req.uri().path); !optionsHeaders.empty()) {
                        headers.merge(httpOptionsHeaders(optionsHeaders));
                        res.write_head(200, headers);
                    } else {
                        res.write_head(404, headers);
                    }
                    res.end();
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
                rejectWithError(sess.getContext(), dataFormat.response, req, res, 500, "application", "operation-failed", "Internal server error due to sysrepo exception.", std::nullopt);
            }
        });

    boost::system::error_code ec;
    if (server->listen_and_serve(ec, address, port, true)) {
        throw std::runtime_error{"Server error: " + ec.message()};
    }
    spdlog::debug("Listening at {} {}", address, port);
}
}
