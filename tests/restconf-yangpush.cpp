/*
 * Copyright (C) 2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <atomic>
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/utils.hpp>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/event_watchers.h"
#include "tests/pretty_printers.h"
#include "tests/restconf-http-calls.h"
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

    SECTION("changing a configured selection-filter updates YANG push subscriptions that use it")
    {
        RestconfYangPushWatcher ypWatcher(srConn.sessionStart().getContext());
        ypWatcher.setDataFormat(libyang::DataFormat::JSON);

        // minimal data in the startup datastore; the configured filter decides which node is pushed
        srSess.switchDatastore(sysrepo::Datastore::Startup);
        srSess.setItem("/example:top-level-leaf", "42");
        srSess.setItem("/example:top-level-list[name='key1']", std::nullopt);
        srSess.applyChanges();

        // the configured selection-filter initially selects top-level-leaf
        srSess.switchDatastore(sysrepo::Datastore::Running);
        srSess.setItem("/ietf-subscribed-notifications:filters/ietf-yang-push:selection-filter[filter-id='flt']/datastore-xpath-filter", "/example:top-level-leaf");
        srSess.applyChanges();
        srSess.switchDatastore(sysrepo::Datastore::Operational);

        YangPushPeriodic yp;
        yp.period = 50ms;
        yp.datastore = sysrepo::Datastore::Startup;
        yp.filter = FilterName{"flt"};

        std::atomic<bool> leafDelivered = false;
        expectations.emplace_back(NAMED_REQUIRE_CALL(ypWatcher, data(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})")).IN_SEQUENCE(seq1).TIMES(AT_LEAST(1)).LR_SIDE_EFFECT(leafDelivered = true));
        expectations.emplace_back(NAMED_REQUIRE_CALL(ypWatcher, data(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-list":[{"name":"key1"}]}}})")).IN_SEQUENCE(seq1).TIMES(AT_LEAST(1)));

        auto uri = establishSubscription(SERVER_ADDRESS, SERVER_PORT, srSess.getContext(), libyang::DataFormat::JSON, {AUTH_ROOT}, "encode-json", yp).url;

        PREPARE_LOOP_WITH_EXCEPTIONS;
        std::jthread notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;

            // make sure the original filter is in effect (at least one message with old filter was delivered)
            while (!leafDelivered) {
                std::this_thread::sleep_for(10ms);
            }

            // re-point the configured filter to the list; applyChanges() is synchronous and waits for our
            auto cfg = sysrepo::Connection{}.sessionStart(sysrepo::Datastore::Running);
            cfg.setItem("/ietf-subscribed-notifications:filters/ietf-yang-push:selection-filter[filter-id='flt']/datastore-xpath-filter", "/example:top-level-list");
            cfg.applyChanges();

            waitForCompletionAndBitMore(seq1);
        }));

        std::map<std::string, std::string> streamHeaders;
        streamHeaders.insert(AUTH_ROOT);
        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, ypWatcher, uri, streamHeaders);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("modify-subscription changes the period of a YANG push periodic subscription")
    {
        // establish with a long period, shorten it via modify-subscription, and check the updates speed up
        RestconfYangPushWatcher ypWatcher(srConn.sessionStart().getContext());
        ypWatcher.setDataFormat(libyang::DataFormat::JSON);

        YangPushPeriodic yp;
        yp.period = 2000ms;
        yp.datastore = sysrepo::Datastore::Startup;

        std::atomic<bool> initialDelivered = false;
        std::vector<std::chrono::steady_clock::time_point> fastUpdateTimes;

        expectations.emplace_back(NAMED_REQUIRE_CALL(ypWatcher, data(R"({"ietf-yang-push:push-update":{"datastore-contents":{}}})")).IN_SEQUENCE(seq1).TIMES(AT_LEAST(1)).LR_SIDE_EFFECT(initialDelivered = true));
        expectations.emplace_back(NAMED_REQUIRE_CALL(ypWatcher, data(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})")).IN_SEQUENCE(seq1).TIMES(AT_LEAST(5)).LR_SIDE_EFFECT(fastUpdateTimes.emplace_back(std::chrono::steady_clock::now())));

        auto [id, uri, replayStartTimeRevision] = establishSubscription(SERVER_ADDRESS, SERVER_PORT, srSess.getContext(), libyang::DataFormat::JSON, {AUTH_ROOT}, "encode-json", yp);

        expectations.emplace_back(NAMED_REQUIRE_CALL(ypWatcher, data(R"({"ietf-subscribed-notifications:subscription-modified":{"id":)" + std::to_string(id) + R"(,"ietf-yang-push:datastore":"ietf-datastores:startup","ietf-yang-push:periodic":{"period":5}}})")));

        PREPARE_LOOP_WITH_EXCEPTIONS;
        std::jthread notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;

            while (!initialDelivered) {
                std::this_thread::sleep_for(10ms);
            }

            // set a leaf so the updates after the modify carry distinct content
            auto sess = sysrepo::Connection{}.sessionStart(sysrepo::Datastore::Startup);
            sess.setItem("/example:top-level-leaf", "42");
            sess.applyChanges();

            auto body = R"({"ietf-subscribed-notifications:input": { "id": )" + std::to_string(id) + R"(, "ietf-yang-push:datastore": "ietf-datastores:startup", "ietf-yang-push:periodic": { "period": 5 } }})"; // 5 centiseconds = 50ms
            REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:modify-subscription", {AUTH_ROOT, CONTENT_TYPE_JSON}, body) == Response{204, noContentTypeHeaders, ""});

            waitForCompletionAndBitMore(seq1);
        }));

        std::map<std::string, std::string> streamHeaders;
        streamHeaders.insert(AUTH_ROOT);
        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, ypWatcher, uri, streamHeaders);
        RUN_LOOP_WITH_EXCEPTIONS;

        // at 50ms five updates span ~200ms; the original 2s period would have needed ~8s
        REQUIRE(fastUpdateTimes.size() >= 5);
        REQUIRE(fastUpdateTimes.back() - fastUpdateTimes.front() < 1500ms);
    }

    SECTION("modify-subscription changes the dampening-period of a YANG push on-change subscription")
    {
        // with a long dampening period rapid changes coalesce into one update; drop it to zero and each is reported on its own
        RestconfYangPushWatcher ypWatcher(srConn.sessionStart().getContext());
        ypWatcher.setDataFormat(libyang::DataFormat::JSON);

        // pre-create the leaf so every later change is a uniform 'replace'
        srSess.switchDatastore(sysrepo::Datastore::Running);
        srSess.setItem("/example:top-level-leaf", "0");
        srSess.applyChanges();
        srSess.switchDatastore(sysrepo::Datastore::Operational);

        YangPushOnChange yp;
        yp.datastore = sysrepo::Datastore::Running;
        yp.syncOnStart = sysrepo::SyncOnStart::No;
        yp.dampeningPeriod = 2000ms;

        auto replaceUpdate = [](const std::string& value) {
            return R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"replace","target":"/example:top-level-leaf","value":{"example:top-level-leaf":")" + value + R"("}}]}}}})";
        };

        std::atomic<bool> initialDelivered = false;

        expectations.emplace_back(NAMED_REQUIRE_CALL(ypWatcher, data(replaceUpdate("1"))).IN_SEQUENCE(seq1).LR_SIDE_EFFECT(initialDelivered = true));
        for (const std::string value : {"2", "3", "4", "5", "6"}) {
            expectations.emplace_back(NAMED_REQUIRE_CALL(ypWatcher, data(replaceUpdate(value))).IN_SEQUENCE(seq1));
        }

        auto [id, uri, replayStartTimeRevision] = establishSubscription(SERVER_ADDRESS, SERVER_PORT, srSess.getContext(), libyang::DataFormat::JSON, {AUTH_ROOT}, "encode-json", yp);

        expectations.emplace_back(NAMED_REQUIRE_CALL(ypWatcher, data(R"({"ietf-subscribed-notifications:subscription-modified":{"id":)" + std::to_string(id) + R"(,"ietf-yang-push:datastore":"ietf-datastores:running","ietf-yang-push:on-change":{"dampening-period":0}}})")));

        PREPARE_LOOP_WITH_EXCEPTIONS;
        std::jthread notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;

            auto sess = sysrepo::Connection{}.sessionStart(sysrepo::Datastore::Running);

            sess.setItem("/example:top-level-leaf", "1");
            sess.applyChanges();
            while (!initialDelivered) {
                std::this_thread::sleep_for(10ms);
            }

            auto body = R"({"ietf-subscribed-notifications:input": { "id": )" + std::to_string(id) + R"(, "ietf-yang-push:datastore": "ietf-datastores:running", "ietf-yang-push:on-change": { "dampening-period": 0 } }})";
            REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:modify-subscription", {AUTH_ROOT, CONTENT_TYPE_JSON}, body) == Response{204, noContentTypeHeaders, ""});

            for (const auto* value : {"2", "3", "4", "5", "6"}) {
                sess.setItem("/example:top-level-leaf", value);
                sess.applyChanges();
                std::this_thread::sleep_for(60ms);
            }

            waitForCompletionAndBitMore(seq1);
        }));

        std::map<std::string, std::string> streamHeaders;
        streamHeaders.insert(AUTH_ROOT);
        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, ypWatcher, uri, streamHeaders);
        RUN_LOOP_WITH_EXCEPTIONS;
    }
}
