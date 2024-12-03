/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <algorithm>
#include <libyang-cpp/Context.hpp>
#include <libyang-cpp/DataNode.hpp>
#include <libyang-cpp/SchemaNode.hpp>
#include <libyang-cpp/Time.hpp>
#include <sysrepo-cpp/Subscription.hpp>

namespace rousette::restconf {

/** @brief Escapes key with the other type of quotes than found in the string.
 *
 *  @throws std::invalid_argument if both single and double quotes are used in the input
 * */
std::string escapeListKey(const std::string& str)
{
    auto singleQuotes = str.find('\'') != std::string::npos;
    auto doubleQuotes = str.find('\"') != std::string::npos;

    if (singleQuotes && doubleQuotes) {
        throw std::invalid_argument("Encountered mixed single and double quotes in XPath. Can't properly escape.");
    } else if (singleQuotes) {
        return '\"' + str + '\"';
    } else {
        return '\'' + str + '\'';
    }
}

/**
 * @brief Constructs list key predicate part of the XPath.
 *
 * @pre The listKeyLeafs and keyValues are of same length. This is not checked in the function because as of now the check is already done before the call.
 * @param listKeyLeafs libyang::Leaf nodes corresponding to keys
 * @param keyValues key values
 * @returns a string in the format "[key_1='value_1']...[key_n='value_n']"
 */
std::string listKeyPredicate(const std::vector<libyang::Leaf>& listKeyLeafs, const std::vector<std::string>& keyValues)
{
    std::string res;

    // FIXME: use std::views::zip in C++23
    auto itKeyValue = keyValues.begin();
    for (auto itKeyName = listKeyLeafs.begin(); itKeyName != listKeyLeafs.end(); ++itKeyName, ++itKeyValue) {
        res += '[' + itKeyName->name() + "=" + escapeListKey(*itKeyValue) + ']';
    }

    return res;
}

std::string leaflistKeyPredicate(const std::string& keyValue)
{
    return "[.=" + escapeListKey(keyValue) + ']';
}

bool isUserOrderedList(const libyang::DataNode& node)
{
    if (node.schema().nodeType() == libyang::NodeType::List) {
        return node.schema().asList().isUserOrdered();
    }

    if (node.schema().nodeType() == libyang::NodeType::Leaflist) {
        return node.schema().asLeafList().isUserOrdered();
    }

    return false;
}

/** @brief Checks if node is a key node in a maybeList node list */
bool isKeyNode(const libyang::DataNode& maybeList, const libyang::DataNode& node)
{
    if (maybeList.schema().nodeType() == libyang::NodeType::List) {
        auto listKeys = maybeList.schema().asList().keys();
        return std::any_of(listKeys.begin(), listKeys.end(), [&node](const auto& key) {
            return node.schema() == key;
        });
    }
    return false;
}


/** @brief Wraps a notification data tree with RESTCONF notification envelope. */
std::string as_restconf_notification(const libyang::Context& ctx, libyang::DataFormat dataFormat, libyang::DataNode notification, const sysrepo::NotificationTimeStamp& time)
{
    static const auto jsonNamespace = "ietf-restconf";
    static const auto xmlNamespace = "urn:ietf:params:xml:ns:netconf:notification:1.0";

    std::optional<libyang::DataNode> envelope;
    std::optional<libyang::DataNode> eventTime;
    std::string timeStr = libyang::yangTimeFormat(time, libyang::TimezoneInterpretation::Local);

    /* The namespaces for XML and JSON envelopes are different. See https://datatracker.ietf.org/doc/html/rfc8040#section-6.4 */
    if (dataFormat == libyang::DataFormat::JSON) {
        envelope = ctx.newOpaqueJSON(jsonNamespace, "notification", std::nullopt);
        eventTime = ctx.newOpaqueJSON(jsonNamespace, "eventTime", libyang::JSON{timeStr});
    } else {
        envelope = ctx.newOpaqueXML(xmlNamespace, "notification", std::nullopt);
        eventTime = ctx.newOpaqueXML(xmlNamespace, "eventTime", libyang::XML{timeStr});
    }

    // the notification data node holds only the notification data tree but for nested notification we should print the whole YANG data tree
    while (notification.parent()) {
        notification = *notification.parent();
    }

    envelope->insertChild(*eventTime);
    envelope->insertChild(notification);

    auto res = *envelope->printStr(dataFormat, libyang::PrintFlags::WithSiblings);

    // notification node comes from sysrepo and sysrepo will free this; if not unlinked then envelope destructor would try to free this as well
    notification.unlink();

    return res;
}
}
