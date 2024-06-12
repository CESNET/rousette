/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <algorithm>
#include <libyang-cpp/DataNode.hpp>
#include <libyang-cpp/SchemaNode.hpp>

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
        res += '[' + std::string{itKeyName->name()} + "=" + escapeListKey(*itKeyValue) + ']';
    }

    return res;
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
}
