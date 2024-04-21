#pragma once
#include "trompeloeil_doctest.h"
#include <optional>
#include <string>
#include <sysrepo-cpp/Session.hpp>
#include <vector>

#define _CHANGE(OP, KEY, VAL) {OP, KEY, VAL}
#define CREATED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Created, KEY, VAL)
#define MODIFIED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Modified, KEY, VAL)
#define DELETED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Deleted, KEY, VAL)
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

static DatastoreChangesMock ignored;

#define SUBSCRIBE_MODULE(SUBNAME, SESSION, MODULE) \
    ALLOW_CALL(ignored, change(trompeloeil::_)); \
    auto SUBNAME = datastoreChangesSubscription(SESSION, ignored, MODULE);
