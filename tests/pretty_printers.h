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
        return std::visit([](auto&& arg) -> std::string {
                   using T = std::decay_t<decltype(arg)>;
                   if constexpr (std::is_same_v<T, rousette::restconf::queryParams::UnboundedDepth>) {
                       return "UnboundedDepth()";
                   } else if constexpr (std::is_same_v<T, unsigned int>) {
                       return std::to_string(arg);
                   } else {
                       return "<unknown query param value>";
                   }
               },
                          obj)
            .c_str();
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
