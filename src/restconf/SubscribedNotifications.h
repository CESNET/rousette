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

/** @brief A configured filter referenced by name from the RPC input (stream-filter-name / selection-filter-ref). */
struct ReferencedFilter {
    /** @brief Which configured filter list the subscription refers to. */
    enum class Kind {
        StreamFilter, ///< ietf-subscribed-notifications stream-filter
        SelectionFilter, ///< ietf-yang-push selection-filter
    };

    std::string name;
    Kind kind;

    /** @brief Builds the instance xpath of the configured filter list entry this filter refers to. */
    std::string configuredXPath() const;
};

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

/** @brief Returns the configured filter the subscription refers to (by name), if any. */
std::optional<ReferencedFilter> referencedFilter(const libyang::DataNode& subscription);

/** @brief Walks up from a changed node to the enclosing configured filter list entry (stream-filter or selection-filter).
 *
 * We match on the absolute schema path, not the bare node name: the *-subtree-filter nodes are anydata and can hold
 * arbitrary user data with look-alike node names, which a name-only compare would falsely match, or one can even filter
 * on the current filter node, e.g.:
 * `/ietf-subscribed-notifications:filters/stream-filter[name=...]/stream-subtree-filter/ietf-subscribed-notifications:filters/stream-filter`
 * */
std::optional<libyang::DataNode> configuredFilterEntry(const libyang::DataNode& changeNode);

/** @brief Reads a configured filter entry (by its instance xpath) and returns its filter-spec, if any.
 *
 * Returns std::nullopt if the entry no longer exists or carries no filter-spec.
 * The node names differ between stream-filter and selection-filter, so we pick the right pair based on the entryXPath.
 * */
std::optional<std::variant<std::string, libyang::DataNodeAny>> resolveConfiguredFilter(sysrepo::Session& session, const std::string& entryXPath);
}
