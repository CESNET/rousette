#include <spdlog/spdlog.h>
#include "UniqueResource.h"
#include "event_watchers.h"

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

RestconfNotificationWatcher::RestconfNotificationWatcher(const libyang::Context& ctx)
    : ctx(ctx)
    , dataFormat(libyang::DataFormat::JSON)
{
}

void RestconfNotificationWatcher::setDataFormat(const libyang::DataFormat dataFormat)
{
    this->dataFormat = dataFormat;
}

void RestconfNotificationWatcher::operator()(const std::string& msg) const
{
    spdlog::trace("Client received data: {}", msg);
    auto notifDataNode = ctx.parseOp(msg,
                                     dataFormat,
                                     dataFormat == libyang::DataFormat::JSON ? libyang::OperationType::NotificationRestconf : libyang::OperationType::NotificationNetconf);

    // parsing nested notifications does not return the data tree root node but the notification data node
    auto dataRoot = notifDataNode.op;
    while (dataRoot->parent()) {
        dataRoot = *dataRoot->parent();
    }

    data(*dataRoot->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::Shrink));
}
