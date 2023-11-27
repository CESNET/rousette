/*
 * Copyright (C) 2016-2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <experimental/iterator>
#include <sysrepo-cpp/Connection.hpp>
#include "restconf/uri.h"
#include "restconf/uri_impl.h"
#include "tests/configure.cmake.h"

namespace rousette::restconf {

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
             }) {

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
                CAPTURE(uriPath);
                REQUIRE(rousette::restconf::impl::parseUriPath(uriPath));
                auto [datastore, path] = rousette::restconf::asLibyangPath(ctx, uriPath);
                REQUIRE(path == expectedLyPath);
                REQUIRE(datastore == expectedDatastore);
            }

            for (const auto& [uriPath, expectedLyPathParent, expectedDatastore, expectedLastSegment] : {
                     std::tuple<std::string, std::string, std::optional<sysrepo::Datastore>, PathSegment>{"/restconf/data/example:top-level-leaf", "", std::nullopt, PathSegment({"example", "top-level-leaf"})},
                     {"/restconf/data/example:top-level-list=hello", "", std::nullopt, PathSegment({"example", "top-level-list"}, {"hello"})},
                     {"/restconf/data/example:tlc/list=eth0/collection=1", "/example:tlc/list[name='eth0']", std::nullopt, PathSegment({"example", "collection"}, {"1"})},
                     {"/restconf/data/example:tlc/status", "/example:tlc", std::nullopt, PathSegment({"example", "status"})},
                     {"/restconf/data/example:a/example-augment:b/c", "/example:a/example-augment:b", std::nullopt, PathSegment({"example-augment", "c"})},
                     {"/restconf/ds/ietf-datastores:startup/example:a/example-augment:b/c", "/example:a/example-augment:b", sysrepo::Datastore::Startup, PathSegment({"example-augment", "c"})},
                 }) {
                CAPTURE(uriPath);
                auto [datastoreAndPathParent, lastSegment] = rousette::restconf::asLibyangPathSplit(ctx, uriPath);
                REQUIRE(datastoreAndPathParent.path == expectedLyPathParent);
                REQUIRE(datastoreAndPathParent.datastore == expectedDatastore);
                REQUIRE(lastSegment == expectedLastSegment);
            }
        }

        SECTION("Contextually invalid paths")
        {
            for (const auto& uriPath : std::vector<std::string>{
                     "/restconf/data/hello:world", // nonexistent module
                     "/restconf/data/example:foo", // nonexistent top-level node
                     "/restconf/data/example-augment:b", // nonexistent top-level node
                     "/restconf/data/example:tlc/hello-world", // nonexistent node
                     "/restconf/data/example:f", // feature not enabled
                     "/restconf/data/example:top-level-list", // list is not a data resource
                     "/restconf/data/example:tlc/key-less-list", // list is not a data resource
                     "/restconf/data/example:tlc/list=eth0/collection", // leaf-list is not a data resource
                     "/restconf/data/example:test-rpc", // RPC is not a data resource
                     "/restconf/data/example:test-rpc/i", // RPC input node is not a data resource
                     "/restconf/data/example:test-rpc/o", // RPC output node is not a data resource
                     "/restconf/data/example:tlc=eth0", // node not a (leaf-)list
                     "/restconf/data/example:tlc/list=eth0,eth1", // wrong number of list elements
                     "/restconf/data/example:tlc/list=eth0/collection=br0,eth1", // wrong number of keys for a leaf-list
                     "/restconf/data/example:tlc/list=eth0/choose", // schema nodes should not be visible
                     "/restconf/data/example:tlc/list=eth0/choose/choice1", // schema nodes should not be visible
                     "/restconf/ds/hello:world/example:tlc", // unsupported datastore
                 }) {
                CAPTURE(uriPath);
                REQUIRE(rousette::restconf::impl::parseUriPath(uriPath));
                REQUIRE_THROWS_AS(rousette::restconf::asLibyangPath(ctx, uriPath), rousette::restconf::InvalidURIException);
            }
        }
    }
}
