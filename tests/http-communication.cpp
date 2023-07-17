/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <iostream>
#include <nghttp2/asio_http2.h>
#include <nghttp2/asio_http2_client.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <sysrepo-cpp/Session.hpp>
#include "UniqueResource.h"
#include "restconf/Server.h"

using namespace std::string_literals;
namespace ng = nghttp2::asio_http2;
namespace ng_client = ng::client;

struct Response {
    int statusCode;
    ng::header_map headers;
    std::string data;

    bool operator==(const Response& o) const
    {
        // Skipping 'date' header. Its value will not be reproducible in simple tests
        ng::header_map myHeaders(headers);
        ng::header_map otherHeaders(headers);
        myHeaders.erase("date");
        otherHeaders.erase("date");

        return statusCode == o.statusCode && data == o.data && std::equal(myHeaders.begin(), myHeaders.end(), otherHeaders.begin(), otherHeaders.end(), [](const auto& a, const auto& b) {
                   return a.first == b.first && a.second.value == b.second.value; // Skipping 'sensitive' field from ng::header_value which does not seem important for us.
               });
    }
};

namespace doctest {

template <>
struct StringMaker<ng::header_map> {
    static String convert(const ng::header_map& m)
    {
        std::ostringstream oss;
        oss << "{\n";
        for (const auto& [k, v] : m) {
            oss << "\t"
                << "{\"" << k << "\", "
                << "{\"" << v.value << "\", " << std::boolalpha << v.sensitive << "}},\n";
        }
        oss << "}";
        return oss.str().c_str();
    }
};

template <>
struct StringMaker<Response> {
    static String convert(const Response& o)
    {
        std::ostringstream oss;

        oss << "{"
            << std::to_string(o.statusCode) << ", "
            << StringMaker<decltype(o.headers)>::convert(o.headers) << ",\n"
            << "\"" << o.data << "\",\n"
            << "}";

        return oss.str().c_str();
    }
};
}

static const auto SERVER_ADDRESS = "::1";
static const auto SERVER_PORT = "10080";
static const auto SERVER_ADDRESS_AND_PORT = "http://["s + SERVER_ADDRESS + "]" + ":" + SERVER_PORT;

Response retrieveData(auto xpath)
{
    boost::asio::io_service io_service;
    ng_client::session client(io_service, SERVER_ADDRESS, SERVER_PORT);

    std::ostringstream oss;
    ng::header_map headers;
    int statusCode;

    client.on_connect([&](auto) {
        boost::system::error_code ec;

        auto req = client.submit(ec, "GET", SERVER_ADDRESS_AND_PORT + "/restconf/data"s + xpath);
        req->on_response([&](const ng_client::response& res) {
            res.on_data([&oss](const uint8_t* data, std::size_t len) {
                oss.write(reinterpret_cast<const char*>(data), len);
            });
            statusCode = res.status_code();

            headers = res.header();
        });
        req->on_close([&client](auto) {
            client.shutdown();
        });
    });
    client.on_error([](const boost::system::error_code& ec) {
        FAIL("HTTP client error: ", ec.message());
    });
    io_service.run();

    return {statusCode, headers, oss.str()};
}

TEST_CASE("HTTP")
{
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Operational);

    auto server = rousette::restconf::Server{srConn};
    auto guard = make_unique_resource(
        [&server]() { server.listen_and_serve(SERVER_ADDRESS, SERVER_PORT, rousette::AsyncServer::ASYNCHRONOUS); },
        [&]() {
            server.stop();
        });

    const ng::header_map yangJsonRespHeaders{
        {"access-control-allow-origin", {"*", false}},
        {"content-type", {"application/yang-data+json", false}},
    };
    const ng::header_map errorRespHeaders{
        {"access-control-allow-origin", {"*", false}},
        {"content-type", {"text/plain", false}},
    };

    DOCTEST_SUBCASE("Query valid and invalid paths")
    {
        srSess.setItem("/ietf-system:system/hostname", "hostname");
        srSess.setItem("/ietf-system:system/location", "location");
        srSess.applyChanges();

        REQUIRE(retrieveData("/ietf-system:*") == Response{200, yangJsonRespHeaders, R"({
  "ietf-system:system": {
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(retrieveData("/ietf-interfaces:idk") == Response{404, errorRespHeaders, "go away"s});
    }
}
