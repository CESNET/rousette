/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#pragma once

#include "trompeloeil_doctest.h"
#include <boost/variant.hpp>
#include <experimental/iterator>
#include <optional>
#include <sstream>
#include <trompeloeil.hpp>
#include "event_watchers.h"
#include "restconf/uri.h"
#include "restconf/uri_impl.h"

// helper type for the visitor
template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
// explicit deduction guide (should not be needed as of C++20 according to https://en.cppreference.com/w/cpp/utility/variant/visit but clang16 does not compile the code without this)
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

namespace trompeloeil {
template <>
struct printer<std::optional<std::string>> {
    static void print(std::ostream& os, const std::optional<std::string>& o)
    {
        if (o) {
            os << *o;
        } else {
            os << "std::nullopt";
        }
    }
};

template <>
struct printer<SrChange> {
    static void print(std::ostream& os, const SrChange& o)
    {
        os << '{';
        os << o.operation << ", ";
        os << o.nodePath << ", ";
        printer<std::optional<std::string>>::print(os, o.currentValue);
        os << '}';
    }
};
}

namespace doctest {
template <class T>
struct StringMaker<std::optional<T>> {
    static String convert(const std::optional<T>& obj)
    {
        if (obj) {
            return ("optional{" + StringMaker<T>::convert(*obj) + "}").c_str();
        } else {
            return "nullopt{}";
        }
    }
};

template <class T>
struct StringMaker<std::vector<T>> {
    static String convert(const std::vector<T>& vec)
    {
        std::ostringstream oss;
        oss << "[";

        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it != vec.begin()) {
                oss << ", ";
            }
            oss << StringMaker<T>::convert(*it);
        }

        oss << "]";
        return oss.str().c_str();
    }
};

template <class T>
struct StringMaker<std::set<T>> {
    static String convert(const std::set<T>& vec)
    {
        std::ostringstream oss;
        oss << "{";

        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it != vec.begin()) {
                oss << ", ";
            }
            oss << StringMaker<T>::convert(*it);
        }

        oss << "}";
        return oss.str().c_str();
    }
};

template <>
struct StringMaker<rousette::restconf::impl::URIPath> {
    static String convert(const rousette::restconf::impl::URIPath& obj)
    {
        return StringMaker<decltype(obj.segments)>::convert(obj.segments);
    }
};

template <>
struct StringMaker<rousette::restconf::ApiIdentifier> {
    static String convert(const rousette::restconf::ApiIdentifier& obj)
    {
        std::string ret = "ApiIdentifier{prefix=";
        if (obj.prefix) {
            ret += "'" + *obj.prefix + "'";
        } else {
            ret += "nullopt{}";
        }

        ret += ", ident='" + obj.identifier + "'}";
        return ret.c_str();
    }
};

template <>
struct StringMaker<rousette::restconf::PathSegment> {
    static String convert(const rousette::restconf::PathSegment& obj)
    {
        std::string ret = "Segment{";
        ret += StringMaker<decltype(obj.apiIdent)>::convert(obj.apiIdent).c_str();
        ret += " keys=";
        ret += StringMaker<decltype(obj.keys)>::convert(obj.keys).c_str();
        ret += "}";
        return ret.c_str();
    }
};

template <>
struct StringMaker<rousette::restconf::queryParams::QueryParamValue> {
    static String convert(const rousette::restconf::queryParams::QueryParamValue& obj)
    {
        return std::visit(overloaded{
            [](const rousette::restconf::queryParams::UnboundedDepth&) -> std::string { return "UnboundedDepth{}"; },
            [](unsigned int i) { return std::to_string(i); },
            [](const std::string& str) { return str; },
            [](const rousette::restconf::queryParams::withDefaults::Explicit&) -> std::string { return "Explicit{}"; },
            [](const rousette::restconf::queryParams::withDefaults::ReportAll&) -> std::string { return "ReportAll{}"; },
            [](const rousette::restconf::queryParams::withDefaults::ReportAllTagged&) -> std::string { return "ReportAllTagged{}"; },
            [](const rousette::restconf::queryParams::withDefaults::Trim&) -> std::string { return "Trim{}"; },
            [](const rousette::restconf::queryParams::content::AllNodes&) -> std::string { return "AllNodes{}"; },
            [](const rousette::restconf::queryParams::content::OnlyConfigNodes&) -> std::string { return "Config{}"; },
            [](const rousette::restconf::queryParams::content::OnlyNonConfigNodes&) -> std::string { return "Nonconfig{}"; },
            [](const rousette::restconf::queryParams::insert::First&) -> std::string { return "First{}"; },
            [](const rousette::restconf::queryParams::insert::Last&) -> std::string { return "Last{}"; },
            [](const rousette::restconf::queryParams::insert::Before&) -> std::string { return "Before{}"; },
            [](const rousette::restconf::queryParams::insert::After&) -> std::string { return "After{}"; },
            [](const rousette::restconf::queryParams::insert::PointParsed& p) -> std::string {
                return ("PointParsed{" + StringMaker<decltype(p)>::convert(p) + "}").c_str();
            },
            [](const rousette::restconf::queryParams::fields::Expr& expr) -> std::string {
                return boost::apply_visitor([&](auto&& next) {
                    using T = std::decay_t<decltype(next)>;
                    std::string res;

                    if constexpr (std::is_same_v<T, rousette::restconf::queryParams::fields::ParenExpr> || std::is_same_v<T, rousette::restconf::queryParams::fields::SemiExpr>) {
                        if constexpr (std::is_same_v<T, rousette::restconf::queryParams::fields::ParenExpr>) {
                            res = "ParenExpr{";
                        } else {
                            res = "SemiExpr{";
                        }
                        res += StringMaker<rousette::restconf::queryParams::QueryParamValue>::convert(next.lhs).c_str();
                    } else if constexpr (std::is_same_v<T, rousette::restconf::queryParams::fields::SlashExpr>) {
                        res = "SlashExpr{" + next.lhs.name();
                    }

                    if (next.rhs) {
                        res += std::string(", ") + StringMaker<rousette::restconf::queryParams::QueryParamValue>::convert(*next.rhs).c_str();
                    }
                    return res += "}";
                }, expr);
            },
        }, obj).c_str();
    }
};

template <>
struct StringMaker<rousette::restconf::queryParams::QueryParams> {
    static String convert(const rousette::restconf::queryParams::QueryParams& obj)
    {
        std::ostringstream oss;
        oss << "{";
        std::transform(obj.begin(), obj.end(), std::experimental::make_ostream_joiner(oss, ", "), [&](const auto& e) {
            return "{" + e.first + ", " + StringMaker<decltype(e.second)>::convert(e.second).c_str() + "}";
        });
        oss << "}";
        return oss.str().c_str();
    }
};
}
