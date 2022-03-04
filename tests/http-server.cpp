/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <nghttp2/asio_http2_client.h>
#include <iostream>
#include <sstream>
#include "trompeloeil_doctest.h"
#include "restconf/Server.h"
#include "UniqueResource.hpp"

namespace std {
std::ostream& operator<<(std::ostream& s, const std::optional<std::string>& x) {
    if (!x) {
        return s << "nullopt{}";
    }
    return s << "optional{" << *x << "}";
}
}

TEST_CASE("subtree path validity") {
    auto data = std::initializer_list<std::pair<std::string, std::optional<std::string>>> {
        {{}, {}},
        {"/restconf/data", {}},
        {"/restconf/data/foo", {}},
        {"/restconf/data/foo:", {}},
        {"/restconf/data/foo:*", "foo:*"},
        {"/restconf/data/foo:*/bar", {}},
        {"/restconf/data/:bar", {}},
        {"/restconf/data/foo:bar", "foo:bar"},
        {"/restconf/data/foo:bar/baz", "foo:bar/baz"},
        {"/restconf/data/foo:bar/meh:baz", "foo:bar/meh:baz"},
        {"/restconf/data/foo:bar/yay/meh:baz", "foo:bar/yay/meh:baz"},
        {"/restconf/data/foo:bar/:baz", {}},
    };
    for (const auto& x : data) {
        const auto& input = x.first;
        const auto& xpath = x.second;
        CAPTURE(input);
        CAPTURE(xpath);
        REQUIRE(rousette::restconf::as_subtree_path(input) == xpath);
    }
}

TEST_CASE("allowed paths for anonymous read") {

    auto allowed = {
        "czechlight-roadm-device:*",
        "czechlight-roadm-device:spectrum-scan",
        "czechlight-roadm-device:something/complex",
        "czechlight-system:firmware",
    };
    auto rejected = {
        "foo:*",
        "foo:bar",
        "unrelated:wtf/czechlight-roadm-device:something/complex",
        "czechlight-system:authentication",
        "czechlight-system:*",
        "ietf-netconf-server:*",
    };

    for (const auto path : allowed) {
        CAPTURE(path);
        REQUIRE(rousette::restconf::allow_anonymous_read_for(path));
    }

    for (const auto path : rejected) {
        CAPTURE(path);
        REQUIRE(!rousette::restconf::allow_anonymous_read_for(path));
    }
}

std::string retrieveData(auto xpath)
{
    using namespace std::string_literals;
    namespace ng_client = nghttp2::asio_http2::client;

    boost::asio::io_service io_service;
    ng_client::session client(io_service, "::1", "10080");

    std::ostringstream oss;

    client.on_connect([&client, &xpath, &oss](auto) {
        boost::system::error_code ec;

        auto req = client.submit(ec, "GET", "http://[::1]:10080/restconf/data"s + xpath);
        req->on_response([&oss](const ng_client::response& res) {
            res.on_data([&oss](const uint8_t* data, std::size_t len) {
                oss.write(reinterpret_cast<const char*>(data), len);
            });
        });
        req->on_close([&client](auto) {
            client.shutdown();
        });
    });

    client.on_error([](const boost::system::error_code& ec) {
        std::cerr << "error: " << ec.message() << std::endl;
    });

    io_service.run();

    return oss.str();
}

TEST_CASE("NACM") {
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};
    auto server = rousette::restconf::Server{srConn};
    auto serverListener = make_unique_resource([&server] {
        server.listen_and_serve("::1", "10080");
    }, [&server] {
        server.stop();
    });


    std::string xpath;
    std::string expected;

    DOCTEST_SUBCASE("/example-nacm:*")
    {
        xpath = "/example-nacm:*";
        expected = R"({
  "example-nacm:allowAllLeaf": "public"
}
)";
    }

    DOCTEST_SUBCASE("/example-nacm:allowAllLeaf")
    {
        xpath = "/example-nacm:allowAllLeaf";
        expected = R"({
  "example-nacm:allowAllLeaf": "public"
}
)";
    }

    DOCTEST_SUBCASE("/example-nacm:denyAllLeaf")
    {
        xpath = "/example-nacm:denyAllLeaf";
        expected = R"({})";
    }

    REQUIRE(retrieveData(xpath) == expected);
}
