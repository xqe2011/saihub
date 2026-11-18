#pragma once

#include <string>
#include <string_view>

namespace saihub {

std::string gzip_compress(std::string_view input);

std::string landing_page_html(std::string_view origin, std::string_view digest);
std::string pairing_page_html(std::string_view digest, std::string_view redirect_uri, std::string_view state);
std::string_view oauth_echo_page_html();
std::string_view admin_page_html();

std::string landing_page_gzip(std::string_view origin, std::string_view digest);
std::string pairing_page_gzip(std::string_view digest, std::string_view redirect_uri, std::string_view state);
std::string_view oauth_echo_page_gzip();
std::string_view admin_page_gzip();

} // namespace saihub
