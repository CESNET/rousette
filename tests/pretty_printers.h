/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#pragma once

#include "trompeloeil_doctest.h"
#include <experimental/iterator>
#include <optional>
#include <sstream>
#include <trompeloeil.hpp>
#include "datastoreUtils.h"
#include "restconf/uri.h"

// helper type for the visitor
template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

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

template <>
struct StringMaker<rousette::restconf::queryParams::QueryParamValue> {
    static String convert(const rousette::restconf::queryParams::QueryParamValue& obj)
    {
        return std::visit(overloaded{
            [](const rousette::restconf::queryParams::UnboundedDepth&) -> std::string { return "UnboundedDepth{}"; },
            [](unsigned int i) { return std::to_string(i); },
            [](const rousette::restconf::queryParams::withDefaults::Explicit&) -> std::string { return "Explicit{}"; },
            [](const rousette::restconf::queryParams::withDefaults::ReportAll&) -> std::string { return "ReportAll{}"; },
            [](const rousette::restconf::queryParams::withDefaults::ReportAllTagged&) -> std::string { return "ReportAllTagged{}"; },
            [](const rousette::restconf::queryParams::withDefaults::Trim&) -> std::string { return "Trim{}"; },
            [](const rousette::restconf::queryParams::content::AllNodes&) -> std::string { return "AllNodes{}"; },
            [](const rousette::restconf::queryParams::content::OnlyConfigNodes&) -> std::string { return "Config{}"; },
            [](const rousette::restconf::queryParams::content::OnlyNonConfigNodes&) -> std::string { return "Nonconfig{}"; },
            [](const rousette::restconf::queryParams::insert::First&) -> std::string { return "First{}"; },
            [](const rousette::restconf::queryParams::insert::Last&) -> std::string { return "Last{}"; },
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
