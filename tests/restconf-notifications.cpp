/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
static const auto SERVER_PORT = "10088";
#include <latch>
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/pretty_printers.h"

#define EXPECT_NOTIFICATION(DATA) expectations.emplace_back(NAMED_REQUIRE_CALL(netconfWatcher, data(DATA)).IN_SEQUENCE(seq1));
#define SEND_NOTIFICATION(DATA) notifSession.sendNotification(*ctx.parseOp(DATA, libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);

using namespace std::chrono_literals;

struct NotificationWatcher {
    libyang::Context ctx;
    libyang::DataFormat dataFormat;

    NotificationWatcher(const libyang::Context& ctx, libyang::DataFormat dataFormat)
        : ctx(ctx)
        , dataFormat(dataFormat)
    {
    }

    void operator()(const std::string& msg) const
    {
        spdlog::trace("Client received data: {}", msg);
        auto notifDataNode = ctx.parseOp(msg, dataFormat, dataFormat == libyang::DataFormat::JSON ? libyang::OperationType::NotificationRestconf : libyang::OperationType::NotificationNetconf);

        // parsing nested notifications does not return the data tree root node but the notification data node
        auto dataRoot = notifDataNode.op;
        while (dataRoot->parent()) {
            dataRoot = *dataRoot->parent();
        }

        data(*dataRoot->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::Shrink));
    }

    MAKE_CONST_MOCK1(data, void(const std::string&));
};

struct SSEClient {
    std::shared_ptr<ng_client::session> client;
    boost::asio::deadline_timer t;

    SSEClient(
        boost::asio::io_service& io,
        std::latch& requestSent,
        const NotificationWatcher& notification,
        const std::string& uri,
        const std::map<std::string, std::string>& headers,
        const boost::posix_time::seconds silenceTimeout = boost::posix_time::seconds(3))
        : client(std::make_shared<ng_client::session>(io, SERVER_ADDRESS, SERVER_PORT))
        , t(io, silenceTimeout)
    {
        ng::header_map reqHeaders;
        for (const auto& [name, value] : headers) {
            reqHeaders.insert({name, {value, false}});
        }

        // shutdown the client after a period of no traffic
        t.async_wait([maybeClient = std::weak_ptr<ng_client::session>{client}](const boost::system::error_code&) {
            if (auto client = maybeClient.lock()) {
                client->shutdown();
            }
        });

        client->on_connect([&, uri, reqHeaders](auto) {
            boost::system::error_code ec;

            auto req = client->submit(ec, "GET", SERVER_ADDRESS_AND_PORT + uri, "", reqHeaders);
            requestSent.count_down();
            req->on_response([&](const ng_client::response& res) {
                res.on_data([&](const uint8_t* data, std::size_t len) {
                    for (const auto& event : parseEvents(std::string(reinterpret_cast<const char*>(data), len))) {
                        notification(event);
                    }
                });
            });
        });

        client->on_error([&](const boost::system::error_code& ec) {
            throw std::runtime_error{"HTTP client error: " + ec.message()};
        });
    }

    static std::vector<std::string> parseEvents(const std::string& msg)
    {
        static const std::string prefix = "data:";

        std::vector<std::string> res;
        std::istringstream iss(msg);
        std::string line;
        std::string event;

        while (std::getline(iss, line)) {
            if (line.compare(0, prefix.size(), prefix) == 0) {
                event += line.substr(prefix.size());
            } else if (line.empty()) {
                res.emplace_back(std::move(event));
                event.clear();
            }
        }
        return res;
    }
};

TEST_CASE("NETCONF notification streams")
{
    trompeloeil::sequence seq1;
    spdlog::set_level(spdlog::level::trace);

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};
    setupRealNacm(srSess);

    SECTION("NETCONF streams")
    {
        // parent for nested notification
        srSess.switchDatastore(sysrepo::Datastore::Operational);
        srSess.setItem("/example:tlc/list[name='k1']/choice1", "something must me here");
        srSess.applyChanges();

        std::vector<std::string> notificationsJSON{
            R"({"example:eventA":{"message":"blabla","progress":11}})",
            R"({"example:eventB":{}})",
            R"({"example-notif:something-happened":{}})",
            R"({"example:eventA":{"message":"almost finished","progress":99}})",
            R"({"example:tlc":{"list":[{"name":"k1","notif":{"message":"nested"}}]}})",
        };
        std::vector<std::string> expectedNotificationsJSON;

        std::string uri;
        libyang::DataFormat dataFormat;
        std::map<std::string, std::string> headers;

        SECTION("XML stream")
        {
            dataFormat = libyang::DataFormat::XML;
            headers = {AUTH_ROOT};

            SECTION("No filter")
            {
                uri = "/streams/NETCONF/XML";
                expectedNotificationsJSON = notificationsJSON;
            }
            SECTION("Filter")
            {
                uri = "/streams/NETCONF/XML?filter=/example:eventA";
                expectedNotificationsJSON = {notificationsJSON[0], notificationsJSON[3]};
            }
        }

        SECTION("JSON stream")
        {
            uri = "/streams/NETCONF/JSON";
            dataFormat = libyang::DataFormat::JSON;

            SECTION("anonymous user cannot read example-notif module")
            {
                expectedNotificationsJSON = {notificationsJSON[0], notificationsJSON[1], notificationsJSON[3], notificationsJSON[4]};
            }

            SECTION("root user")
            {
                headers = {AUTH_ROOT};
                expectedNotificationsJSON = notificationsJSON;
            }
        }

        boost::asio::io_service io;

        std::latch requestSent(1);

        std::jthread notificationThread = std::jthread([&]() {
            auto notifSession = sysrepo::Connection{}.sessionStart();
            auto ctx = notifSession.getContext();

            // wait until the client requests
            requestSent.wait();

            SEND_NOTIFICATION(notificationsJSON[0]);
            SEND_NOTIFICATION(notificationsJSON[1]);
            std::this_thread::sleep_for(33ms); // simulate some delays
            SEND_NOTIFICATION(notificationsJSON[2]);
            std::this_thread::sleep_for(125ms);
            SEND_NOTIFICATION(notificationsJSON[3]);
            SEND_NOTIFICATION(notificationsJSON[4]);

            // stop io_service after everything is processed
            waitForCompletionAndBitMore(seq1);
            io.stop();
        });

        NotificationWatcher netconfWatcher(srConn.sessionStart().getContext(), dataFormat);
        SSEClient cli(io, requestSent, netconfWatcher, uri, headers);

        for (const auto& notif : expectedNotificationsJSON) {
            EXPECT_NOTIFICATION(notif);
        }

        io.run();
    }

    SECTION("Invalid URLs")
    {
        REQUIRE(get("/streams/NETCONF/", {}) == Response{404, plaintextHeaders, "Invalid stream"});
        REQUIRE(get("/streams/NETCONF/", {AUTH_ROOT}) == Response{404, plaintextHeaders, "Invalid stream"});
        REQUIRE(get("/streams/NETCONF/bla", {}) == Response{404, plaintextHeaders, "Invalid stream"});
    }

    SECTION("Invalid parameters")
    {
        REQUIRE(get("/streams/NETCONF/XML?filter=.878", {}) == Response{400, plaintextHeaders, "Couldn't create notification subscription: SR_ERR_INVAL_ARG\n XPath \".878\" does not select any notifications. (SR_ERR_INVAL_ARG)"});
        REQUIRE(get("/streams/NETCONF/XML?filter=", {}) == Response{400, plaintextHeaders, "Query parameters syntax error"});
    }

    SECTION("RESTCONF state")
    {
        // no replays so sending a notification does not trigger replay-* leafs
        srSess.sendNotification(*srSess.getContext().parseOp(R"({"example:eventB": {}})", libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);

        SECTION("Stream location rewriting")
        {
            REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams", {AUTH_ROOT, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-restconf-monitoring:restconf-state": {
    "streams": {
      "stream": [
        {
          "name": "NETCONF",
          "description": "Default NETCONF notification stream",
          "access": [
            {
              "encoding": "xml",
              "location": "http://example.net/streams/NETCONF/XML"
            },
            {
              "encoding": "json",
              "location": "http://example.net/streams/NETCONF/JSON"
            }
          ]
        }
      ]
    }
  }
}
)"});

            // no forwarded header means we can't add schema and host prefix
            REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams", {AUTH_ROOT}) == Response{200, jsonHeaders, R"({
  "ietf-restconf-monitoring:restconf-state": {
    "streams": {
      "stream": [
        {
          "name": "NETCONF",
          "description": "Default NETCONF notification stream",
          "access": [
            {
              "encoding": "xml",
              "location": "/streams/NETCONF/XML"
            },
            {
              "encoding": "json",
              "location": "/streams/NETCONF/JSON"
            }
          ]
        }
      ]
    }
  }
}
)"});
        }

        SECTION("Replays on")
        {
            // announce replay support
            srConn.setModuleReplaySupport("example", true);
            REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams/stream=NETCONF/replay-support", {AUTH_ROOT, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-restconf-monitoring:restconf-state": {
    "streams": {
      "stream": [
        {
          "name": "NETCONF",
          "replay-support": true
        }
      ]
    }
  }
}
)"});

            // sending a notification with replay support on means that the timestamp leaf appears
            srSess.sendNotification(*srSess.getContext().parseOp(R"({"example:eventB": {}})", libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);
            {
                // check HTTP response code and headers
                auto resp = get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams/stream=NETCONF/replay-log-creation-time", {AUTH_ROOT, FORWARDED});
                REQUIRE(resp.equalStatusCodeAndHeaders({200, jsonHeaders, ""}));

                // the replay-log-creation-time node must be present in the output
                auto responseDataTree = srSess.getContext().parseData(resp.data, libyang::DataFormat::JSON, libyang::ParseOptions::ParseOnly);
                REQUIRE(!!responseDataTree->findPath("/ietf-restconf-monitoring:restconf-state/streams/stream[name='NETCONF']/replay-log-creation-time"));
            }

            // no more replays
            srConn.setModuleReplaySupport("example", false);
            REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams", {AUTH_ROOT, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-restconf-monitoring:restconf-state": {
    "streams": {
      "stream": [
        {
          "name": "NETCONF",
          "description": "Default NETCONF notification stream",
          "access": [
            {
              "encoding": "xml",
              "location": "http://example.net/streams/NETCONF/XML"
            },
            {
              "encoding": "json",
              "location": "http://example.net/streams/NETCONF/JSON"
            }
          ]
        }
      ]
    }
  }
}
)"});
        }
    }
}
