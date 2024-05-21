#pragma once

#include <libyang-cpp/Enum.hpp>
#include <nghttp2/asio_http2_server.h>
#include <optional>
#include <string>

namespace rousette::restconf {

enum class MimeTypeWildcards { ALLOWED,
                               FORBIDDEN };

struct DataFormat {
    std::optional<libyang::DataFormat> request; // request encoding is not always needed (e.g. GET)
    libyang::DataFormat response;
};

DataFormat chooseDataEncoding(const nghttp2::asio_http2::header_map& headers);
std::string asMimeType(libyang::DataFormat dataFormat);
bool mimeMatch(const std::string& providedMime, const std::string& applicationMime, MimeTypeWildcards wildcards);
std::optional<libyang::DataFormat> dataTypeFromMimeType(const std::string& mime, MimeTypeWildcards wildcards);
}
