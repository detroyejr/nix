#include "nix/util/url.hh"
#include "nix/util/url-parts.hh"
#include "nix/util/util.hh"
#include "nix/util/split.hh"
#include "nix/util/canon-path.hh"

namespace nix {

std::regex refRegex(refRegexS, std::regex::ECMAScript);
std::regex badGitRefRegex(badGitRefRegexS, std::regex::ECMAScript);
std::regex revRegex(revRegexS, std::regex::ECMAScript);

ParsedURL parseURL(const std::string & url)
{
    static std::regex uriRegex(
        "((" + schemeNameRegex + "):" + "(?:(?://(" + authorityRegex + ")(" + absPathRegex + "))|(/?" + pathRegex
            + ")))" + "(?:\\?(" + queryRegex + "))?" + "(?:#(" + fragmentRegex + "))?",
        std::regex::ECMAScript);

    std::smatch match;

    if (std::regex_match(url, match, uriRegex)) {
        std::string scheme = match[2];
        auto authority = match[3].matched ? std::optional<std::string>(match[3]) : std::nullopt;
        std::string path = match[4].matched ? match[4] : match[5];
        auto & query = match[6];
        auto & fragment = match[7];

        auto transportIsFile = parseUrlScheme(scheme).transport == "file";

        if (authority && *authority != "" && transportIsFile)
            throw BadURL("file:// URL '%s' has unexpected authority '%s'", url, *authority);

        if (transportIsFile && path.empty())
            path = "/";

        return ParsedURL{
            .scheme = scheme,
            .authority = authority,
            .path = percentDecode(path),
            .query = decodeQuery(query),
            .fragment = percentDecode(std::string(fragment))};
    }

    else
        throw BadURL("'%s' is not a valid URL", url);
}

std::string percentDecode(std::string_view in)
{
    std::string decoded;
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '%') {
            if (i + 2 >= in.size())
                throw BadURL("invalid URI parameter '%s'", in);
            try {
                decoded += std::stoul(std::string(in, i + 1, 2), 0, 16);
                i += 3;
            } catch (...) {
                throw BadURL("invalid URI parameter '%s'", in);
            }
        } else
            decoded += in[i++];
    }
    return decoded;
}

StringMap decodeQuery(const std::string & query)
{
    StringMap result;

    for (const auto & s : tokenizeString<Strings>(query, "&")) {
        auto e = s.find('=');
        if (e == std::string::npos) {
            warn("dubious URI query '%s' is missing equal sign '%s', ignoring", s, "=");
            continue;
        }

        result.emplace(s.substr(0, e), percentDecode(std::string_view(s).substr(e + 1)));
    }

    return result;
}

const static std::string allowedInQuery = ":@/?";
const static std::string allowedInPath = ":@/";

std::string percentEncode(std::string_view s, std::string_view keep)
{
    std::string res;
    for (auto & c : s)
        // unreserved + keep
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~", c)
            || keep.find(c) != std::string::npos)
            res += c;
        else
            res += fmt("%%%02X", c & 0xFF);
    return res;
}

std::string encodeQuery(const StringMap & ss)
{
    std::string res;
    bool first = true;
    for (auto & [name, value] : ss) {
        if (!first)
            res += '&';
        first = false;
        res += percentEncode(name, allowedInQuery);
        res += '=';
        res += percentEncode(value, allowedInQuery);
    }
    return res;
}

std::string ParsedURL::to_string() const
{
    return scheme + ":" + (authority ? "//" + *authority : "") + percentEncode(path, allowedInPath)
           + (query.empty() ? "" : "?" + encodeQuery(query)) + (fragment.empty() ? "" : "#" + percentEncode(fragment));
}

std::ostream & operator<<(std::ostream & os, const ParsedURL & url)
{
    os << url.to_string();
    return os;
}

bool ParsedURL::operator==(const ParsedURL & other) const noexcept
{
    return scheme == other.scheme && authority == other.authority && path == other.path && query == other.query
           && fragment == other.fragment;
}

ParsedURL ParsedURL::canonicalise()
{
    ParsedURL res(*this);
    res.path = CanonPath(res.path).abs();
    return res;
}

/**
 * Parse a URL scheme of the form '(applicationScheme\+)?transportScheme'
 * into a tuple '(applicationScheme, transportScheme)'
 *
 * > parseUrlScheme("http") == ParsedUrlScheme{ {}, "http"}
 * > parseUrlScheme("tarball+http") == ParsedUrlScheme{ {"tarball"}, "http"}
 */
ParsedUrlScheme parseUrlScheme(std::string_view scheme)
{
    auto application = splitPrefixTo(scheme, '+');
    auto transport = scheme;
    return ParsedUrlScheme{
        .application = application,
        .transport = transport,
    };
}

std::string fixGitURL(const std::string & url)
{
    std::regex scpRegex("([^/]*)@(.*):(.*)");
    if (!hasPrefix(url, "/") && std::regex_match(url, scpRegex))
        return std::regex_replace(url, scpRegex, "ssh://$1@$2/$3");
    if (hasPrefix(url, "file:"))
        return url;
    if (url.find("://") == std::string::npos) {
        return (ParsedURL{.scheme = "file", .authority = "", .path = url}).to_string();
    }
    return url;
}

// https://www.rfc-editor.org/rfc/rfc3986#section-3.1
bool isValidSchemeName(std::string_view s)
{
    static std::regex regex(schemeNameRegex, std::regex::ECMAScript);

    return std::regex_match(s.begin(), s.end(), regex, std::regex_constants::match_default);
}

} // namespace nix
