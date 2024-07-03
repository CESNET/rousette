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
#define FORWARDED                                 \
    {                                             \
        "forward", "proto=https;host=example.net" \
    }

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

    std::vector<std::string> notificationsJSON{
        R"({"example:eventA":{"message":"blabla","progress":11}})",
            R"({"example:eventB":{}})",
            R"({"example-notif:something-happened":{}})",
            R"({"example:eventA":{"message":"almost finished","progress":99}})",
            R"({"example:tlc":{"list":[{"name":"k1","notif":{"message":"nested"}}]}})",
    };
    std::vector<std::string> expectedNotificationsJSON;

    SECTION("NETCONF streams")
    {
        // parent for nested notification
        srSess.switchDatastore(sysrepo::Datastore::Operational);
        srSess.setItem("/example:tlc/list[name='k1']/choice1", "something must me here");
        srSess.applyChanges();

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
        REQUIRE(get("/streams/NETCONF/XML?filter=.878", {}) == Response{400, plaintextHeaders, R"EOF(Couldn't create notification subscription: SR_ERR_INVAL_ARG
 XPath ".878" does not select any notifications. (SR_ERR_INVAL_ARG))EOF"});
        REQUIRE(get("/streams/NETCONF/XML?filter=", {}) == Response{400, plaintextHeaders, "Query parameters syntax error"});

        REQUIRE(get("/streams/NETCONF/XML?start_time=2000-01-01T00:00:00+00:00&stop-time=1990-01-01T00:00:00+00:00", {}) == Response{400, plaintextHeaders, "Query parameters syntax error"});
    }

    SECTION("RESTCONF state")
    {
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
              "location": "https://example.net/streams/NETCONF/XML"
            },
            {
              "encoding": "json",
              "location": "https://example.net/streams/NETCONF/JSON"
            }
          ]
        }
      ]
    }
  }
}
)"});
            REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams", {AUTH_ROOT}) == Response{200, jsonHeaders, R"({

}
)"});
        }

        SECTION("Replays")
        {
            srConn.setModuleReplaySupport("example", false);

            SECTION("Without replay-log-creation-time")
            {
                std::string expected = R"({
  "ietf-restconf-monitoring:restconf-state": {
    "streams": {
      "stream": [
        {
          "name": "NETCONF",
          "description": "Default NETCONF notification stream",
          "access": [
            {
              "encoding": "xml",
              "location": "https://example.net/streams/NETCONF/XML"
            },
            {
              "encoding": "json",
              "location": "https://example.net/streams/NETCONF/JSON"
            }
          ]
        }
      ]
    }
  }
}
)";

                SECTION("No replay enabled")
                {
                    SECTION("Notifications sent")
                    {
                        srSess.sendNotification(*srSess.getContext().parseOp(R"({"example:eventB": {}})", libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);
                    }
                }

                SECTION("Replay enabled")
                {
                    srConn.setModuleReplaySupport("example", true);

                    SECTION("No notifications")
                    {
                        expected = R"({
  "ietf-restconf-monitoring:restconf-state": {
    "streams": {
      "stream": [
        {
          "name": "NETCONF",
          "description": "Default NETCONF notification stream",
          "replay-support": true,
          "access": [
            {
              "encoding": "xml",
              "location": "https://example.net/streams/NETCONF/XML"
            },
            {
              "encoding": "json",
              "location": "https://example.net/streams/NETCONF/JSON"
            }
          ]
        }
      ]
    }
  }
}
)";
                    }

                    /* TODO: It would be also nice to test what happends when replay is disabled after a notification is sent
                       but doing so would set the earliest notif. time in sysrepo and it seems that I can't reset it
                    SECTION("Replay disabled after notification sent")
                    {
                        srSess.sendNotification(*srSess.getContext().parseOp(R"({"example:eventB": {}})", libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);
                        srConn.setModuleReplaySupport("example", false);
                    }
                    */
                }

                REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams", {AUTH_ROOT, FORWARDED}) == Response{200, jsonHeaders, expected});
            }

            SECTION("Replay log creation time")
            {
                std::string expectedWithoutReplayLogCreationTime = R"({
  "ietf-restconf-monitoring:restconf-state": {
    "streams": {
      "stream": [
        {
          "name": "NETCONF",
          "description": "Default NETCONF notification stream",
          "replay-support": true,
          "access": [
            {
              "encoding": "xml",
              "location": "https://example.net/streams/NETCONF/XML"
            },
            {
              "encoding": "json",
              "location": "https://example.net/streams/NETCONF/JSON"
            }
          ]
        }
      ]
    }
  }
}
)";

                srConn.setModuleReplaySupport("example", true);

                auto start = std::chrono::system_clock::now();
                srSess.sendNotification(*srSess.getContext().parseOp(R"({"example:eventB": {}})", libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);
                auto end = std::chrono::system_clock::now();

                // We have to compare timestamps in the output so bear with me please

                // check HTTP response code and headers
                auto resp = get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams", {AUTH_ROOT, FORWARDED});
                REQUIRE(resp.equalStatusCodeAndHeaders({200, jsonHeaders, ""}));

                // parse the real output and the expected output back to libyang trees
                auto responseDataTree = srSess.getContext().parseData(resp.data, libyang::DataFormat::JSON, libyang::ParseOptions::ParseOnly);
                auto expectedDataTree = srSess.getContext().parseData(expectedWithoutReplayLogCreationTime, libyang::DataFormat::JSON, libyang::ParseOptions::ParseOnly);

                // the replay-log-creation-time node should must be present in the output
                auto replayLogCreationNode = responseDataTree->findPath("/ietf-restconf-monitoring:restconf-state/streams/stream[name='NETCONF']/replay-log-creation-time");
                REQUIRE(!!replayLogCreationNode);

                // check that the timestamp corresponds to the notification time
                auto reaplyLogCreationTime = libyang::fromYangTimeFormat<std::chrono::system_clock>(replayLogCreationNode->asTerm().valueStr());
                REQUIRE(start <= reaplyLogCreationTime);
                REQUIRE(reaplyLogCreationTime <= end);

                // finally, compare outputs with the timestamp node removed
                replayLogCreationNode->unlink();
                auto respDataTreeStr = *responseDataTree->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::WithSiblings);
                auto expeDataTreeStr = *expectedDataTree->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::WithSiblings);
                REQUIRE(respDataTreeStr == expeDataTreeStr);
            }
        }
    }

    SECTION("Replay support")
    {
        srConn.setModuleReplaySupport("example", true);
        srConn.setModuleReplaySupport("example-notif", true);

        std::string uri = "/streams/NETCONF/XML";

        boost::asio::io_service io;

        std::latch requestSent(1);
        std::jthread notificationThread;

        SECTION("Start time")
        {
            expectedNotificationsJSON = notificationsJSON;
            uri += "?start-time=" + libyang::yangTimeFormat(std::chrono::system_clock::now(), libyang::TimezoneInterpretation::Local);

            SECTION("All notifications before client connects")
            {
                notificationThread = std::jthread([&]() {
                    auto notifSession = sysrepo::Connection{}.sessionStart();
                    auto ctx = notifSession.getContext();

                    SEND_NOTIFICATION(notificationsJSON[0]);
                    SEND_NOTIFICATION(notificationsJSON[1]);
                    SEND_NOTIFICATION(notificationsJSON[2]);
                    SEND_NOTIFICATION(notificationsJSON[3]);
                    SEND_NOTIFICATION(notificationsJSON[4]);
                    requestSent.count_down();

                    waitForCompletionAndBitMore(seq1);
                    io.stop();
                });
            }

            SECTION("Some notifications before client connects")
            {
                notificationThread = std::jthread([&]() {
                    auto notifSession = sysrepo::Connection{}.sessionStart();
                    auto ctx = notifSession.getContext();

                    SEND_NOTIFICATION(notificationsJSON[0]);
                    SEND_NOTIFICATION(notificationsJSON[1]);

                    std::this_thread::sleep_for(250ms);
                    requestSent.count_down();
                    std::this_thread::sleep_for(250ms);

                    SEND_NOTIFICATION(notificationsJSON[2]);
                    SEND_NOTIFICATION(notificationsJSON[3]);
                    SEND_NOTIFICATION(notificationsJSON[4]);

                    waitForCompletionAndBitMore(seq1);
                    io.stop();
                });
            }
        }

        SECTION("Start and stop time")
        {
            auto notifSession = sysrepo::Connection{}.sessionStart();
            auto ctx = notifSession.getContext();

            expectedNotificationsJSON = {notificationsJSON[2]};

            SEND_NOTIFICATION(notificationsJSON[0]);
            SEND_NOTIFICATION(notificationsJSON[1]);

            std::this_thread::sleep_for(50ms);
            auto start = std::chrono::system_clock::now();
            SEND_NOTIFICATION(notificationsJSON[2]);
            auto end = std::chrono::system_clock::now();
            std::this_thread::sleep_for(50ms);

            uri += "?start-time=" + libyang::yangTimeFormat(start, libyang::TimezoneInterpretation::Local) + "&stop-time=" + libyang::yangTimeFormat(end, libyang::TimezoneInterpretation::Local);

            notificationThread = std::jthread([&]() {
                auto notifSession = sysrepo::Connection{}.sessionStart();
                auto ctx = notifSession.getContext();
                requestSent.count_down();
                SEND_NOTIFICATION(notificationsJSON[3]);
                SEND_NOTIFICATION(notificationsJSON[4]);

                waitForCompletionAndBitMore(seq1);
                io.stop();
            });
        }

        NotificationWatcher netconfWatcher(srConn.sessionStart().getContext(), libyang::DataFormat::XML);
        SSEClient cli(io, requestSent, netconfWatcher, uri, {AUTH_ROOT});

        for (const auto& notif : expectedNotificationsJSON) {
            EXPECT_NOTIFICATION(notif);
        }

        io.run();
    }
}
