/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/
#pragma once

#include <libyang-cpp/DataNode.hpp>
#include <libyang-cpp/Enum.hpp>
#include <optional>
#include <string>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/Subscription.hpp>
#include <variant>

namespace rousette::restconf {

constexpr auto streamFilter = "/ietf-subscribed-notifications:filters/stream-filter";
constexpr auto streamFilterKey = "name";
constexpr auto selectionFilter = "/ietf-subscribed-notifications:filters/ietf-yang-push:selection-filter";
constexpr auto selectionFilterKey = "filter-id";

/** @brief Builds a sysrepo dynamic subscription from a subscription data tree.
 *
 * Dispatches on the target to a subscribed-notifications stream, a YANG-push periodic, or a YANG-push
 * on-change subscription.
 */
sysrepo::DynamicSubscription makeSubscription(sysrepo::Session& session, const libyang::DataNode& subscription);

/** @brief Reads the subscription's `encoding` leaf, returning @p fallback when it is absent.
 *
 * Only the encode-json and encode-xml identities are supported; anything else throws.
 */
libyang::DataFormat subscriptionEncoding(const libyang::DataNode& subscription, libyang::DataFormat fallback);

/** @brief Creates a filter for the subscription.
 *
 * Filters for YANG Push and for subscribed notifications are specified in the same way,
 * only in a different YANG node. The same holds for filter resolution.
 * */
std::optional<std::variant<std::string, libyang::DataNodeAny>> createFilter(
    sysrepo::Session& session,
    const libyang::DataNode& node,
    const std::string& filterListPath,
    const std::string& filterListKey,
    const std::string& xpathFilterPath,
    const std::string& subtreeFilterPath,
    const std::string& filterNamePath);
}
