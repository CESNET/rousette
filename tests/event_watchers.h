#pragma once
#include "trompeloeil_doctest.h"
#include <optional>
#include <string>
#include <sysrepo-cpp/Subscription.hpp>
#include <vector>

#define _CHANGE(OP, KEY, VAL) {OP, KEY, VAL}
#define CREATED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Created, KEY, VAL)
#define MODIFIED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Modified, KEY, VAL)
#define DELETED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Deleted, KEY, VAL)
#define MOVED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Moved, KEY, VAL)
#define EXPECT_CHANGE(...) REQUIRE_CALL(dsChangesMock, change((std::vector<SrChange>{__VA_ARGS__}))).IN_SEQUENCE(seq1).TIMES(1)

struct SrChange {
    sysrepo::ChangeOperation operation;
    std::string nodePath;
    std::optional<std::string> currentValue;
    bool operator==(const SrChange&) const = default;
};

struct DatastoreChangesMock {
    MAKE_MOCK1(change, void(const std::vector<SrChange>&));
    MAKE_MOCK1(contentAfterChange, void(const std::optional<std::string>&));
};

sysrepo::Subscription datastoreNewStateSubscription(sysrepo::Session& session, DatastoreChangesMock& dsChangesMock, const std::string& moduleName);
sysrepo::Subscription datastoreChangesSubscription(sysrepo::Session& session, DatastoreChangesMock& dsChangesMock, const std::string& moduleName);

static DatastoreChangesMock testMockForUntrackedModuleWrites;

#define SUBSCRIBE_MODULE(SUBNAME, SESSION, MODULE) \
    ALLOW_CALL(testMockForUntrackedModuleWrites, change(trompeloeil::_)); \
    auto SUBNAME = datastoreChangesSubscription(SESSION, testMockForUntrackedModuleWrites, MODULE);

struct RestconfNotificationWatcher {
    libyang::Context ctx;
    libyang::DataFormat dataFormat;

    RestconfNotificationWatcher(const libyang::Context& ctx);
    void setDataFormat(const libyang::DataFormat dataFormat);
    virtual void dataEvent(const std::string& msg) const;
    void commentEvent(const std::string& msg) const;

    MAKE_CONST_MOCK1(comment, void(const std::string&));
    MAKE_CONST_MOCK1(data, void(const std::string&));
};

struct RestconfYangPushWatcher : public RestconfNotificationWatcher {
    using RestconfNotificationWatcher::RestconfNotificationWatcher;
    void dataEvent(const std::string& msg) const override;
};

#define EXPECT_NOTIFICATION(DATA, SEQ) expectations.emplace_back(NAMED_REQUIRE_CALL(netconfWatcher, data(DATA)).IN_SEQUENCE(SEQ));
#define EXPECT_YP_UPDATE(DATA) expectations.emplace_back(NAMED_REQUIRE_CALL(ypWatcher, data(DATA)).IN_SEQUENCE(seq1));
