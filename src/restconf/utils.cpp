/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <chrono>
#include <iomanip>
#include <libyang-cpp/SchemaNode.hpp>

namespace rousette::restconf {

namespace {
// cannot static_asssert(false) directly in constexpr else
template<bool flag = false> void error_wrong_precision() { static_assert(flag, "Wrong precision for fraction-digit time"); }
}

/** @short Format YANG's date-and-time from RFC6991 with specified precision */
template <typename Clock, typename Precision>
std::string yangDateTime(const std::chrono::time_point<Clock>& timePoint)
{
    auto tt = Clock::to_time_t(timePoint);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%S");

    if constexpr (std::is_same_v<Precision, std::chrono::seconds>) {
        // do nothing
    } else {
        static_assert(Precision::period::num == 1, "Unrecognized fraction format");
        auto frac = std::chrono::time_point_cast<Precision>(timePoint).time_since_epoch().count() % Precision::period::den;
        ss << '.';
        ss.fill('0');
        if constexpr (std::is_same_v<Precision, std::chrono::nanoseconds>) {
            ss.width(9);
        } else if constexpr (std::is_same_v<Precision, std::chrono::microseconds>) {
            ss.width(6);
        } else if constexpr (std::is_same_v<Precision, std::chrono::milliseconds>) {
            ss.width(3);
        } else {
            error_wrong_precision();
        }
        ss << frac << "-00:00";
    }
    return ss.str();
}

template std::string yangDateTime<std::chrono::system_clock, std::chrono::nanoseconds>(const std::chrono::time_point<std::chrono::system_clock>&);

/** @brief Escapes key with the other type of quotes than found in the string.
 *
 *  @throws std::invalid_argument if both single and double quotes used in the input
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
}
