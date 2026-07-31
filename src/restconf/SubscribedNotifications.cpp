/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/
#include <fmt/format.h>
#include <libyang-cpp/Time.hpp>
#include <set>
#include "restconf/Exceptions.h"
#include "restconf/SubscribedNotifications.h"
#include "restconf/utils/sysrepo.h"
#include "restconf/utils/yang.h"

namespace rousette::restconf {

namespace {

sysrepo::YangPushChange yangPushChange(const std::string& str)
{
    if (str == "create") {
        return sysrepo::YangPushChange::Create;
    } else if (str == "delete") {
        return sysrepo::YangPushChange::Delete;
    } else if (str == "insert") {
        return sysrepo::YangPushChange::Insert;
    } else if (str == "move") {
        return sysrepo::YangPushChange::Move;
    } else if (str == "replace") {
        return sysrepo::YangPushChange::Replace;
    }

    throw std::invalid_argument("Unknown YangPushChange: " + str);
}

sysrepo::DynamicSubscription makeStreamSubscription(sysrepo::Session& session, const libyang::DataNode& subscription)
{
    auto streamNode = subscription.findPath("stream");

    if (!streamNode) {
        throw ErrorResponse(400, "application", "invalid-attribute", "Stream is required");
    }

    auto stopTime = optionalTime(subscription, "stop-time");

    std::optional<sysrepo::NotificationTimeStamp> replayStartTime;
    if (auto node = subscription.findPath("replay-start-time")) {
        replayStartTime = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(node->asTerm().valueStr());
    }

    /* TODO: A change of entry in filters container must change all subscriptions that refer to that filter.
     * This is not implemented yet, but we should at least check that the provided filter name exists and is valid.
     * see for instance https://datatracker.ietf.org/doc/html/rfc8639.html#section-2.7.2 */

    return session.subscribeNotifications(
        createFilter(session, subscription, streamFilter, streamFilterKey, "stream-xpath-filter", "stream-subtree-filter", "stream-filter-name"),
        streamNode->asTerm().valueStr(),
        stopTime,
        replayStartTime);
}

sysrepo::DynamicSubscription makeYangPushOnChangeSubscription(sysrepo::Session& session, const libyang::DataNode& subscription)
{
    sysrepo::Datastore datastore = sysrepo::Datastore::Running;
    if (auto node = subscription.findPath("ietf-yang-push:datastore")) {
        datastore = datastoreFromString(node->asTerm().valueStr());
    } else {
        throw ErrorResponse(400, "application", "invalid-attribute", "Datastore is required for ietf-yang-push:on-change");
    }

    auto stopTime = optionalTime(subscription, "stop-time");

    sysrepo::SyncOnStart syncOnStart = sysrepo::SyncOnStart::No;
    if (auto node = subscription.findPath("ietf-yang-push:on-change/sync-on-start")) {
        syncOnStart = std::get<bool>(node->asTerm().value()) ? sysrepo::SyncOnStart::Yes : sysrepo::SyncOnStart::No;
    }

    std::set<sysrepo::YangPushChange> excludedChanges;
    for (const auto& node : subscription.findXPath("ietf-yang-push:on-change/excluded-change")) {
        excludedChanges.emplace(yangPushChange(node.asTerm().valueStr()));
    }

    ScopedDatastoreSwitch dsSwitch(session, datastore);
    return session.yangPushOnChange(
        createFilter(session, subscription, selectionFilter, selectionFilterKey, "ietf-yang-push:datastore-xpath-filter", "ietf-yang-push:datastore-subtree-filter", "ietf-yang-push:selection-filter-ref"),
        createInterval<std::centi>(subscription, "ietf-yang-push:on-change/dampening-period"),
        syncOnStart,
        excludedChanges,
        stopTime);
}

sysrepo::DynamicSubscription makeYangPushPeriodicSubscription(sysrepo::Session& session, const libyang::DataNode& subscription)
{
    sysrepo::Datastore datastore = sysrepo::Datastore::Running;
    if (auto node = subscription.findPath("ietf-yang-push:datastore")) {
        datastore = datastoreFromString(node->asTerm().valueStr());
    } else {
        throw ErrorResponse(400, "application", "invalid-attribute", "Datastore is required for ietf-yang-push:periodic");
    }

    auto period = createInterval<std::centi>(subscription, "ietf-yang-push:periodic/period");
    if (!period) {
        throw ErrorResponse(400, "application", "invalid-attribute", "period is required for ietf-yang-push:periodic");
    }

    auto stopTime = optionalTime(subscription, "stop-time");
    auto anchorTime = optionalTime(subscription, "ietf-yang-push:periodic/anchor-time");

    ScopedDatastoreSwitch dsSwitch(session, datastore);
    return session.yangPushPeriodic(
        createFilter(session, subscription, selectionFilter, selectionFilterKey, "ietf-yang-push:datastore-xpath-filter", "ietf-yang-push:datastore-subtree-filter", "ietf-yang-push:selection-filter-ref"),
        *period,
        anchorTime,
        stopTime);
}
}

std::optional<std::variant<std::string, libyang::DataNodeAny>> createFilter(
    sysrepo::Session& session,
    const libyang::DataNode& node,
    const std::string& filterListPath,
    const std::string& filterListKey,
    const std::string& xpathFilterPath,
    const std::string& subtreeFilterPath,
    const std::string& filterNamePath)
{
    if (auto filterNode = node.findPath(xpathFilterPath)) {
        return filterNode->asTerm().valueStr();
    }

    if (auto filterNode = node.findPath(subtreeFilterPath)) {
        return filterNode->asAny();
    }

    // resolve filter from ietf-subscribed-notifications:filters
    if (auto nameNode = node.findPath(filterNamePath)) {
        ScopedDatastoreSwitch dsSwitch(session, sysrepo::Datastore::Operational);

        const auto xpath = fmt::format("{}[{}={}]", filterListPath, filterListKey, escapeListKey(nameNode->asTerm().valueStr()));
        auto data = session.getData(xpath);
        if (!data) {
            throw ErrorResponse(400, "application", "invalid-attribute", "Name '" + nameNode->asTerm().valueStr() + "' does not refer to an existing filter/selection.");
        }

        auto filterEntry = data->findPath(xpath);
        if (!filterEntry) {
            throw ErrorResponse(400, "application", "invalid-attribute", "Name '" + nameNode->asTerm().valueStr() + "' does not refer to an existing filter/selection.");
        }

        if (auto filterNode = filterEntry->findPath(xpathFilterPath)) {
            return filterNode->asTerm().valueStr();
        }

        if (auto filterNode = filterEntry->findPath(subtreeFilterPath)) {
            return filterNode->asAny();
        }
    }

    return std::nullopt;
}

libyang::DataFormat subscriptionEncoding(const libyang::DataNode& subscription, const libyang::DataFormat fallback)
{
    /* FIXME: So far we allow only encode-json or encode-xml encoding values and not their derived values.
     * We do not know what those derived values might mean and how do they change the meaning of the encoding leaf.
     */
    if (auto encodingNode = subscription.findPath("encoding")) {
        const auto encodingStr = encodingNode->asTerm().valueStr();
        if (encodingStr == "ietf-subscribed-notifications:encode-json") {
            return libyang::DataFormat::JSON;
        } else if (encodingStr == "ietf-subscribed-notifications:encode-xml") {
            return libyang::DataFormat::XML;
        } else {
            throw ErrorResponse(400, "application", "invalid-attribute", "Unsupported subscription encoding '" + encodingStr + "'. Currently we support only 'encode-xml' and 'encode-json' identities.");
        }
    }

    return fallback;
}

sysrepo::DynamicSubscription makeSubscription(sysrepo::Session& session, const libyang::DataNode& subscription)
{
    if (subscription.findPath("stream")) {
        return makeStreamSubscription(session, subscription);
    } else if (subscription.findPath("ietf-yang-push:on-change")) {
        return makeYangPushOnChangeSubscription(session, subscription);
    } else if (subscription.findPath("ietf-yang-push:periodic")) {
        return makeYangPushPeriodicSubscription(session, subscription);
    }
    throw ErrorResponse(400, "application", "invalid-attribute", "Could not deduce if YANG push on-change, YANG push periodic or subscribed notification");
}
}
