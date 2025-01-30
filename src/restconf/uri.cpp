/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <boost/algorithm/string/join.hpp>
#include <boost/fusion/adapted/struct/adapt_struct.hpp>
#include <boost/fusion/include/std_pair.hpp>
#include <boost/spirit/home/x3/support/ast/variant.hpp>
#include <boost/uuid/string_generator.hpp>
#include <experimental/iterator>
#include <libyang-cpp/Enum.hpp>
#include <map>
#include <string>
#include "restconf/Exceptions.h"
#include "restconf/uri.h"
#include "restconf/uri_impl.h"
#include "restconf/utils/yang.h"

using namespace std::string_literals;

namespace rousette::restconf {
namespace impl {

namespace {
namespace x3 = boost::spirit::x3;

// clang-format off

auto set_zero = [](auto& ctx) { _val(ctx) = 0u; };
auto add = [](auto& ctx) {
    char c = std::tolower(_attr(ctx));
    _val(ctx) = _val(ctx) * 16 + (c >= 'a' ? c - 'a' + 10 : c - '0');
};
const auto percentEncodedChar = x3::rule<class percentEncodedChar, unsigned>{"percentEncodedChar"} = x3::lit('%')[set_zero] >> x3::xdigit[add] >> x3::xdigit[add];

/* reserved characters according to RFC 3986, sec. 2.2 with '%' added. The '%' character is not specified as reserved but it effectively is because
 * "Percent sign serves as the indicator for percent-encoded octets, it must be percent-encoded (...)" [RFC 3986, sec. 2.4]. */
const auto reservedChars = x3::lit(':') | '/' | '?' | '#' | '[' | ']' | '@' | '!' | '$' | '&' | '\'' | '(' | ')' | '*' | '+' | ',' | ',' | ';' | '=' | '%';
const auto percentEncodedString = x3::rule<class percentEncodedString, std::string>{"percentEncodedString"} = *(percentEncodedChar | (x3::char_ - reservedChars));

const auto keyList = x3::rule<class keyList, std::vector<std::string>>{"keyList"} = percentEncodedString % ',';
const auto identifier = x3::rule<class identifier, std::string>{"identifier"} = (x3::alpha | x3::char_('_')) >> *(x3::alnum | x3::char_('_') | x3::char_('-') | x3::char_('.'));
const auto apiIdentifier = x3::rule<class apiIdentifier, ApiIdentifier>{"apiIdentifier"} = -(identifier >> ':') >> identifier;
const auto listInstance = x3::rule<class keyList, PathSegment>{"listInstance"} = apiIdentifier >> -('=' >> keyList);
const auto fullyQualifiedApiIdentifier = x3::rule<class identifier, ApiIdentifier>{"apiIdentifier"} = identifier >> ':' >> identifier;
const auto fullyQualifiedListInstance = x3::rule<class keyList, PathSegment>{"listInstance"} = fullyQualifiedApiIdentifier >> -('=' >> keyList);

const auto uriPath = x3::rule<class uriPath, std::vector<PathSegment>>{"uriPath"} = -x3::lit("/") >> -(fullyQualifiedListInstance >> -("/" >> listInstance % "/")); // RFC 8040, sec 3.5.3
const auto nmdaDatastore = x3::rule<class nmdaDatastore, URIPrefix>{"nmdaDatastore"} = x3::attr(URIPrefix::Type::NMDADatastore) >> "/" >> fullyQualifiedApiIdentifier;
const auto resources = x3::rule<class resources, URIPath>{"resources"} =
    (x3::lit("/data") >> x3::attr(URIPrefix{URIPrefix::Type::BasicRestconfData}) >> uriPath) |
    (x3::lit("/ds") >> nmdaDatastore >> uriPath) |
    (x3::lit("/operations") >> x3::attr(URIPrefix{URIPrefix::Type::BasicRestconfOperations}) >> uriPath) |
    // restconf and /restconf/yang-library-version do not expect any uriPath but we need to construct correct URIPath instance. Use the x3::attr call to create empty vector<PathSegment>
    (x3::lit("/yang-library-version") >> x3::attr(URIPrefix{URIPrefix::Type::YangLibraryVersion}) >> x3::attr(std::vector<PathSegment>{}) >> -x3::lit("/")) |
    ((x3::lit("/") | x3::eps) /* /restconf/ and /restconf */ >> x3::attr(URIPrefix{URIPrefix::Type::RestconfRoot}) >> x3::attr(std::vector<PathSegment>{}));
const auto uriGrammar = x3::rule<class grammar, URIPath>{"grammar"} = x3::lit("/restconf") >> resources;

const auto string_to_uuid = [](auto& ctx) {
    try {
        _val(ctx) = boost::uuids::string_generator()(_attr(ctx));
        _pass(ctx) = true;
    } catch (const std::runtime_error&) {
        _pass(ctx) = false;
    }
};

const auto uuid_impl = x3::rule<class uuid_impl, std::string>{"uuid_impl"} =
    x3::repeat(8)[x3::xdigit] >> x3::char_('-') >>
    x3::repeat(4)[x3::xdigit] >> x3::char_('-') >>
    x3::repeat(4)[x3::xdigit] >> x3::char_('-') >>
    x3::repeat(4)[x3::xdigit] >> x3::char_('-') >>
    x3::repeat(12)[x3::xdigit];
const auto uuid = x3::rule<class uuid, boost::uuids::uuid>{"uuid"} = uuid_impl[string_to_uuid];
const auto subscribedStream = x3::rule<class subscribedStream, RestconfStreamRequest::SubscribedStream>{"subscribedStream"} = x3::lit("/subscribed") >> "/" >> uuid;

const auto netconfStream = x3::rule<class netconfStream, RestconfStreamRequest::NetconfStream>{"netconfStream"} =
    x3::lit("/NETCONF") >>
    ((x3::lit("/XML") >> x3::attr(libyang::DataFormat::XML)) |
     (x3::lit("/JSON") >> x3::attr(libyang::DataFormat::JSON)));

const auto streamUriGrammar = x3::rule<class grammar, boost::variant<RestconfStreamRequest::NetconfStream, RestconfStreamRequest::SubscribedStream>>{"streamsGrammar"} =
    x3::lit("/streams") >> (netconfStream | subscribedStream);

// clang-format on
}

namespace {
namespace x3 = boost::spirit::x3;

// clang-format off

const auto moduleName = x3::rule<class apiIdentifier, std::string>{"moduleName"} = (x3::alpha | x3::char_('_')) >> *(x3::alnum | x3::char_('_') | x3::char_('-') | x3::char_('.'));
const auto revision = x3::rule<class revision, std::string>{"revision"} = x3::repeat(4, x3::inf)[x3::digit] >> x3::char_("-") >> x3::repeat(2)[x3::digit] >> x3::char_("-") >> x3::repeat(2)[x3::digit];
const auto yangSchemaUriGrammar = x3::rule<class grammar, impl::YangModule>{"yangSchemaUriGrammar"} = x3::lit("/") >> x3::lit("yang") >> "/" >> moduleName >> -(x3::lit("@") >> revision >> -x3::lit(".yang"));

// clang-format on
}

namespace {
namespace x3 = boost::spirit::x3;


// clang-format off

auto validDepthValues = [](auto& ctx) {
    _val(ctx) = _attr(ctx); // it seems that this must be present, otherwise the _val(ctx) is default-constructed?
    _pass(ctx) = _attr(ctx) > 0 && _attr(ctx) < 65536;
};

struct withDefaultsTable : x3::symbols<queryParams::QueryParamValue> {
    withDefaultsTable()
    {
        add
            ("trim", queryParams::withDefaults::Trim{})
            ("explicit", queryParams::withDefaults::Explicit{})
            ("report-all", queryParams::withDefaults::ReportAll{})
            ("report-all-tagged", queryParams::withDefaults::ReportAllTagged{});
    }
} const withDefaultsParam;

struct contentTable : x3::symbols<queryParams::QueryParamValue> {
    contentTable()
    {
        add
            ("all", queryParams::content::AllNodes{})
            ("nonconfig", queryParams::content::OnlyNonConfigNodes{})
            ("config", queryParams::content::OnlyConfigNodes{});
    }
} const contentParam;

struct insertTable: x3::symbols<queryParams::QueryParamValue> {
    insertTable()
    {
    add
        ("first", queryParams::insert::First{})
        ("last", queryParams::insert::Last{})
        ("after", queryParams::insert::After{})
        ("before", queryParams::insert::Before{});
    }
} const insertParam;

/* This grammar is implemented a little bit differently than the RFC states. The ABNF from RFC is:
 *
 *     fields-expr = path "(" fields-expr ")" / path ";" fields-expr / path
 *     path = api-identifier [ "/" path ]
 *
 * Firstly, the grammar from the RFC doesn't allow for expression like `a(b);c` but allows for `c;a(b)`.
 * I think both should be valid (as user I would expect that the order of such expression does not matter).
 * Hence our grammar allows for more strings than the grammar from RFC.
 * This issue was already raised on IETF mailing list: https://mailarchive.ietf.org/arch/msg/netconf/TYBpTE_ELzzMOe6amrw6fQF07nE/
 * but neither a formal errata was issued nor there was a resolution on the mailing list.
 */
const auto fieldsExpr = x3::rule<class fieldsExpr, queryParams::fields::Expr>{"fieldsExpr"};
const auto fieldsSemi = x3::rule<class fieldsSemiExpr, queryParams::fields::SemiExpr>{"fieldsSemi"};
const auto fieldsSlash = x3::rule<class fieldsSlashExpr, queryParams::fields::SlashExpr>{"fieldsSlash"};
const auto fieldsParen = x3::rule<class fieldsParen, queryParams::fields::ParenExpr>{"fieldsParen"};

const auto fieldsSemi_def = fieldsParen >> -(x3::lit(";") >> fieldsSemi);
const auto fieldsParen_def = fieldsSlash >> -(x3::lit("(") >> fieldsExpr >> x3::lit(")"));
const auto fieldsSlash_def = apiIdentifier >> -(x3::lit("/") >> fieldsSlash);
const auto fieldsExpr_def = fieldsSemi;
BOOST_SPIRIT_DEFINE(fieldsParen, fieldsExpr, fieldsSlash, fieldsSemi);

// early sanity check, this timestamp will be parsed by libyang::fromYangTimeFormat anyways
const auto dateAndTime = x3::rule<class dateAndTime, std::string>{"dateAndTime"} =
    x3::repeat(4)[x3::digit] >> x3::char_('-') >> x3::repeat(2)[x3::digit] >> x3::char_('-') >> x3::repeat(2)[x3::digit] >> x3::char_('T') >>
    x3::repeat(2)[x3::digit] >> x3::char_(':') >> x3::repeat(2)[x3::digit] >> x3::char_(':') >> x3::repeat(2)[x3::digit] >> -(x3::char_('.') >> +x3::digit) >>
    (x3::char_('Z') | (-(x3::char_('+')|x3::char_('-')) >> x3::repeat(2)[x3::digit] >> x3::char_(':') >> x3::repeat(2)[x3::digit]));
const auto filter = x3::rule<class filter, std::string>{"filter"} = +(percentEncodedChar | (x3::char_ - '&'));
const auto depthParam = x3::rule<class depthParam, queryParams::QueryParamValue>{"depthParam"} = x3::uint_[validDepthValues] | (x3::string("unbounded") >> x3::attr(queryParams::UnboundedDepth{}));
const auto queryParamPair = x3::rule<class queryParamPair, std::pair<std::string, queryParams::QueryParamValue>>{"queryParamPair"} =
        (x3::string("depth") >> "=" >> depthParam) |
        (x3::string("with-defaults") >> "=" >> withDefaultsParam) |
        (x3::string("content") >> "=" >> contentParam) |
        (x3::string("insert") >> "=" >> insertParam) |
        (x3::string("point") >> "=" >> uriPath) |
        (x3::string("filter") >> "=" >> filter) |
        (x3::string("start-time") >> "=" >> dateAndTime) |
        (x3::string("stop-time") >> "=" >> dateAndTime) |
        (x3::string("fields") >> "=" >> fieldsExpr);

const auto queryParamGrammar = x3::rule<class grammar, queryParams::QueryParams>{"queryParamGrammar"} = queryParamPair % "&" | x3::eps;

// clang-format on
}

std::optional<URIPath> parseUriPath(const std::string& uriPath)
{
    URIPath out;
    auto iter = std::begin(uriPath);
    auto end = std::end(uriPath);

    if (!x3::parse(iter, end, uriGrammar >> x3::eoi, out)) {
        return std::nullopt;
    }

    return out;
}

std::optional<impl::YangModule> parseModuleWithRevision(const std::string& uriPath)
{
    impl::YangModule parsed;
    auto iter = std::begin(uriPath);
    auto end = std::end(uriPath);

    if (!x3::parse(iter, end, yangSchemaUriGrammar >> x3::eoi, parsed)) {
        return std::nullopt;
    }

    return parsed;
}

std::optional<queryParams::QueryParams> parseQueryParams(const std::string& queryString)
{
    std::optional<queryParams::QueryParams> ret;

    if (!x3::parse(std::begin(queryString), std::end(queryString), queryParamGrammar >> x3::eoi, ret)) {
        return std::nullopt;
    }

    return ret;
}

std::optional<std::variant<RestconfStreamRequest::NetconfStream, RestconfStreamRequest::SubscribedStream>> parseStreamUri(const std::string& uriPath)
{
    /*
     * FIXME once we can ensure gcc>=13.3.
     * Well... this is a bit ugly, but I didn't find a better way to do this.
     * In our CI, on gcc 13.2 I am getting a weird error coming from the optimizer that something in this variant is not initialized.
     * I am not sure what is the problem, and I am not able to reproduce it locally on gcc 13.3,  gcc 14.2, and clang 17, 18 and 19.
     * This leads me to believe that it is a bug in the compiler.
     *
     * So, what should be reverted:
     *  - This variant is supposed to be actually an std::variant and the object should be returned without the ugly conversion
     *    from boost::variant to std::variant.
     *  - SubscribedStream::DataFormat should not be initialized to libyang::DataFormat::XML but default initialized.
     */
    boost::variant<RestconfStreamRequest::NetconfStream, RestconfStreamRequest::SubscribedStream> ret;

    if (!x3::parse(std::begin(uriPath), std::end(uriPath), streamUriGrammar >> x3::eoi, ret)) {
        return std::nullopt;
    }

    // FIXME: See comment above
    std::variant<RestconfStreamRequest::NetconfStream, RestconfStreamRequest::SubscribedStream> ret2;
    if (auto* p = boost::get<RestconfStreamRequest::NetconfStream>(&ret)) {
        ret2 = *p;
    } else if (auto* p = boost::get<RestconfStreamRequest::SubscribedStream>(&ret)) {
        ret2 = *p;
    }
    return ret2;
}

URIPrefix::URIPrefix()
    : resourceType(URIPrefix::Type::BasicRestconfData)
{
}

URIPrefix::URIPrefix(URIPrefix::Type resourceType, const boost::optional<ApiIdentifier>& datastore)
    : resourceType(resourceType)
    , datastore(datastore)
{
}

URIPath::URIPath() = default;

URIPath::URIPath(const URIPrefix& prefix, const std::vector<PathSegment>& segments)
    : prefix(prefix)
    , segments(segments)
{
}

URIPath::URIPath(const std::vector<PathSegment>& segments)
    : segments(segments)
{
}
}

ApiIdentifier::ApiIdentifier() = default;

ApiIdentifier::ApiIdentifier(const std::string& prefix, const std::string& identifier)
    : prefix(prefix)
    , identifier(identifier)
{
}

ApiIdentifier::ApiIdentifier(const std::string& identifier)
    : prefix(boost::none)
    , identifier(identifier)
{
}

/** @brief Returns ApiIdentifier as a string, optionally prefixed with the module name and colon */
std::string ApiIdentifier::name() const
{
    if (!prefix) {
        return identifier;
    }
    return *prefix + ":" + identifier;
}

PathSegment::PathSegment() = default;

PathSegment::PathSegment(const ApiIdentifier& apiIdent, const std::vector<std::string>& keys)
    : apiIdent(apiIdent)
    , keys(keys)
{
}

namespace {
std::optional<sysrepo::Datastore> datastoreFromApiIdentifier(const boost::optional<ApiIdentifier>& datastore)
{
    if (!datastore) {
        return std::nullopt;
    }

    if (*datastore->prefix == "ietf-datastores") {
        if (datastore->identifier == "running") {
            return sysrepo::Datastore::Running;
        } else if (datastore->identifier == "operational") {
            return sysrepo::Datastore::Operational;
        } else if (datastore->identifier == "candidate") {
            return sysrepo::Datastore::Candidate;
        } else if (datastore->identifier == "startup") {
            return sysrepo::Datastore::Startup;
        } else if (datastore->identifier == "factory-default") {
            return sysrepo::Datastore::FactoryDefault;
        }
    }

    throw ErrorResponse(400, "application", "operation-failed", "Unsupported datastore " + *datastore->prefix + ":" + datastore->identifier);
}

std::optional<std::variant<libyang::Module, libyang::SubmoduleParsed>> getModuleOrSubmodule(const libyang::Context& ctx, const std::string& name, const std::optional<std::string>& revision)
{
    if (auto mod = ctx.getModule(name, revision)) {
        return *mod;
    }
    if (auto mod = ctx.getSubmodule(name, revision)) {
        return *mod;
    }
    return std::nullopt;
}
}

RestconfRequest::RestconfRequest(Type type, const boost::optional<ApiIdentifier>& datastore, const std::string& path, const queryParams::QueryParams& queryParams)
    : type(type)
    , datastore(datastoreFromApiIdentifier(datastore))
    , path(path)
    , queryParams(queryParams)
{
}

namespace {
std::optional<libyang::SchemaNode> findChildSchemaNode(const libyang::SchemaNode& node, const ApiIdentifier& childIdentifier)
{
    for (const auto& child : node.childInstantiables()) {
        if (child.name() == childIdentifier.identifier) {
            // If the prefix is not specified then we must ensure that child's module is the same as the node's module so that we don't accidentally return a child that was inserted here via an augment
            if (
                (!childIdentifier.prefix && node.module().name() == child.module().name()) || (childIdentifier.prefix && child.module().name() == *childIdentifier.prefix)) {
                return child;
            }
        }
    }

    return std::nullopt;
}

/** @brief Construct a fully qualified name of the node if needed
 *
 * @return string in the form <module>:<nodeName> if the parent module does not exist or is different from module of @p node else return only name of @p node.
 */
std::string maybeQualified(const libyang::SchemaNode& currentNode)
{
    if (!currentNode.parent() || currentNode.parent()->module().name() != currentNode.module().name()) {
        return currentNode.module().name() + ':' + currentNode.name();
    } else {
        return currentNode.name();
    }
}

/** @brief checks if provided schema node is valid for this HTTP method */
void checkValidDataResource(const std::optional<libyang::SchemaNode>& node, const impl::URIPrefix& prefix)
{
    if (prefix.resourceType != impl::URIPrefix::Type::BasicRestconfData && prefix.resourceType != impl::URIPrefix::Type::NMDADatastore) {
        throw ErrorResponse(400, "application", "operation-failed", "GET method must be used with a data resource or a complete datastore resource");
    }

    switch (node->nodeType()) {
    case libyang::NodeType::Container:
    case libyang::NodeType::Leaf:
    case libyang::NodeType::AnyXML:
    case libyang::NodeType::AnyData:
    /* querying the actual (leaf-)list node is not a valid data resource, only (leaf-)list entries are.
     * Yet we consider this as a valid resource here. If this function is called we already checked if the keys are specified in the caller.
     * If they were correctly specified, then we are querying instance. If not, then the code already throwed.
     */
    case libyang::NodeType::Leaflist:
    case libyang::NodeType::List:
        return;
    case libyang::NodeType::RPC:
    case libyang::NodeType::Action:
        throw ErrorResponse(405, "protocol", "operation-not-supported", "'"s + node->path() + "' is an RPC/Action node");
    default:
        throw ErrorResponse(400, "protocol", "operation-failed", "'"s + node->path() + "' is not a data resource");
    }
}

/** @brief Validates whether SchemaNode is valid for this HTTP method and prefix
 *
 * @throw ErrorResponse If node is invalid for this httpMethod and URI prefix */
void validateMethodForNode(const std::string& httpMethod, const impl::URIPrefix& prefix, const std::optional<libyang::SchemaNode>& node)
{
    if (httpMethod == "OPTIONS") {
        // no check is needed; path validation happens via the request handler by trying all other methods
    } else if (!node) {
        // no data path provided
        switch (prefix.resourceType) {
        case impl::URIPrefix::Type::BasicRestconfData:
        case impl::URIPrefix::Type::NMDADatastore:
            if (httpMethod == "DELETE") {
                throw ErrorResponse(400, "application", "operation-failed", "'/' is not a data resource");
            }
            break;
        case impl::URIPrefix::Type::RestconfRoot:
        case impl::URIPrefix::Type::BasicRestconfOperations:
        case impl::URIPrefix::Type::YangLibraryVersion:
            if (httpMethod != "GET" && httpMethod != "HEAD") {
                throw ErrorResponse(405, "application", "operation-not-supported", "Method not allowed.");
            }
            break;
        }
    } else if (httpMethod == "POST") {
        switch (node->nodeType()) {
        case libyang::NodeType::RPC:
            if (prefix.resourceType != impl::URIPrefix::Type::BasicRestconfOperations) {
                throw ErrorResponse(400, "protocol", "operation-failed", "RPC '"s + node->path() + "' must be requested using operation prefix");
            }
            break;
        case libyang::NodeType::Action:
            if (!(prefix.resourceType == impl::URIPrefix::Type::BasicRestconfData) && !(prefix.resourceType == impl::URIPrefix::Type::NMDADatastore && prefix.datastore == ApiIdentifier{"ietf-datastores", "operational"})) {
                throw ErrorResponse(400, "protocol", "operation-failed", "Action '"s + node->path() + "' must be requested using data prefix or via operational NMDA");
            }
            break;
        default:
            checkValidDataResource(node, prefix);
        }
    } else {
        checkValidDataResource(node, prefix);
    }
}

void validateQueryParameters(const std::multimap<std::string, queryParams::QueryParamValue>& params_, const std::string& httpMethod)
{
    std::map<std::string, queryParams::QueryParamValue> params;
    for (const auto& [k, v] : params_) {
        auto [it, inserted] = params.emplace(k, v);
        if (!inserted) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '" + k + "' already specified");
        }
    }

    for (const auto& param : {"depth", "with-defaults", "content", "fields"}) {
        if (auto it = params.find(param); it != params.end() && httpMethod != "GET" && httpMethod != "HEAD") {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '"s + param + "' can be used only with GET and HEAD methods");
        }
    }

    for (const auto& param : {"insert", "point"}) {
        if (auto it = params.find(param); it != params.end() && httpMethod != "POST" && httpMethod != "PUT") {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '"s + param + "' can be used only with POST and PUT methods");
        }
    }

    for (const auto& param : {"filter", "start-time", "stop-time"}) {
        if (auto it = params.find(param); it != params.end()) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '"s + param + "' can be used only with streams");
        }
    }

    {
        auto itInsert = params.find("insert");
        auto itPoint = params.find("point");
        auto hasInsertParamBeforeOrAfter = itInsert != params.end() && (std::holds_alternative<queryParams::insert::After>(itInsert->second) || std::holds_alternative<queryParams::insert::Before>(itInsert->second));
        auto hasPointParam = itPoint != params.end();

        if (hasPointParam != hasInsertParamBeforeOrAfter) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter 'point' must always come with parameter 'insert' set to 'before' or 'after'");
        }
    }
}

void validateQueryParametersForStream(const std::multimap<std::string, queryParams::QueryParamValue>& params_)
{
    std::map<std::string, queryParams::QueryParamValue> params;
    for (const auto& [k, v] : params_) {
        auto [it, inserted] = params.emplace(k, v);
        if (!inserted) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '" + k + "' already specified");
        }

        if (k != "filter" && k != "start-time" && k != "stop-time") {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '" + k + "' can't be used with streams");
        }
    }
}

/** @brief Wrapper for a libyang path and a corresponding SchemaNode. SchemaNode is nullopt for datastore resource */
struct SchemaNodeAndPath {
    std::string dataPath;
    std::optional<libyang::SchemaNode> schemaNode;
};

/** @brief Translates PathSegment sequence to a path understood by libyang
 * @return libyang path to a data node
 * @throws ErrorResponse On invalid URI which can mean that, e.g, a node is not found, wrong number of list keys provided, list key could not be properly escaped.
 */
SchemaNodeAndPath asLibyangPath(const libyang::Context& ctx, const std::vector<PathSegment>::const_iterator& begin, const std::vector<PathSegment>::const_iterator& end)
{
    std::optional<libyang::SchemaNode> currentNode;
    std::string res;

    for (auto it = begin; it != end; ++it) {
        if (auto prevNode = currentNode) {
            if (!(currentNode = findChildSchemaNode(*currentNode, it->apiIdent))) {
                throw ErrorResponse(400, "application", "operation-failed", "Node '" + it->apiIdent.name() + "' is not a child of '" + prevNode->path() + "'");
            }
        } else { // we are starting at root (no parent)
            try {
                currentNode = ctx.findPath("/" + it->apiIdent.name());
            } catch (const libyang::Error& e) {
                throw ErrorResponse(400, "application", "operation-failed", e.what());
            }
        }

        res += "/" + maybeQualified(*currentNode);

        if (currentNode->nodeType() == libyang::NodeType::List) {
            const auto& listKeys = currentNode->asList().keys();

            if (listKeys.size() == 0) {
                throw ErrorResponse(400, "application", "operation-failed", "List '" + currentNode->path() + "' has no keys. It can not be accessed directly");
            } else if (it->keys.size() != listKeys.size()) {
                throw ErrorResponse(400, "application", "operation-failed", "List '" + currentNode->path() + "' requires " + std::to_string(listKeys.size()) + " keys");
            }

            try {
                res += listKeyPredicate(listKeys, it->keys);
            } catch (const std::invalid_argument& e) {
                throw ErrorResponse(400, "application", "operation-failed", e.what());
            }
        } else if (currentNode->nodeType() == libyang::NodeType::Leaflist) {
            if (it->keys.size() != 1) {
                throw ErrorResponse(400, "application", "operation-failed", "Leaf-list '" + currentNode->path() + "' requires exactly one key");
            }

            try {
                res += "[.=" + escapeListKey(it->keys.front()) + ']';
            } catch (const std::invalid_argument& e) {
                throw ErrorResponse(400, "application", "operation-failed", e.what());
            }
        } else if (it->keys.size() > 0) {
            throw ErrorResponse(400, "application", "operation-failed", "No keys allowed for node '" + currentNode->path() + "'");
        }

        if (std::next(it) != end && (currentNode->nodeType() == libyang::NodeType::RPC || currentNode->nodeType() == libyang::NodeType::Action)) {
            throw ErrorResponse(400, "application", "operation-failed", "'"s + currentNode->path() + "' is an RPC/Action node, any child of it can't be requested", std::nullopt);
        }
    }
    return {res, currentNode};
}
}

/** @brief Returns a schema node corresponding to the parsed RESTCONF URI */
std::optional<libyang::SchemaNode> asLibyangSchemaNode(const libyang::Context& ctx, const std::vector<PathSegment>& pathSegments)
{
    return asLibyangPath(ctx, pathSegments.begin(), pathSegments.end()).schemaNode;
}

std::vector<PathSegment> asPathSegments(const std::string& uriPath)
{
    auto uri = impl::parseUriPath(uriPath);
    if (!uri) {
        throw ErrorResponse(400, "application", "operation-failed", "Syntax error");
    }

    return uri->segments;
}

/** @brief Checks if the RPC with this schema path should be handled internally by the RESTCONF server */
constexpr bool isInternalRPCPath(const std::string& schemaPath)
{
    std::vector<std::string> arr{
        "/ietf-subscribed-notifications:establish-subscription",
        "/ietf-subscribed-notifications:modify-subscription",
        "/ietf-subscribed-notifications:delete-subscription",
        "/ietf-subscribed-notifications:kill-subscription",
    };

    return std::find(arr.begin(), arr.end(), schemaPath) != arr.end();
}

/** @brief Parse requested URL as a RESTCONF requested
 *
 * The URI path (i.e., a resource identifier) will be parsed into an action that is supposed to be performed,
 * the target datastore, and a libyang path over which the operation will be performed.
 *
 * @throws ErrorResponse when the URI cannot be parsed or the URI is invalid for this HTTP method
 */
RestconfRequest asRestconfRequest(const libyang::Context& ctx, const std::string& httpMethod, const std::string& uriPath, const std::string& uriQueryString)
{
    if (httpMethod != "GET" && httpMethod != "PUT" && httpMethod != "POST" && httpMethod != "DELETE" && httpMethod != "HEAD" && httpMethod != "OPTIONS" && httpMethod != "PATCH") {
        throw ErrorResponse(405, "application", "operation-not-supported", "Method not allowed.");
    }

    auto uri = impl::parseUriPath(uriPath);
    if (!uri) {
        throw ErrorResponse(400, "application", "operation-failed", "Syntax error");
    }

    auto queryParameters = impl::parseQueryParams(uriQueryString);
    if (!queryParameters) {
        throw ErrorResponse(400, "protocol", "invalid-value", "Query parameters syntax error");
    }
    validateQueryParameters(*queryParameters, httpMethod);

    auto [lyPath, schemaNode] = asLibyangPath(ctx, uri->segments.begin(), uri->segments.end());
    validateMethodForNode(httpMethod, uri->prefix, schemaNode);

    auto path = uri->segments.empty() ? "/" : lyPath;
    RestconfRequest::Type type;

    if (httpMethod == "OPTIONS") {
        return {RestconfRequest::Type::OptionsQuery, boost::none, ""s, {}};
    } else if (uri->prefix.resourceType == impl::URIPrefix::Type::RestconfRoot) {
        return {RestconfRequest::Type::RestconfRoot, boost::none, ""s, *queryParameters};
    } else if (uri->prefix.resourceType == impl::URIPrefix::Type::YangLibraryVersion) {
        return {RestconfRequest::Type::YangLibraryVersion, boost::none, ""s, *queryParameters};
    } else if (uri->prefix.resourceType == impl::URIPrefix::Type::BasicRestconfOperations && !schemaNode) {
        return {RestconfRequest::Type::ListRPC, boost::none, ""s, *queryParameters};
    } else if ((httpMethod == "GET" || httpMethod == "HEAD")) {
        type = RestconfRequest::Type::GetData;
        path = uri->segments.empty() ? "/*" : path;
    } else if (httpMethod == "PUT") {
        type = RestconfRequest::Type::CreateOrReplaceThisNode;
    } else if (httpMethod == "DELETE" && schemaNode) {
        type = RestconfRequest::Type::DeleteNode;
    } else if (httpMethod == "POST" && schemaNode && (schemaNode->nodeType() == libyang::NodeType::Action || schemaNode->nodeType() == libyang::NodeType::RPC)) {
        type = isInternalRPCPath(schemaNode->path()) ? RestconfRequest::Type::ExecuteInternal : RestconfRequest::Type::Execute;
    } else if (httpMethod == "POST" && (uri->prefix.resourceType == impl::URIPrefix::Type::BasicRestconfData || uri->prefix.resourceType == impl::URIPrefix::Type::NMDADatastore)) {
        type = RestconfRequest::Type::CreateChildren;
    } else if (httpMethod == "PATCH") {
        type = RestconfRequest::Type::MergeData;
    } else {
        throw std::logic_error("Unhandled request "s + httpMethod + " " + uriPath);
    }

    return {type, uri->prefix.datastore, path, *queryParameters};
}

/** @brief Transforms URI path into a libyang path to the parent node (or empty if this path was a root node) and PathSegment describing the last path segment.
 * This is useful for the PUT method where we have to start editing the tree in the parent node.
 *
 * @throws ErrorResponse On invalid URI
 * @return Pair of a libyang path to the parent as a string and a PathSegment instance describing the last path segment node
 */
std::pair<std::string, PathSegment> asLibyangPathSplit(const libyang::Context& ctx, const std::string& uriPath)
{
    auto uri = impl::parseUriPath(uriPath);
    if (!uri) {
        throw ErrorResponse(400, "application", "operation-failed", "Syntax error");
    }
    if (uri->segments.empty()) {
        throw ErrorResponse(400, "application", "operation-failed", "Cannot split the datastore resource URI");
    }


    auto lastSegment = uri->segments.back();
    auto [parentLyPath, schemaNodeParent] = asLibyangPath(ctx, uri->segments.begin(), uri->segments.end() - 1);

    // we know that the path is valid so we can get last segment module from the returned SchemaNode
    if (!lastSegment.apiIdent.prefix) {
        auto [fullLyPath, schemaNode] = asLibyangPath(ctx, uri->segments.begin(), uri->segments.end());
        lastSegment.apiIdent.prefix = std::string(schemaNode->module().name());
    }

    return {parentLyPath, lastSegment};
}

std::optional<std::variant<libyang::Module, libyang::SubmoduleParsed>> asYangModule(const libyang::Context& ctx, const std::string& uriPath)
{
    if (auto parsedModule = impl::parseModuleWithRevision(uriPath)) {
        // Converting between boost::optional and std::optional is not trivial
        if (parsedModule->revision) {
            return getModuleOrSubmodule(ctx, parsedModule->name, *parsedModule->revision);
        } else {
            return getModuleOrSubmodule(ctx, parsedModule->name, std::nullopt);
        }
    }
    return std::nullopt;
}

RestconfStreamRequest::NetconfStream::NetconfStream() = default;
RestconfStreamRequest::NetconfStream::NetconfStream(const libyang::DataFormat& encoding)
    : encoding(encoding)
{
}

RestconfStreamRequest::SubscribedStream::SubscribedStream() = default;
RestconfStreamRequest::SubscribedStream::SubscribedStream(const boost::uuids::uuid& uuid)
    : uuid(uuid)
{
}

RestconfStreamRequest asRestconfStreamRequest(const std::string& httpMethod, const std::string& uriPath, const std::string& uriQueryString)
{
    if (httpMethod != "GET" && httpMethod != "HEAD") {
        throw ErrorResponse(405, "application", "operation-not-supported", "Method not allowed.");
    }

    auto type = impl::parseStreamUri(uriPath);
    if (!type) {
        throw ErrorResponse(404, "application", "invalid-value", "Invalid stream");
    }

    auto queryParameters = impl::parseQueryParams(uriQueryString);
    if (!queryParameters) {
        throw ErrorResponse(400, "protocol", "invalid-value", "Query parameters syntax error");
    }

    validateQueryParametersForStream(*queryParameters);

    return {*type, *queryParameters};
}

/** @brief Returns a set of allowed HTTP methods for given URI. Usable for the 'allow' header */
std::set<std::string> allowedHttpMethodsForUri(const libyang::Context& ctx, const std::string& uriPath)
{
    std::set<std::string> allowedHttpMethods;

    for (const auto& httpMethod : {"GET", "PUT", "POST", "DELETE", "HEAD", "PATCH"}) {
        try {
            asRestconfRequest(ctx, httpMethod, uriPath, "");
            allowedHttpMethods.insert(httpMethod);
        } catch (const ErrorResponse&) {
            // httpMethod is not allowed for this uri path
        }
    }

    if (!allowedHttpMethods.empty()) {
        allowedHttpMethods.insert("OPTIONS");
    }

    return allowedHttpMethods;
}

/** @brief Traverses the AST of the fields input expression and collects all the possible paths
 *
 * @param expr The fields expressions
 * @param currentPath The current path in the AST, it serves as a stack for the DFS
 * @param output The collection of all collected paths
 * @param end If this is the terminal node, i.e., the last node in the expression. This is needed for the correct handling of the leafs under paren expression, which does not "split" the paths but rather concatenates.
 * */
void fieldsToXPath(const queryParams::fields::Expr& expr, std::vector<std::string>& currentPath, std::vector<std::string>& output, bool end = false)
{
    boost::apply_visitor([&](auto&& node) {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, queryParams::fields::ParenExpr>) {
            // the paths from left and right subtree are concatenated, i.e., the nodes we collect in the left tree
            // are joined together with the nodes from the right tree
            fieldsToXPath(node.lhs, currentPath, output, !node.rhs.has_value());
            if (node.rhs) {
                fieldsToXPath(*node.rhs, currentPath, output, end);
            }
        } else if constexpr (std::is_same_v<T, queryParams::fields::SemiExpr>) {
            // the two paths are now independent and nodes from left subtree do not affect the right subtree
            // hence we need to copy the current path
            auto pathCopy = currentPath;
            fieldsToXPath(node.lhs, currentPath, output, !node.rhs.has_value());
            if (node.rhs) {
                fieldsToXPath(*node.rhs, pathCopy, output, false);
            }
        } else if constexpr (std::is_same_v<T, queryParams::fields::SlashExpr>) {
            // the paths from left and right subtree are concatenated, i.e., the the nodes we collect in the left tree
            // are joined together with the nodes from the right tree, but if this is the terminal node, we need to
            // add it to the collection of all the gathered paths
            currentPath.push_back(node.lhs.name());

            if (node.rhs) {
                fieldsToXPath(*node.rhs, currentPath, output, end);
            } else if (end) {
                output.emplace_back(boost::algorithm::join(currentPath, "/"));
            }
        }
    },
                         expr);
}

/** @brief Translates the fields expression into a XPath expression and checks for schema validity of the resulting nodes
 *
 * The expressions are "unwrapped" into a linear structure and then a union of such paths is made.
 * E.g., the expression "a(b;c)" is translated into "a/b | a/c".
 * */
std::string fieldsToXPath(const libyang::Context& ctx, const std::string& prefix, const queryParams::fields::Expr& expr)
{
    std::vector<std::string> currentPath{prefix};
    std::vector<std::string> paths;

    fieldsToXPath(expr, currentPath, paths);

    for (auto& xpath : paths) {
        try {
            validateMethodForNode("GET", impl::URIPrefix{impl::URIPrefix::Type::BasicRestconfData}, ctx.findPath(xpath));
        } catch (const libyang::Error& e) {
            throw ErrorResponse(400, "application", "operation-failed", "Can't find schema node for '" + xpath + "'");
        }
    }

    return boost::algorithm::join(paths, " | ");
}
}
