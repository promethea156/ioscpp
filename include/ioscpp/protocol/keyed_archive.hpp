#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/protocol/plist.hpp"

namespace ioscpp::protocol
{

/**
 * @brief An `NSKeyedArchive`, how a `DTX` argument carries an object.
 *
 * A `DTX` method call does not put its arguments on the wire directly: each one is
 * an `NSKeyedArchive`, a binary property list with a fixed skeleton. The skeleton
 * is the `$version` (`100000`), the `$archiver` (`NSKeyedArchiver`), the `$top`
 * whose `root` is a `Uid` into `$objects`, and the `$objects` table whose first
 * slot is `$null`.
 *
 * An object that is a string, number, `Data`, or date is stored inline in
 * `$objects`; an array or dictionary is stored as a dictionary with a `$class`
 * `Uid` and the `NS.keys`/`NS.objects` `Uid` lists, and its class is another
 * `$objects` entry. A `Uid` refers to a slot by index.
 *
 * @warning An archive is not a plist a service exchanges: it is the wrapper a `DTX`
 * argument rides in, so it is built and read here, not by the caller.
 */
class IOSCPP_API KeyedArchive
{
public:
    /// Archives `value` into the archive's binary plist bytes.
    static std::vector<std::byte> archive(const Plist &value);

    /// Unarchives the `root` object of an archive's binary plist bytes.
    static Result<Plist> unarchive(std::span<const std::byte> bytes);
};

} // namespace ioscpp::protocol
