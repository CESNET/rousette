#include "UniqueResource.h"
#include "datastoreUtils.h"

namespace {
void datastoreChanges(auto session, auto& dsChangesMock, auto path)
{
    std::vector<SrChange> changes;

    for (const auto& change : session.getChanges(path)) {
        std::optional<std::string> val;

        if (change.node.isTerm()) {
            val = change.node.asTerm().valueStr();
        }

        changes.emplace_back(change.operation, change.node.path(), val);
    }

    dsChangesMock.change(changes);
}

void datastoreNewState(auto session, auto& dsChangesMock, auto path)
{
    dsChangesMock.contentAfterChange(session.getData(path)->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::WithSiblings));
}
}

sysrepo::Subscription datastoreChangesSubscription(sysrepo::Session& session, DatastoreChangesMock& dsChangesMock, const std::string& moduleName)
{
    return session.onModuleChange(
        moduleName,
        [moduleName, &dsChangesMock](auto session, auto, auto, auto, auto, auto) {
            datastoreChanges(session, dsChangesMock, "/" + moduleName + ":*//.");
            return sysrepo::ErrorCode::Ok;
        },
        std::nullopt,
        0,
        sysrepo::SubscribeOptions::DoneOnly);
}

sysrepo::Subscription datastoreNewStateSubscription(sysrepo::Session& session, DatastoreChangesMock& dsChangesMock, const std::string& moduleName)
{
    return session.onModuleChange(
        moduleName,
        [moduleName, &dsChangesMock](auto session, auto, auto, auto, auto, auto) {
            datastoreNewState(session, dsChangesMock, "/" + moduleName + ":*");
            return sysrepo::ErrorCode::Ok;
        },
        std::nullopt,
        0,
        sysrepo::SubscribeOptions::DoneOnly);
}
