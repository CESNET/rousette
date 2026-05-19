/*
 * Copyright (C) 2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/utils.hpp>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/event_watchers.h"
#include "tests/pretty_printers.h"
#include "tests/restconf_utils.h"

using namespace std::chrono_literals;
using namespace std::string_literals;

TEST_CASE("RESTCONF subscribed notifications")
{
    trompeloeil::sequence seq1, seq2;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    sysrepo::setGlobalContextOptions(sysrepo::ContextFlags::LibYangPrivParsed | sysrepo::ContextFlags::NoPrinted, sysrepo::GlobalContextEffect::Immediate);
    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    // subscribe to running data so they appear in oper ds
    srSess.switchDatastore(sysrepo::Datastore::Running);
    auto sub = srSess.onModuleChange(
        "ietf-subscribed-notifications",
        [&](auto, auto, auto, auto, auto, auto) { return sysrepo::ErrorCode::Ok; },
        "/ietf-subscribed-notifications:filters/*",
        0,
        sysrepo::SubscribeOptions::DoneOnly);
    srSess.switchDatastore(sysrepo::Datastore::Operational);

    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};
    setupRealNacm(srSess);

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    libyang::DataFormat rpcRequestEncoding = libyang::DataFormat::JSON;
    std::optional<std::string> rpcSubscriptionEncoding;
    std::optional<std::pair<std::string, std::string>> rpcRequestAuthHeader;

    SECTION("YANG push on change")
    {
        RestconfYangPushWatcher ypWatcher(srConn.sessionStart().getContext());

        YangPushOnChange yp;
        yp.datastore = sysrepo::Datastore::Running;
        yp.syncOnStart = sysrepo::SyncOnStart::No;

        SECTION("Basic test")
        {
            ypWatcher.setDataFormat(libyang::DataFormat::JSON);
            rpcRequestAuthHeader = AUTH_ROOT;
            rpcRequestEncoding = libyang::DataFormat::JSON;
            rpcSubscriptionEncoding = "encode-json";

            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"42"}}]}}}})");
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"replace","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"44"}},{"edit-id":"edit-2","operation":"create","target":"/example:top-level-list[name='key1']","value":{"example:top-level-list":[{"name":"key1"}]}}]}}}})");
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example-delete:secret[name='bla']","value":{"example-delete:secret":[{"name":"bla"}]}}]}}}})");
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"delete","target":"/example:top-level-leaf"}]}}}})");
        }

        SECTION("Only startup DS changes")
        {
            yp.datastore = sysrepo::Datastore::Startup;
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"43"}}]}}}})");
        }

        SECTION("NACM works")
        {
            rpcRequestAuthHeader = std::nullopt;
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"42"}}]}}}})");
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"replace","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"44"}},{"edit-id":"edit-2","operation":"create","target":"/example:top-level-list[name='key1']","value":{"example:top-level-list":[{"name":"key1"}]}}]}}}})");
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"delete","target":"/example:top-level-leaf"}]}}}})");
        }

        SECTION("Filter")
        {
            SECTION("XPath filter")
            {
                SECTION("Through selection-filter-ref")
                {
                    CREATE_SUBTREE_SELECTION_FILTER(srSess, "abc", "<top-level-list xmlns='http://example.tld/example' />");
                    yp.filter = FilterName{"abc"};
                }
                SECTION("Directly")
                {
                    yp.filter = FilterXPath{"/example:top-level-list"};
                }
            }

            SECTION("Subtree filter is set")
            {
                SECTION("Through selection-filter-ref")
                {
                    CREATE_SUBTREE_SELECTION_FILTER(srSess, "def", "<top-level-list xmlns='http://example.tld/example' />");
                    yp.filter = FilterName{"def"};
                }
                SECTION("Directly")
                {
                    yp.filter = *srSess.getContext().parseData("<top-level-list xmlns='http://example.tld/example' />"s, libyang::DataFormat::XML, libyang::ParseOptions::Opaque | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);
                }
            }

            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example:top-level-list[name='key1']","value":{"example:top-level-list":[{"name":"key1"}]}}]}}}})");
        }

        SECTION("Excluded changes")
        {
            yp.excludedChangeTypes = {"delete", "insert", "create", "move"};

            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"replace","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"44"}}]}}}})");
        }

        SECTION("Sync on start")
        {
            // push some data in advance
            srSess.switchDatastore(sysrepo::Datastore::Startup);
            srSess.setItem("/example:tlc/list[name='k1']/choice1", "choice1-startup");
            srSess.applyChanges();

            yp.datastore = sysrepo::Datastore::Startup;

            SECTION("Yes")
            {
                yp.syncOnStart = sysrepo::SyncOnStart::Yes;
                EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:tlc":{"list":[{"name":"k1","choice1":"choice1-startup"}]}}}})");
            }

            SECTION("No")
            {
                yp.syncOnStart = sysrepo::SyncOnStart::No;
            }

            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"43"}}]}}}})");
        }

        auto uri = establishSubscription(SERVER_ADDRESS, SERVER_PORT, srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding, yp).url;

        // The thread cooperation is described in the subscribed notification subcase

        PREPARE_LOOP_WITH_EXCEPTIONS;
        std::jthread notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            auto sess = sysrepo::Connection{}.sessionStart();
            auto ctx = sess.getContext();

            WAIT_UNTIL_SSE_CLIENT_REQUESTS;

            sess.switchDatastore(sysrepo::Datastore::Running);
            sess.setItem("/example:top-level-leaf", "42");
            sess.applyChanges();

            sess.switchDatastore(sysrepo::Datastore::Startup);
            sess.setItem("/example:top-level-leaf", "43");
            sess.applyChanges();

            std::this_thread::sleep_for(400ms);

            sess.switchDatastore(sysrepo::Datastore::Running);
            sess.setItem("/example:top-level-leaf", "44");
            sess.setItem("/example:top-level-list[name='key1']", std::nullopt);
            sess.setItem("/example-delete:secret[name='bla']", std::nullopt);
            sess.applyChanges();

            std::this_thread::sleep_for(400ms);

            sess.deleteItem("/example:top-level-leaf");
            sess.applyChanges();

            // once the main thread has processed all the notifications, stop the ASIO loop
            waitForCompletionAndBitMore(seq1);
        }));

        std::map<std::string, std::string> streamHeaders;
        if (rpcRequestAuthHeader) {
            streamHeaders.insert(*rpcRequestAuthHeader);
        }
        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, ypWatcher, uri, streamHeaders);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("YANG push periodic")
    {
        RestconfYangPushWatcher ypWatcher(srConn.sessionStart().getContext());

        YangPushPeriodic yp;
        yp.period = 50ms;
        yp.datastore = sysrepo::Datastore::Startup; // I'm intentionally avoiding running and operational datastores; they contain a lot of data (for instance, config false stuff in operational and NACM rules in running)

        SECTION("Basic")
        {
            rpcRequestAuthHeader = AUTH_ROOT;

            ypWatcher.setDataFormat(libyang::DataFormat::JSON);
            rpcRequestEncoding = libyang::DataFormat::JSON;
            rpcSubscriptionEncoding = "encode-json";

            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example-delete:secret":[{"name":"bla"}]}}})");
        }

        SECTION("NACM works")
        {
            rpcRequestAuthHeader = std::nullopt;

            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{}}})");
        }

        SECTION("Filter")
        {
            SECTION("XPath filter")
            {
                SECTION("Through selection-filter-ref")
                {
                    CREATE_SUBTREE_SELECTION_FILTER(srSess, "abc", "<top-level-leaf xmlns='http://example.tld/example' />");
                    yp.filter = FilterName{"abc"};
                }
                SECTION("Directly")
                {
                    yp.filter = FilterXPath{"/example:top-level-leaf"};
                }
            }

            SECTION("Subtree filter is set")
            {
                SECTION("Through selection-filter-ref")
                {
                    CREATE_SUBTREE_SELECTION_FILTER(srSess, "def", "<top-level-leaf xmlns='http://example.tld/example' />");
                    yp.filter = FilterName{"def"};
                }
                SECTION("Directly")
                {
                    yp.filter = *srSess.getContext().parseData("<top-level-leaf xmlns='http://example.tld/example' />"s, libyang::DataFormat::XML, libyang::ParseOptions::Opaque | libyang::ParseOptions::NoState | libyang::ParseOptions::ParseOnly);
                }
            }

            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{}}})");
        }

        auto uri = establishSubscription(SERVER_ADDRESS, SERVER_PORT, srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding, yp).url;

        // The thread cooperation is described in the subscribed notification subcase

        PREPARE_LOOP_WITH_EXCEPTIONS;
        std::jthread notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            auto sess = sysrepo::Connection{}.sessionStart();
            auto ctx = sess.getContext();

            WAIT_UNTIL_SSE_CLIENT_REQUESTS;

            std::this_thread::sleep_for(400ms);

            sess.switchDatastore(sysrepo::Datastore::Startup);
            sess.setItem("/example:top-level-leaf", "42");
            sess.applyChanges();

            std::this_thread::sleep_for(400ms);

            sess.switchDatastore(sysrepo::Datastore::Startup);
            sess.deleteItem("/example:top-level-leaf");
            sess.setItem("/example-delete:secret[name='bla']", std::nullopt);
            sess.applyChanges();

            // once the main thread has processed all the notifications, stop the ASIO loop
            waitForCompletionAndBitMore(seq1);
            waitForCompletionAndBitMore(seq2);
        }));

        std::map<std::string, std::string> streamHeaders;
        if (rpcRequestAuthHeader) {
            streamHeaders.insert(*rpcRequestAuthHeader);
        }
        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, ypWatcher, uri, streamHeaders);
        RUN_LOOP_WITH_EXCEPTIONS;
    }
}
