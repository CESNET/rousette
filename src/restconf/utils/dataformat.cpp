#include <boost/algorithm/string.hpp>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <vector>
#include "http/utils.hpp"
#include "restconf/Exceptions.h"
#include "restconf/utils/dataformat.h"

namespace rousette::restconf {

std::string asMimeType(libyang::DataFormat dataFormat)
{
    switch (dataFormat) {
    case libyang::DataFormat::JSON:
        return "application/yang-data+json";
    case libyang::DataFormat::XML:
        return "application/yang-data+xml";
    default:
        throw std::logic_error("Invalid data format");
    }
}

bool mimeMatch(const std::string& providedMime, const std::string& applicationMime, MimeTypeWildcards wildcards)
{
    std::vector<std::string> tokensMime;
    std::vector<std::string> tokensApplicationMime;

    boost::split(tokensMime, providedMime, boost::is_any_of("/"));
    boost::split(tokensApplicationMime, applicationMime, boost::is_any_of("/"));

    if (wildcards == MimeTypeWildcards::ALLOWED) {
        if (tokensMime[0] == "*") {
            return true;
        }
        if (tokensMime[0] == tokensApplicationMime[0] && tokensMime[1] == "*") {
            return true;
        }
    }

    return tokensMime[0] == tokensApplicationMime[0] && tokensMime[1] == tokensApplicationMime[1];
}

std::optional<libyang::DataFormat> dataTypeFromMimeType(const std::string& mime, MimeTypeWildcards wildcards)
{
    if (mimeMatch(mime, asMimeType(libyang::DataFormat::JSON), wildcards) || mimeMatch(mime, "application/yang-patch+json", wildcards)) {
        return libyang::DataFormat::JSON;
    } else if (mimeMatch(mime, asMimeType(libyang::DataFormat::XML), wildcards) || mimeMatch(mime, "application/yang-patch+xml", wildcards)) {
        return libyang::DataFormat::XML;
    }

    return std::nullopt;
}

/** @brief Chooses request and response data format w.r.t. accept/content-type http headers.
 * @throws ErrorResponse if invalid accept/content-type header found
 */
DataFormat chooseDataEncoding(const nghttp2::asio_http2::header_map& headers)
{
    std::vector<std::string> acceptTypes;
    std::optional<std::string> contentType;

    if (auto value = http::getHeaderValue(headers, "accept")) {
        acceptTypes = http::parseAcceptHeader(*value);
    }
    if (auto value = http::getHeaderValue(headers, "content-type")) {
        auto contentTypes = http::parseAcceptHeader(*value); // content type doesn't have the same syntax as accept but content-type is a singleton object similar to those in accept header (RFC 9110) so this should be fine

        if (contentTypes.size() > 1) {
            spdlog::trace("Multiple content-type entries found");
        }
        if (!contentTypes.empty()) {
            contentType = contentTypes.back(); // RFC 9110: Recipients often attempt to handle this error by using the last syntactically valid member of the list
        }
    }

    std::optional<libyang::DataFormat> resAccept;
    std::optional<libyang::DataFormat> resContentType;

    if (!acceptTypes.empty()) {
        for (const auto& mediaType : acceptTypes) {
            if (auto type = dataTypeFromMimeType(mediaType, MimeTypeWildcards::ALLOWED)) {
                resAccept = *type;
                break;
            }
        }

        if (!resAccept) {
            throw ErrorResponse(406, "application", "operation-not-supported", "No requested format supported");
        }
    }

    // If it (the types in the accept header) is not specified, the request input encoding format SHOULD be used, or the server MAY choose any supported content encoding format
    if (contentType) {
        if (auto type = dataTypeFromMimeType(*contentType, MimeTypeWildcards::FORBIDDEN)) {
            resContentType = *type;
        } else {
            // If the server does not support the requested input encoding for a request, then it MUST return an error response with a "415 Unsupported Media Type" status-line.
            throw ErrorResponse(415, "application", "operation-not-supported", "content-type format value not supported");
        }
    }

    if (!resAccept) {
        resAccept = resContentType;
    }

    // If there was no request input, then the default output encoding is XML or JSON, depending on server preference.
    return {resContentType, resAccept ? *resAccept : libyang::DataFormat::JSON};
}
}
