/*
 * Copyright (C) 2016-2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <experimental/iterator>
#include <string>
#include <sysrepo-cpp/Connection.hpp>
#include "restconf/Exceptions.h"
#include "restconf/uri.h"
#include "restconf/uri_impl.h"
#include "tests/configure.cmake.h"

using namespace std::string_literals;

namespace {
std::string serializeErrorResponse(int code, const std::string& errorType, const std::string& errorTag, const std::string& errorMessage)
{
    return "("s + std::to_string(code) + ", \"" + errorType + "\", \"" + errorTag + "\", \"" + errorMessage + "\")";
}
}

namespace rousette::restconf {

REGISTER_EXCEPTION_TRANSLATOR(const ErrorResponse& e)
{
    return serializeErrorResponse(e.code, e.errorType, e.errorTag, e.errorMessage).c_str();
}

std::ostream& operator<<(std::ostream& os, const ApiIdentifier& obj)
{
    os << "ApiIdentifier{";
    os << "prefix=";
    if (obj.prefix) {
        os << "'" << *obj.prefix << "'";
    } else {
        os << "nullopt{}";
    }

    return os << ", ident='" << obj.identifier << "'}";
}

std::ostream& operator<<(std::ostream& os, const PathSegment& obj)
{

    os << "Segment{" << obj.apiIdent << " "
       << "keys=";
    os << "[";
    std::copy(obj.keys.begin(), obj.keys.end(), std::experimental::make_ostream_joiner(os, ", "));
    return os << "]}";
}

std::ostream& operator<<(std::ostream& os, const std::vector<PathSegment>& v)
{
    os << "[";
    std::copy(v.begin(), v.end(), std::experimental::make_ostream_joiner(os, ", "));
    return os << "]";
}
}

namespace rousette::restconf::impl {
std::ostream& operator<<(std::ostream& os, const URI& obj)
{
    os << "[";
    std::copy(obj.segments.begin(), obj.segments.end(), std::experimental::make_ostream_joiner(os, ", "));
    return os << "]";
}
}

namespace doctest {
template <>
struct StringMaker<std::optional<std::string>> {
    static String convert(const std::optional<std::string>& obj)
    {
        std::ostringstream oss;

        if (obj) {
            oss << "optional{" << *obj << "}";
        } else {
            oss << "nullopt{}";
        }

        return oss.str().c_str();
    }
};

template <>
struct StringMaker<std::optional<rousette::restconf::impl::URI>> {
    static String convert(const std::optional<rousette::restconf::impl::URI>& obj)
    {
        std::ostringstream oss;

        if (obj) {
            oss << "optional{" << obj->segments << "}";
        } else {
            oss << "nullopt{}";
        }

        return oss.str().c_str();
    }
};
}

TEST_CASE("URI path parser")
{
    using rousette::restconf::ApiIdentifier;
    using rousette::restconf::PathSegment;
    using rousette::restconf::RestconfRequest;
    using rousette::restconf::impl::URI;
    using rousette::restconf::impl::URIPrefix;

    SECTION("Valid paths")
    {
        for (const auto& [uriPath, expected] : {
                 std::pair<std::string, URI>{"/restconf/data/x333:y666", URI({
                                                                             {{"x333", "y666"}},
                                                                         })},
                 {"/restconf/data/foo:bar", URI(std::vector<PathSegment>{
                                                {{"foo", "bar"}},
                                            })},
                 {"/restconf/data/foo:bar/baz", URI({
                                                    {{"foo", "bar"}},
                                                    {{"baz"}},
                                                })},
                 {"/restconf/data/foo:bar/meh:baz", URI({
                                                        {{"foo", "bar"}},
                                                        {{"meh", "baz"}},
                                                    })},
                 {"/restconf/data/foo:bar/yay/meh:baz", URI({
                                                            {{"foo", "bar"}},
                                                            {{"yay"}},
                                                            {{"meh", "baz"}},
                                                        })},
                 {"/restconf/data/foo:bar/Y=val", URI({
                                                      {{"foo", "bar"}},
                                                      {{"Y"}, {"val"}},
                                                  })},
                 {"/restconf/data/foo:bar/Y=val-ue", URI({
                                                         {{"foo", "bar"}},
                                                         {{"Y"}, {"val-ue"}},
                                                     })},
                 {"/restconf/data/foo:bar/p:lst=key1", URI({
                                                           {{"foo", "bar"}},
                                                           {{"p", "lst"}, {"key1"}},
                                                       })},

                 {"/restconf/data/foo:bar/p:lst=key1/leaf", URI({
                                                                {{"foo", "bar"}},
                                                                {{"p", "lst"}, {"key1"}},
                                                                {{"leaf"}},
                                                            })},
                 {"/restconf/data/foo:bar/lst=key1,", URI({
                                                          {{"foo", "bar"}},
                                                          {{"lst"}, {"key1", ""}},
                                                      })},
                 {"/restconf/data/foo:bar/lst=key1,,,", URI({
                                                            {{"foo", "bar"}},
                                                            {{"lst"}, {"key1", "", "", ""}},
                                                        })},
                 {"/restconf/data/foo:bar/lst=key1,/leaf", URI({
                                                               {{"foo", "bar"}},
                                                               {{"lst"}, {"key1", ""}},
                                                               {{"leaf"}},
                                                           })},
                 {"/restconf/data/foo:bar/lst=key1,key2", URI({
                                                              {{"foo", "bar"}},
                                                              {{"lst"}, {"key1", "key2"}},
                                                          })},
                 {"/restconf/data/foo:bar/lst=key1,key2/leaf", URI({
                                                                   {{"foo", "bar"}},
                                                                   {{"lst"}, {"key1", "key2"}},
                                                                   {{"leaf"}},
                                                               })},
                 {"/restconf/data/foo:bar/lst=key1,key2/lst2=key1/leaf", URI({
                                                                             {{"foo", "bar"}},
                                                                             {{"lst"}, {"key1", "key2"}},
                                                                             {{"lst2"}, {"key1"}},
                                                                             {{"leaf"}},
                                                                         })},
                 {"/restconf/data/foo:bar/lst=,key2/lst2=key1/leaf", URI({
                                                                         {{"foo", "bar"}},
                                                                         {{"lst"}, {"", "key2"}},
                                                                         {{"lst2"}, {"key1"}},
                                                                         {{"leaf"}},
                                                                     })},
                 {"/restconf/data/foo:bar/lst=,/lst2=key1/leaf", URI({
                                                                     {{"foo", "bar"}},
                                                                     {{"lst"}, {"", ""}},
                                                                     {{"lst2"}, {"key1"}},
                                                                     {{"leaf"}},
                                                                 })},
                 {"/restconf/data/foo:bar/lst=", URI({
                                                     {{"foo", "bar"}},
                                                     {{"lst"}, {""}},
                                                 })},
                 {"/restconf/data/foo:bar/lst=/leaf", URI({
                                                          {{"foo", "bar"}},
                                                          {{"lst"}, {""}},
                                                          {{"leaf"}},
                                                      })},
                 {"/restconf/data/foo:bar/prefix:lst=key1/prefix:leaf", URI({
                                                                            {{"foo", "bar"}},
                                                                            {{"prefix", "lst"}, {"key1"}},
                                                                            {{"prefix", "leaf"}},
                                                                        })},
                 {"/restconf/data/foo:bar/lst=key1,,key3", URI({
                                                               {{"foo", "bar"}},
                                                               {{"lst"}, {"key1", "", "key3"}},
                                                           })},
                 {"/restconf/data/foo:bar/lst=key%2CWithCommas,,key2C", URI({
                                                                            {{"foo", "bar"}},
                                                                            {{"lst"}, {"key,WithCommas", "", "key2C"}},
                                                                        })},
                 {R"(/restconf/data/foo:bar/list1=%2C%27"%3A"%20%2F,,foo)", URI({
                                                                                {{"foo", "bar"}},
                                                                                {{"list1"}, {R"(,'":" /)", "", "foo"}},
                                                                            })},
                 {"/restconf/data/foo:bar/list1= %20,%20,foo", URI({
                                                                   {{"foo", "bar"}},
                                                                   {{"list1"}, {"  ", " ", "foo"}},
                                                               })},
                 {"/restconf/data/foo:bar/list1= %20,%20, ", URI({
                                                                 {{"foo", "bar"}},
                                                                 {{"list1"}, {"  ", " ", " "}},
                                                             })},
                 {"/restconf/data/foo:bar/list1=žluťoučkýkůň", URI({
                                                                   {{"foo", "bar"}},
                                                                   {{"list1"}, {"žluťoučkýkůň"}},
                                                               })},
                 {"/restconf/data/foo:list=A%20Z", URI({
                                                       {{"foo", "list"}, {"A Z"}},
                                                   })},
                 {"/restconf/data/foo:list=A%25Z", URI({
                                                       {{"foo", "list"}, {"A%Z"}},
                                                   })},
                 {"/restconf/data", URI({}, {})},
                 {"/restconf/data/", URI({}, {})},

                 // RFC 8527 uris
                 {"/restconf/ds/hello:world", URI(URIPrefix(URIPrefix::Type::NMDADatastore, ApiIdentifier{"hello", "world"}), {})},
                 {"/restconf/ds/ietf-datastores:running/foo:bar/list1=a", URI(URIPrefix(URIPrefix::Type::NMDADatastore, ApiIdentifier{"ietf-datastores", "running"}), {{{"foo", "bar"}}, {{"list1"}, {"a"}}})},
                 {"/restconf/ds/ietf-datastores:operational", URI(URIPrefix(URIPrefix::Type::NMDADatastore, ApiIdentifier{"ietf-datastores", "operational"}), {})},
                 {"/restconf/ds/ietf-datastores:operational/", URI(URIPrefix(URIPrefix::Type::NMDADatastore, ApiIdentifier{"ietf-datastores", "operational"}), {})},

                 // RPCs and actions
                 {"/restconf/operations/example:rpc-test", URI(URIPrefix(URIPrefix::Type::BasicRestconfOperations, boost::none), {{{"example", "rpc-test"}}})},
                 {"/restconf/data/example:tlc/list=hello-world/example-action", URI({
                                                                                    {{"example", "tlc"}},
                                                                                    {{"list"}, {"hello-world"}},
                                                                                    {{"example-action"}},
                                                                                })}}) {
            CAPTURE(uriPath);
            auto path = rousette::restconf::impl::parseUriPath(uriPath);
            REQUIRE(path == expected);
        }
    }

    SECTION("Invalid URIs")
    {
        for (const auto& uriPath : {
                 "/restconf/foo",
                 "/restconf/foo/foo:bar",
                 "/restconf/data/foo",
                 "/restconf/data/foo:",
                 "/restconf/data/:bar",
                 "/restconf/data/333:666",
                 "/restconf/data/foo:bar/lst==",
                 "/restconf/data/foo:bar/lst==key",
                 "/restconf/data/foo:bar/=key",
                 "/restconf/data/foo:bar/lst=key1,,,,=",
                 "/restconf/data/foo:bar/X=Y=instance-value",
                 "/restconf/data/foo:bar/:baz",
                 "/restconf/data/foo:list=A%xyZ",
                 "/restconf/data/foo:list=A%0zZ",
                 "/restconf/data/foo:list=A%%1Z",
                 "/restconf/data/foo:list=A%%25Z",
                 "/restconf/data/foo:list=A%2",
                 "/restconf/data/foo:list=A%2,",
                 "/restconf/data/foo:bar/list1=%%",
                 "/restconf/data/foo:bar/",
                 "/restconf/data/ foo : bar",
                 "/rest conf/data / foo:bar",
                 "/restconf/da ta/foo:bar",
                 "/restconf/data / foo:bar = key1",
                 "/restconf/data / foo:bar =key1",
                 "/restconf/ data",
                 "/restconf /data",
                 "/restconf  data",

                 "/restconf/ds",
                 "/restconf/ds/operational",
                 "/restconf/ds/ietf-datastores",
                 "/restconf/ds/ietf-datastores:",
                 "/restconf/ds/ietf-datastores:operational/foo:bar/",
             }) {

            CAPTURE(uriPath);
            REQUIRE(!rousette::restconf::impl::parseUriPath(uriPath));
        }
    }

    SECTION("Translation to libyang path")
    {
        auto ctx = libyang::Context{std::filesystem::path{CMAKE_CURRENT_SOURCE_DIR} / "tests" / "yang"};
        ctx.loadModule("example", std::nullopt, {"f1"});
        ctx.loadModule("example-augment");

        SECTION("Contextually valid paths")
        {
            SECTION("GET and PUT")
            {
                for (const auto& [httpMethod, expectedRequestType] : {std::pair<std::string, RestconfRequest::Type>{"GET", RestconfRequest::Type::GetData}, {"PUT", RestconfRequest::Type::CreateOrReplaceThisNode}}) {
                    for (const auto& [uriPath, expectedLyPath, expectedDatastore] : {
                             std::tuple<std::string, std::string, std::optional<sysrepo::Datastore>>{"/restconf/data/example:top-level-leaf", "/example:top-level-leaf", std::nullopt},
                             {"/restconf/data/example:top-level-list=hello", "/example:top-level-list[name='hello']", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0", "/example:tlc/list[name='eth0']", std::nullopt},
                             {R"(/restconf/data/example:tlc/list=et"h0)", R"(/example:tlc/list[name='et"h0'])", std::nullopt},
                             {R"(/restconf/data/example:tlc/list=et%22h0)", R"(/example:tlc/list[name='et"h0'])", std::nullopt},
                             {R"(/restconf/data/example:tlc/list=et%27h0)", R"(/example:tlc/list[name="et'h0"])", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/name", "/example:tlc/list[name='eth0']/name", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/nested=1,2,3", "/example:tlc/list[name='eth0']/nested[first='1'][second='2'][third='3']", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/nested=,2,3", "/example:tlc/list[name='eth0']/nested[first=''][second='2'][third='3']", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/nested=,2,", "/example:tlc/list[name='eth0']/nested[first=''][second='2'][third='']", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/choice1", "/example:tlc/list[name='eth0']/choice1", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/choice2", "/example:tlc/list[name='eth0']/choice2", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/collection=val", "/example:tlc/list[name='eth0']/collection[.='val']", std::nullopt},
                             {"/restconf/data/example:tlc/status", "/example:tlc/status", std::nullopt},
                             // container example:a has a container b inserted locally and also via an augment. Check that we return the correct one
                             {"/restconf/data/example:a/b", "/example:a/b", std::nullopt},
                             {"/restconf/data/example:a/b/c", "/example:a/b/c", std::nullopt},
                             {"/restconf/data/example:a/b/c/enabled", "/example:a/b/c/enabled", std::nullopt},
                             {"/restconf/data/example:a/example-augment:b", "/example:a/example-augment:b", std::nullopt},
                             {"/restconf/data/example:a/example-augment:b/c", "/example:a/example-augment:b/c", std::nullopt},
                             {"/restconf/data/example:a/example-augment:b/example-augment:c", "/example:a/example-augment:b/c", std::nullopt},
                             {"/restconf/data/example:a/example-augment:b/c/enabled", "/example:a/example-augment:b/c/enabled", std::nullopt},
                             // rfc 8527
                             {"/restconf/ds/ietf-datastores:running/example:tlc/status", "/example:tlc/status", sysrepo::Datastore::Running},
                             {"/restconf/ds/ietf-datastores:operational/example:tlc/status", "/example:tlc/status", sysrepo::Datastore::Operational},
                             {"/restconf/ds/ietf-datastores:startup/example:tlc/status", "/example:tlc/status", sysrepo::Datastore::Startup},
                             {"/restconf/ds/ietf-datastores:candidate/example:tlc/status", "/example:tlc/status", sysrepo::Datastore::Candidate},
                             {"/restconf/ds/ietf-datastores:factory-default/example:tlc/status", "/example:tlc/status", sysrepo::Datastore::FactoryDefault},
                         }) {
                        CAPTURE(httpMethod);
                        CAPTURE(expectedRequestType);
                        CAPTURE(uriPath);
                        REQUIRE(rousette::restconf::impl::parseUriPath(uriPath));
                        auto [requestType, datastore, path] = rousette::restconf::asRestconfRequest(ctx, httpMethod, uriPath);
                        REQUIRE(requestType == expectedRequestType);
                        REQUIRE(path == expectedLyPath);
                        REQUIRE(datastore == expectedDatastore);
                    }

                    for (const auto& [uriPath, expectedLyPathParent, expectedLastSegment] : {
                             std::tuple<std::string, std::string, PathSegment>{"/restconf/data/example:top-level-leaf", "", PathSegment({"example", "top-level-leaf"})},
                             {"/restconf/data/example:top-level-list=hello", "", PathSegment({"example", "top-level-list"}, {"hello"})},
                             {"/restconf/data/example:tlc/list=eth0/collection=1", "/example:tlc/list[name='eth0']", PathSegment({"example", "collection"}, {"1"})},
                             {"/restconf/data/example:tlc/status", "/example:tlc", PathSegment({"example", "status"})},
                             {"/restconf/data/example:a/example-augment:b/c", "/example:a/example-augment:b", PathSegment({"example-augment", "c"})},
                             {"/restconf/ds/ietf-datastores:startup/example:a/example-augment:b/c", "/example:a/example-augment:b", PathSegment({"example-augment", "c"})},
                         }) {
                        CAPTURE(httpMethod);
                        CAPTURE(expectedRequestType);
                        CAPTURE(uriPath);
                        auto [parentPath, lastSegment] = rousette::restconf::asLibyangPathSplit(ctx, uriPath);
                        REQUIRE(parentPath == expectedLyPathParent);
                        REQUIRE(lastSegment == expectedLastSegment);
                    }
                }

                SECTION("GET datastore resource")
                {
                    std::string uriPath;
                    std::optional<sysrepo::Datastore> expectedDatastore;

                    SECTION("/restconf/data")
                    {
                        uriPath = "/restconf/data";
                    }
                    SECTION("/restconf/ds/")
                    {
                        uriPath = "/restconf/ds/ietf-datastores:running";
                        expectedDatastore = sysrepo::Datastore::Running;
                    }

                    auto [requestType, datastore, path] = rousette::restconf::asRestconfRequest(ctx, "GET", uriPath);
                    REQUIRE(requestType == RestconfRequest::Type::GetData);
                    REQUIRE(path == "/*");
                    REQUIRE(datastore == expectedDatastore);
                }
            }

            SECTION("POST (RPC)")
            {
                std::string uri;
                std::string expectedPath;

                SECTION("RPC")
                {
                    uri = "/restconf/operations/example:test-rpc";
                    expectedPath = "/example:test-rpc";
                }
                SECTION("Action")
                {
                    uri = "/restconf/data/example:tlc/list=hello-world/example-action";
                    expectedPath = "/example:tlc/list[name='hello-world']/example-action";
                }

                CAPTURE(uri);
                auto [action, datastore, path] = rousette::restconf::asRestconfRequest(ctx, "POST", uri);
                REQUIRE(path == expectedPath);
                REQUIRE(datastore == std::nullopt);
                REQUIRE(action == RestconfRequest::Type::Execute);
            }

            SECTION("Contextually invalid paths")
            {
                int expectedCode;
                std::string expectedErrorType;
                std::string expectedErrorTag;
                std::string expectedErrorMessage;
                std::string uriPath;

                SECTION("GET and PUT")
                {
                    expectedCode = 400;
                    expectedErrorType = "application";
                    expectedErrorTag = "operation-failed";

                    SECTION("Unparseable URI")
                    {
                        uriPath = "/restconf/data///!/@akjsaosdasdlasd";
                        expectedErrorMessage = "Syntax error";
                    }

                    SECTION("Nonexistent modules and nodes")
                    {
                        SECTION("Nonexistent module")
                        {
                            uriPath = "/restconf/data/hello:world";
                            expectedErrorMessage = "Couldn't find schema node: /hello:world";
                        }
                        SECTION("Nonexistent top-level node")
                        {
                            uriPath = "/restconf/data/example:foo";
                            expectedErrorMessage = "Couldn't find schema node: /example:foo";
                        }
                        SECTION("Augment top-level node can't be top-level node")
                        {
                            uriPath = "/restconf/data/example-augment:b";
                            expectedErrorMessage = "Couldn't find schema node: /example-augment:b";
                        }
                        SECTION("Nonexistent node")
                        {
                            uriPath = "/restconf/data/example:tlc/hello-world";
                            expectedErrorMessage = "Node 'hello-world' is not a child of '/example:tlc'";
                        }
                        SECTION("Feature not enabled")
                        {
                            uriPath = "/restconf/data/example:f";
                            expectedErrorMessage = "Couldn't find schema node: /example:f";
                        }
                        SECTION("Schema node")
                        {
                            uriPath = "/restconf/data/example:tlc/list=eth0/choose";
                            expectedErrorMessage = "Node 'choose' is not a child of '/example:tlc/list'";
                        }
                        SECTION("Schema node")
                        {
                            uriPath = "/restconf/data/example:tlc/list=eth0/choose/choice1";
                            expectedErrorMessage = "Node 'choose' is not a child of '/example:tlc/list'";
                        }
                    }

                    SECTION("Invalid data resources")
                    {
                        SECTION("top-level list node")
                        {
                            uriPath = "/restconf/data/example:top-level-list";
                            expectedErrorMessage = "List '/example:top-level-list' requires 1 keys";
                        }
                        SECTION("List node")
                        {
                            uriPath = "/restconf/data/example:tlc/key-less-list";
                            expectedErrorMessage = "List '/example:tlc/key-less-list' has no keys. It can not be accessed directly";
                        }
                        SECTION("leaf-list node")
                        {
                            uriPath = "/restconf/data/example:tlc/list=eth0/collection";
                            expectedErrorMessage = "Leaf-list '/example:tlc/list/collection' requires exactly one key";
                        }
                        SECTION("RPCs")
                        {
                            expectedCode = 405;
                            expectedErrorType = "protocol";
                            expectedErrorTag = "operation-not-supported";

                            SECTION("RPC node")
                            {
                                uriPath = "/restconf/data/example:test-rpc";
                                expectedErrorMessage = "'/example:test-rpc' is an RPC/Action node";
                            }
                            SECTION("Action node")
                            {
                                uriPath = "/restconf/data/example:tlc/list=eth0/example-action";
                                expectedErrorMessage = "'/example:tlc/list/example-action' is an RPC/Action node";
                            }
                        }
                        SECTION("RPC input node")
                        {
                            uriPath = "/restconf/data/example:test-rpc/i";
                            expectedErrorMessage = "'/example:test-rpc' is an RPC/Action node, any child of it can't be requested";
                        }
                        SECTION("RPC output node")
                        {
                            uriPath = "/restconf/data/example:test-rpc/o";
                            expectedErrorMessage = "'/example:test-rpc' is an RPC/Action node, any child of it can't be requested";
                        }
                    }

                    SECTION("(Leaf-)list key handling")
                    {
                        SECTION("Not a list")
                        {
                            uriPath = "/restconf/data/example:tlc=eth0";
                            expectedErrorMessage = "No keys allowed for node '/example:tlc'";
                        }
                        SECTION("Wrong number of keys in a list")
                        {
                            uriPath = "/restconf/data/example:tlc/list=eth0,eth1";
                            expectedErrorMessage = "List '/example:tlc/list' requires 1 keys";
                        }
                        SECTION("Wrong number of keys in a leaf-list")
                        {
                            uriPath = "/restconf/data/example:tlc/list=eth0/collection=br0,eth1";
                            expectedErrorMessage = "Leaf-list '/example:tlc/list/collection' requires exactly one key";
                        }
                    }

                    SECTION("Unsupported datastore")
                    {
                        uriPath = "/restconf/ds/hello:world/example:tlc";
                        expectedErrorMessage = "Unsupported datastore hello:world";
                    }

                    for (const auto& httpMethod : {"GET"s, "PUT"s}) {
                        CAPTURE(httpMethod);
                        REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, httpMethod, uriPath),
                                               serializeErrorResponse(expectedCode, expectedErrorType, expectedErrorTag, expectedErrorMessage).c_str(),
                                               rousette::restconf::ErrorResponse);
                    }
                }

                SECTION("PUT on datastore resource")
                {
                    std::string uriPath;
                    SECTION("/restconf/data") { uriPath = "/restconf/data"; }
                    SECTION("/restconf/ds/") { uriPath = "/restconf/ds/ietf-datastores:running"; }
                    REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, "PUT", uriPath),
                                           serializeErrorResponse(400, "application", "operation-failed", "'/' is not a data resource").c_str(),
                                           rousette::restconf::ErrorResponse);
                }

                SECTION("POST")
                {
                    expectedCode = 400;
                    expectedErrorType = "protocol";
                    expectedErrorTag = "operation-failed";

                    SECTION("Operation resource")
                    {
                        SECTION("RPC node missing")
                        {
                            uriPath = "/restconf/operations";
                            expectedErrorMessage = "'/' is not an operation resource";
                        }
                        SECTION("RPC must be invoked via /restconf/operations")
                        {
                            uriPath = "/restconf/data/example:test-rpc";
                            expectedErrorMessage = "RPC '/example:test-rpc' must be requested using operation prefix";
                        }
                        SECTION("actions must be invoked via /restconf/data")
                        {
                            uriPath = "/restconf/operations/example:tlc/list=eth0/example-action";
                            expectedErrorMessage = "Action '/example:tlc/list/example-action' must be requested using data prefix";
                        }

                        SECTION("RPC and action input/output nodes")
                        {
                            expectedErrorType = "application";
                            SECTION("RPC")
                            {
                                expectedErrorMessage = "'/example:test-rpc' is an RPC/Action node, any child of it can't be requested";

                                SECTION("Input node")
                                {
                                    uriPath = "/restconf/operations/example:test-rpc/i";
                                }
                                SECTION("Output node")
                                {
                                    uriPath = "/restconf/operations/example:test-rpc/o";
                                }
                            }
                            SECTION("Action")
                            {
                                expectedErrorMessage = "'/example:tlc/list/example-action' is an RPC/Action node, any child of it can't be requested";

                                SECTION("Input node")
                                {
                                    uriPath = "/restconf/data/example:tlc/list=eth0/example-action/i";
                                }
                                SECTION("Output node")
                                {
                                    uriPath = "/restconf/data/example:tlc/list=eth0/example-action/o";
                                }
                            }
                        }
                    }

                    SECTION("Data(store) resources")
                    {
                        expectedCode = 405;
                        expectedErrorType = "application";
                        expectedErrorTag = "operation-not-supported";

                        SECTION("Data")
                        {
                            uriPath = "/restconf/data/example:tlc";
                            expectedErrorMessage = "POST method for a data resource is not yet implemented";
                        }
                        SECTION("Datastore")
                        {
                            expectedErrorMessage = "POST method for a complete-datastore resource is not yet implemented";

                            SECTION("Basic datastore")
                            {
                                uriPath = "/restconf/data/";
                            }
                            SECTION("NMDA")
                            {
                                uriPath = "/restconf/ds/ietf-datastores:running";
                            }
                        }
                    }

                    REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, "POST", uriPath),
                                           serializeErrorResponse(expectedCode, expectedErrorType, expectedErrorTag, expectedErrorMessage).c_str(),
                                           rousette::restconf::ErrorResponse);
                }
            }

            SECTION("Unsupported HTTP methods")
            {
                auto exc = serializeErrorResponse(405, "application", "operation-not-supported", "Method not allowed.");
                REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, "HEAD", "/restconf/data/example:top-level-leaf"), exc.c_str(), rousette::restconf::ErrorResponse);
                REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, "OPTIONS", "/restconf/data/example:top-level-leaf"), exc.c_str(), rousette::restconf::ErrorResponse);
                REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, "PATCH", "/restconf/data"), exc.c_str(), rousette::restconf::ErrorResponse);
                REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, "DELETE", "/restconf/data/example:top-level-leaf"), exc.c_str(), rousette::restconf::ErrorResponse);
            }
        }
    }
}
