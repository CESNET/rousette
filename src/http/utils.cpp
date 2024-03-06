/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#include <boost/fusion/adapted/std_pair.hpp>
#include <boost/fusion/adapted/struct/adapt_struct.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/spirit/home/x3.hpp>
#include "http/utils.hpp"

namespace {
struct Mime {
    std::string mediaType;
    std::string mediaSubtype;
};

struct MediaType {
    Mime mime;
    std::vector<std::pair<std::string, std::string>> parameters;

    double qValue() const
    {
        if (auto it = std::find_if(parameters.begin(), parameters.end(), [](const auto& kv) {
                return kv.first == "q";
            });
            it != parameters.end()) {
            return std::stod(it->second);
        }
        return 1; // default q value
    };
};
}

BOOST_FUSION_ADAPT_STRUCT(Mime, mediaType, mediaSubtype);
BOOST_FUSION_ADAPT_STRUCT(MediaType, mime, parameters);

namespace rousette::http {

/** @short Reasonably unique, but free-form string for identifying client connections */
std::string peer_from_request(const nghttp2::asio_http2::server::request& req)
{
    std::string peer = boost::lexical_cast<std::string>(req.remote_endpoint());
    if (auto forwarded = req.header().find("forwarded"); forwarded != req.header().end()) {
        peer += '(' + forwarded->second.value + ')';
    }
    return peer;
}

/** @short Returns a vector of media types (strings) parsed from accept header sorted by preference (quality) descending (excluding q=0 entries)
 *
 * @return empty vector for invalid header values, otherwise sorted vector by quality descending (also order is kept for same quality entries)
 */
std::vector<std::string> parseAcceptHeader(const std::string& headerValue)
{
    namespace x3 = boost::spirit::x3;

    const auto identifier = x3::rule<class identifier, std::string>{"identifier"} = x3::alpha >> *(x3::alnum | x3::char_("-") | x3::char_("."));
    const auto decimal = x3::rule<class decimal, std::string>{"decimal"} = -x3::char_("-") >> +x3::digit >> -(x3::char_(".") >> *x3::digit);
    const auto quotedString = x3::rule<class quotedString, std::string>{"quotedString"} = '"' >> *('\\' >> x3::char_ | ~x3::char_('"')) >> '"';

    // RFC 7231, section 3.1.1.1
    const auto param = x3::rule<class param, std::pair<std::string, std::string>>{"param"} = ((x3::string("q") | x3::string("Q")) >> "=" >> decimal) | ((identifier - (x3::char_("q") | "Q")) >> "=" >> (quotedString | identifier));
    const auto parameters = x3::rule<class parameters, std::vector<std::pair<std::string, std::string>>>{"parameters"} = *(x3::omit[*x3::space >> ";" >> *x3::space] >> param);

    const auto mimeWildcard = x3::rule<class mimeWildcard, std::string>{"mimeWildcard"} = x3::string("*");
    const auto mimeSubtype = x3::rule<class mimeSubtype, std::string>{"mimeSubtype"} = identifier >> -(x3::char_("+") >> identifier) | mimeWildcard;
    const auto mimeType = x3::rule<class mimeType, std::string>{"mimeType"} = identifier;
    const auto mime = x3::rule<class mimeType, Mime>{"mime"} = mimeWildcard >> "/" >> mimeWildcard | mimeType >> "/" >> (mimeSubtype | mimeWildcard);

    // RFC 9110, section 12.5.1
    const auto oneItem = x3::rule<class oneItem, MediaType>{"oneItem"} = mime >> parameters;
    const auto acceptGrammar = x3::rule<class grammar, std::vector<MediaType>>{"acceptGrammar"} = x3::omit[*x3::space] >> oneItem % (*x3::space >> "," >> *x3::space);

    auto iter = std::begin(headerValue);
    auto end = std::end(headerValue);

    std::vector<MediaType> mediaTypes;
    if (!x3::parse(iter, end, acceptGrammar >> x3::omit[*x3::space] >> x3::eoi, mediaTypes)) {
        return {};
    }

    // The type, subtype, and parameter name tokens are case-insensitive. (RFC 7231, sec 3.1.1.1)
    for (auto& e : mediaTypes) {
        std::transform(e.mime.mediaType.begin(), e.mime.mediaType.end(), e.mime.mediaType.begin(), ::tolower);
        std::transform(e.mime.mediaSubtype.begin(), e.mime.mediaSubtype.end(), e.mime.mediaSubtype.begin(), ::tolower);
        for (auto& param : e.parameters) {
            std::transform(param.first.begin(), param.first.end(), param.first.begin(), ::tolower);

            if (param.first == "q") {
                try {
                    std::stod(param.second); // if q parameter contains an number not representable as a double then the header is invalid
                } catch (const std::out_of_range&) {
                    return {};
                }
            }
        }
    }

    // remove q=0 entries
    mediaTypes.erase(std::remove_if(mediaTypes.begin(), mediaTypes.end(), [](const auto& e) { return e.qValue() == 0; }), mediaTypes.end());

    // sort by quality and preference descending; if two types share the same quality then prefer the most specific one. If they share same quality and "specificity" then prefer the first so we can have stable tests
    std::stable_sort(mediaTypes.begin(), mediaTypes.end(), [](const auto& a, const auto& b) {
        if (a.qValue() != b.qValue()) {
            return a.qValue() > b.qValue();
        }

        if (a.mime.mediaType == "*") {
            return false;
        } else if (b.mime.mediaType == "*") {
            return true;
        } else if (a.mime.mediaSubtype == "*") {
            return false;
        } else if (b.mime.mediaSubtype == "*") {
            return true;
        }

        return false;
    });

    std::vector<std::string> res;
    res.reserve(mediaTypes.size());
    std::transform(mediaTypes.begin(), mediaTypes.end(), std::back_inserter(res), [](const auto& e) {
        return e.mime.mediaType + "/" + e.mime.mediaSubtype;
    });
    return res;
}

ProtoAndHost parseForwardedHeader(const std::string& headerValue)
{
    namespace x3 = boost::spirit::x3;

    const auto token = x3::rule<class token, std::string>{"token"} = +(x3::alnum | x3::char_("-") | x3::char_("."));
    const auto quotedString = x3::rule<class quotedString, std::string>{"quotedString"} = '"' >> *('\\' >> x3::char_ | ~x3::char_('"')) >> '"';
    const auto value = x3::rule<class value, std::string>{"value"} = token | quotedString;

    const auto forwardedPair = x3::rule<class forwardedPair, std::pair<std::string, std::string>>{"forwardedPair"} = token >> '=' >> value;
    const auto forwarded = x3::rule<class singleForwarded, std::map<std::string, std::string>>{"forwarded"} = forwardedPair % ";";
    const auto forwardedList = x3::rule<class grammar, std::vector<std::map<std::string, std::string>>>{"forwardedList"} = forwarded % (x3::omit[*x3::space] >> ',' >> x3::omit[*x3::space]);

    std::string str;
    std::transform(headerValue.begin(), headerValue.end(), std::back_inserter(str), [](const auto& c) { return std::tolower(c); });

    std::vector<std::map<std::string, std::string>> forwardedKvPairs;
    if (!x3::parse(std::begin(headerValue), std::end(headerValue), forwardedList >> x3::eoi, forwardedKvPairs)) {
        return {};
    }

    ProtoAndHost res;

    // The first entry should win; this is the one that contains what user requested. (Other proxies in the chain append to this value or add another Forwarded header entry)
    if (auto it = forwardedKvPairs.front().find("proto"); it != forwardedKvPairs.front().end()) {
        res.proto = it->second;
    }
    if (auto it = forwardedKvPairs.front().find("host"); it != forwardedKvPairs.front().end()) {
        res.host = it->second;
    }

    return res;
}
}
