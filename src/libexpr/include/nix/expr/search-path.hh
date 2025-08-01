#pragma once
///@file

#include <optional>

#include "nix/util/types.hh"
#include "nix/util/comparator.hh"

namespace nix {

/**
 * A "search path" is a list of ways look for something, used with
 * `builtins.findFile` and `< >` lookup expressions.
 */
struct LookupPath
{
    /**
     * A single element of a `LookupPath`.
     *
     * Each element is tried in succession when looking up a path. The first
     * element to completely match wins.
     */
    struct Elem;

    /**
     * The first part of a `LookupPath::Elem` pair.
     *
     * Called a "prefix" because it takes the form of a prefix of a file
     * path (first `n` path components). When looking up a path, to use
     * a `LookupPath::Elem`, its `Prefix` must match the path.
     */
    struct Prefix;

    /**
     * The second part of a `LookupPath::Elem` pair.
     *
     * It is either a path or a URL (with certain restrictions / extra
     * structure).
     *
     * If the prefix of the path we are looking up matches, we then
     * check if the rest of the path points to something that exists
     * within the directory denoted by this. If so, the
     * `LookupPath::Elem` as a whole matches, and that *something* being
     * pointed to by the rest of the path we are looking up is the
     * result.
     */
    struct Path;

    /**
     * The list of search path elements. Each one is checked for a path
     * when looking up. (The actual lookup entry point is in `EvalState`
     * not in this class.)
     */
    std::list<LookupPath::Elem> elements;

    /**
     * Parse a string into a `LookupPath`
     */
    static LookupPath parse(const Strings & rawElems);
};

struct LookupPath::Prefix
{
    /**
     * Underlying string
     *
     * @todo Should we normalize this when constructing a `LookupPath::Prefix`?
     */
    std::string s;

    GENERATE_CMP(LookupPath::Prefix, me->s);

    /**
     * If the path possibly matches this search path element, return the
     * suffix that we should look for inside the resolved value of the
     * element
     * Note the double optionality in the name. While we might have a matching prefix, the suffix may not exist.
     */
    std::optional<std::string_view> suffixIfPotentialMatch(std::string_view path) const;
};

struct LookupPath::Path
{
    /**
     * The location of a search path item, as a path or URL.
     *
     * @todo Maybe change this to `std::variant<SourcePath, URL>`.
     */
    std::string s;

    GENERATE_CMP(LookupPath::Path, me->s);
};

struct LookupPath::Elem
{

    Prefix prefix;
    Path path;

    GENERATE_CMP(LookupPath::Elem, me->prefix, me->path);

    /**
     * Parse a string into a `LookupPath::Elem`
     */
    static LookupPath::Elem parse(std::string_view rawElem);
};

} // namespace nix
