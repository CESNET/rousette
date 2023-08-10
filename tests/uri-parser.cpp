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

namespace rousette::restconf::impl {

std::ostream& operator<<(std::ostream& os, const ApiIdentifier& obj)
{
    os << "ApiIdentifier{";
    os << "prefix=";
    if (obj.prefix) {
        os << "'" << *obj.prefix << "'";
    } else {
        os << "nullopt{}";
    }

    return os << "ident='" << obj.identifier << "'}";
}

std::ostream& operator<<(std::ostream& os, const PathSegment& obj)
{

    os << "Segment{" << obj.apiIdent << " "
       << "keys=";
    os << "[";
    std::copy(obj.keys.begin(), obj.keys.end(), std::experimental::make_ostream_joiner(os, ", "));
    return os << "]}";
}

std::ostream& operator<<(std::ostream& os, const ResourcePath& obj)
{
    os << "[";
    std::copy(obj.segments.begin(), obj.segments.end(), std::experimental::make_ostream_joiner(os, ", "));
    return os << "]";
}
}

namespace doctest {
template <>
struct StringMaker<std::optional<rousette::restconf::impl::ResourcePath>> {
    static String convert(const std::optional<rousette::restconf::impl::ResourcePath>& obj)
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
    using rousette::restconf::impl::PathSegment;
    using rousette::restconf::impl::ResourcePath;

    SECTION("Valid paths")
    {
        for (const auto& [uriPath, expected] : {
                 std::pair<std::string, ResourcePath>{"/restconf/data/x333:y666", ResourcePath({
                                                                                      {{"x333", "y666"}},
                                                                                  })},
                 {"/restconf/data/foo:bar", ResourcePath(std::vector<PathSegment>{
                                                {{"foo", "bar"}},
                                            })},
                 {"/restconf/data/foo:bar/baz", ResourcePath({
                                                    {{"foo", "bar"}},
                                                    {{"baz"}},
                                                })},
                 {"/restconf/data/foo:bar/meh:baz", ResourcePath({
                                                        {{"foo", "bar"}},
                                                        {{"meh", "baz"}},
                                                    })},
                 {"/restconf/data/foo:bar/yay/meh:baz", ResourcePath({
                                                            {{"foo", "bar"}},
                                                            {{"yay"}},
                                                            {{"meh", "baz"}},
                                                        })},
                 {"/restconf/data/foo:bar/Y=val", ResourcePath({
                                                      {{"foo", "bar"}},
                                                      {{"Y"}, {"val"}},
                                                  })},
                 {"/restconf/data/foo:bar/Y=val-ue", ResourcePath({
                                                         {{"foo", "bar"}},
                                                         {{"Y"}, {"val-ue"}},
                                                     })},
                 {"/restconf/data/foo:bar/p:lst=key1", ResourcePath({
                                                           {{"foo", "bar"}},
                                                           {{"p", "lst"}, {"key1"}},
                                                       })},

                 {"/restconf/data/foo:bar/p:lst=key1/leaf", ResourcePath({
                                                                {{"foo", "bar"}},
                                                                {{"p", "lst"}, {"key1"}},
                                                                {{"leaf"}},
                                                            })},
                 {"/restconf/data/foo:bar/lst=key1,", ResourcePath({
                                                          {{"foo", "bar"}},
                                                          {{"lst"}, {"key1", ""}},
                                                      })},
                 {"/restconf/data/foo:bar/lst=key1,,,", ResourcePath({
                                                            {{"foo", "bar"}},
                                                            {{"lst"}, {"key1", "", "", ""}},
                                                        })},
                 {"/restconf/data/foo:bar/lst=key1,/leaf", ResourcePath({
                                                               {{"foo", "bar"}},
                                                               {{"lst"}, {"key1", ""}},
                                                               {{"leaf"}},
                                                           })},
                 {"/restconf/data/foo:bar/lst=key1,key2", ResourcePath({
                                                              {{"foo", "bar"}},
                                                              {{"lst"}, {"key1", "key2"}},
                                                          })},
                 {"/restconf/data/foo:bar/lst=key1,key2/leaf", ResourcePath({
                                                                   {{"foo", "bar"}},
                                                                   {{"lst"}, {"key1", "key2"}},
                                                                   {{"leaf"}},
                                                               })},
                 {"/restconf/data/foo:bar/lst=key1,key2/lst2=key1/leaf", ResourcePath({
                                                                             {{"foo", "bar"}},
                                                                             {{"lst"}, {"key1", "key2"}},
                                                                             {{"lst2"}, {"key1"}},
                                                                             {{"leaf"}},
                                                                         })},
                 {"/restconf/data/foo:bar/lst=,key2/lst2=key1/leaf", ResourcePath({
                                                                         {{"foo", "bar"}},
                                                                         {{"lst"}, {"", "key2"}},
                                                                         {{"lst2"}, {"key1"}},
                                                                         {{"leaf"}},
                                                                     })},
                 {"/restconf/data/foo:bar/lst=,/lst2=key1/leaf", ResourcePath({
                                                                     {{"foo", "bar"}},
                                                                     {{"lst"}, {"", ""}},
                                                                     {{"lst2"}, {"key1"}},
                                                                     {{"leaf"}},
                                                                 })},
                 {"/restconf/data/foo:bar/lst=", ResourcePath({
                                                     {{"foo", "bar"}},
                                                     {{"lst"}, {""}},
                                                 })},
                 {"/restconf/data/foo:bar/lst=/leaf", ResourcePath({
                                                          {{"foo", "bar"}},
                                                          {{"lst"}, {""}},
                                                          {{"leaf"}},
                                                      })},
                 {"/restconf/data/foo:bar/prefix:lst=key1/prefix:leaf", ResourcePath({
                                                                            {{"foo", "bar"}},
                                                                            {{"prefix", "lst"}, {"key1"}},
                                                                            {{"prefix", "leaf"}},
                                                                        })},
                 {"/restconf/data/foo:bar/lst=key1,,key3", ResourcePath({
                                                               {{"foo", "bar"}},
                                                               {{"lst"}, {"key1", "", "key3"}},
                                                           })},
                 {"/restconf/data/foo:bar/lst=key%2CWithCommas,,key2C", ResourcePath({
                                                                            {{"foo", "bar"}},
                                                                            {{"lst"}, {"key,WithCommas", "", "key2C"}},
                                                                        })},
                 {R"(/restconf/data/foo:bar/list1=%2C%27"%3A"%20%2F,,foo)", ResourcePath({
                                                                                {{"foo", "bar"}},
                                                                                {{"list1"}, {R"(,'":" /)", "", "foo"}},
                                                                            })},
                 {"/restconf/data/foo:bar/list1= %20,%20,foo", ResourcePath({
                                                                   {{"foo", "bar"}},
                                                                   {{"list1"}, {"  ", " ", "foo"}},
                                                               })},
                 {"/restconf/data/foo:bar/list1= %20,%20, ", ResourcePath({
                                                                 {{"foo", "bar"}},
                                                                 {{"list1"}, {"  ", " ", " "}},
                                                             })},
                 {"/restconf/data/foo:bar/list1=žluťoučkýkůň", ResourcePath({
                                                                   {{"foo", "bar"}},
                                                                   {{"list1"}, {"žluťoučkýkůň"}},
                                                               })},
                 {"/restconf/data/foo:list=A%20Z", ResourcePath({
                                                       {{"foo", "list"}, {"A Z"}},
                                                   })},
                 {"/restconf/data/foo:list=A%25Z", ResourcePath({
                                                       {{"foo", "list"}, {"A%Z"}},
                                                   })},
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
                 "/restconf/data/ foo : bar",
                 "/rest conf/data / foo:bar",
                 "/restconf/da ta/foo:bar",
                 "/restconf/data / foo:bar = key1",
                 "/restconf/data / foo:bar =key1",
                 "/restconf/ data",
                 "/restconf /data",
                 "/restconf  data",
             }) {

            CAPTURE(uriPath);
            REQUIRE(!rousette::restconf::impl::parseUriPath(uriPath));
        }
    }

    SECTION("LY")
    {
        auto conn = sysrepo::Connection{};
        auto sess = conn.sessionStart();

        REQUIRE(sess.getContext().getModuleImplemented("ietf-system"));
        REQUIRE(sess.getContext().getModuleImplemented("ietf-interfaces"));
        REQUIRE(sess.getContext().getModuleImplemented("ietf-ip"));

        SECTION("Contextually valid paths")
        {
            for (const auto& [uriPath, expectedLyPath] : {
                     std::pair<std::string, std::optional<std::string>>{"/restconf/data/ietf-system:system/clock", "/ietf-system:system/clock"},
                     {"/restconf/data/ietf-system:system/clock/timezone-utc-offset", "/ietf-system:system/clock/timezone-utc-offset"},
                     {"/restconf/data/ietf-interfaces:interfaces/interface=eth0", "/ietf-interfaces:interfaces/interface[name='eth0']"},
                     {"/restconf/data/ietf-interfaces:interfaces/interface=et\"h0", "/ietf-interfaces:interfaces/interface[name='et\"h0']"},
                     {"/restconf/data/ietf-interfaces:interfaces/interface", "/ietf-interfaces:interfaces/interface"},
                     {"/restconf/data/ietf-interfaces:interfaces/interface=eth0/ietf-ip:ipv4/address=127.0.0.1/prefix-length", "/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/address[ip='127.0.0.1']/prefix-length"},
                     {"/restconf/data/ietf-interfaces:interfaces/interface=eth0/higher-layer-if", "/ietf-interfaces:interfaces/interface[name='eth0']/higher-layer-if"},
                     {"/restconf/data/ietf-interfaces:interfaces/interface=eth0/higher-layer-if=br0", "/ietf-interfaces:interfaces/interface[name='eth0']/higher-layer-if[.='br0']"},
                 }) {
                CAPTURE(uriPath);
                REQUIRE(rousette::restconf::impl::parseUriPath(uriPath));
                auto lyPath = rousette::restconf::asLibyangPath(sess.getContext(), uriPath);
                CAPTURE(lyPath);
                REQUIRE(lyPath == expectedLyPath);
            }
        }

        SECTION("Contextually invalid paths")
        {
            for (const auto& uriPath : std::vector<std::string>{
                     "/restconf/data/hello:world", // nonexistent module
                     "/restconf/data/ietf-system:foo", // nonexistent top-level node
                     "/restconf/data/ietf-system:system/tick-tock", // nonexistent node
                     "/restconf/data/ietf-system:system/clock/timezone-name", // feature not enabled
                     "/restconf/data/ietf-interfaces:interfaces=eth0", // node not a list
                     "/restconf/data/ietf-interfaces:interfaces/interface=eth0,eth1", // wrong number of list elements
                     "/restconf/data/ietf-interfaces:interfaces/interface=eth0/higher-layer-if=br0,eth1", // wrong number of leaf-list elements
                     //"/restconf/data/ietf-system:system/clock/timezone-utc-offset[name='foo']", // node not a list node
                 }) {
                CAPTURE(uriPath);
                REQUIRE(rousette::restconf::impl::parseUriPath(uriPath));
                REQUIRE_THROWS_AS(rousette::restconf::asLibyangPath(sess.getContext(), uriPath), std::runtime_error);
            }
        }
    }
}
