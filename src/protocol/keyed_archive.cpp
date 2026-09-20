#include "ioscpp/protocol/keyed_archive.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace ioscpp::protocol
{
namespace
{
Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}
/// The `$version` every `NSKeyedArchive` skeleton carries.
constexpr std::int64_t kVersion = 100000;
/// The `$archiver` every `NSKeyedArchive` skeleton names.
constexpr std::string_view kArchiver = "NSKeyedArchiver";
/// The first slot of `$objects`, the placeholder a `Uid` of 0 refers to.
constexpr std::string_view kNull = "$null";
/// The nesting bound, so a hostile archive's cyclic `Uid` cannot loop forever.
constexpr std::size_t kMaxDepth = 64;
/// The class definition an archived array or dictionary points its `$class` at.
Plist class_dictionary(std::string_view name, std::string_view superclass)
{
    Plist::Dictionary dictionary;
    dictionary.emplace("$classes", Plist::array({Plist(std::string(name)), Plist(std::string(superclass))}));
    dictionary.emplace("$classname", Plist(std::string(name)));
    return Plist::dictionary(std::move(dictionary));
}
/// Appends `value` to `objects` and returns its slot.
std::size_t append_object(const Plist &value, Plist::Array &objects)
{
    if (value.type() == PlistType::Array)
    {
        const Plist::Array &items = *value.array();
        const std::size_t slot = objects.size();
        objects.emplace_back(Plist());
        const std::size_t class_slot = objects.size();
        objects.push_back(class_dictionary("NSArray", "NSObject"));
        Plist::Array refs;
        refs.reserve(items.size());
        for (const Plist &item : items)
        {
            refs.push_back(Plist::uid(append_object(item, objects)));
        }
        Plist::Dictionary dictionary;
        dictionary.emplace("$class", Plist::uid(class_slot));
        dictionary.emplace("NS.objects", Plist::array(std::move(refs)));
        objects[slot] = Plist::dictionary(std::move(dictionary));
        return slot;
    }
    if (value.type() == PlistType::Dictionary)
    {
        const Plist::Dictionary &items = *value.dictionary();
        const std::size_t slot = objects.size();
        objects.emplace_back(Plist());
        const std::size_t class_slot = objects.size();
        objects.push_back(class_dictionary("NSDictionary", "NSObject"));
        Plist::Array keys;
        keys.reserve(items.size());
        for (const auto &entry : items)
        {
            keys.push_back(Plist::uid(append_object(Plist(entry.first), objects)));
        }
        Plist::Array refs;
        refs.reserve(items.size());
        for (const auto &entry : items)
        {
            refs.push_back(Plist::uid(append_object(entry.second, objects)));
        }
        Plist::Dictionary dictionary;
        dictionary.emplace("$class", Plist::uid(class_slot));
        dictionary.emplace("NS.keys", Plist::array(std::move(keys)));
        dictionary.emplace("NS.objects", Plist::array(std::move(refs)));
        objects[slot] = Plist::dictionary(std::move(dictionary));
        return slot;
    }
    objects.push_back(value);
    return objects.size() - 1;
}
Result<Plist> resolve(const Plist &value, const Plist::Array &objects, std::size_t depth);
/// Resolves a `Uid` to its slot, an array to its resolved entries, and an
/// `NSArray`/`NSDictionary` class dictionary to a plain array or dictionary. A
/// custom class object is returned unchanged.
Result<Plist> resolve_object(const Plist &value, const Plist::Array &objects, std::size_t depth)
{
    if (value.type() == PlistType::Uid)
    {
        const std::uint64_t index = value.uid().value();
        if (index >= objects.size())
        {
            return tl::unexpected(protocol_error("an NSKeyedArchive Uid is out of range"));
        }
        return resolve(objects[static_cast<std::size_t>(index)], objects, depth + 1);
    }
    if (value.type() == PlistType::Array)
    {
        const Plist::Array &refs = *value.array();
        Plist::Array items;
        items.reserve(refs.size());
        for (const Plist &ref : refs)
        {
            auto item = resolve(ref, objects, depth + 1);
            if (!item)
            {
                return tl::unexpected(item.error());
            }
            items.push_back(std::move(*item));
        }
        return Plist::array(std::move(items));
    }
    const Plist *class_ref = value.find("$class");
    if (value.type() != PlistType::Dictionary || class_ref == nullptr)
    {
        return value;
    }
    const std::optional<std::uint64_t> class_index = class_ref->uid();
    if (!class_index.has_value() || *class_index >= objects.size())
    {
        return tl::unexpected(protocol_error("an NSKeyedArchive $class is not a Uid"));
    }
    const Plist *class_name = objects[static_cast<std::size_t>(*class_index)].find("$classname");
    const std::string name = class_name == nullptr ? std::string() : class_name->string_or();
    if (name == "NSArray" || name == "NSMutableArray")
    {
        const Plist *refs = value.find("NS.objects");
        if (refs == nullptr || refs->array() == nullptr)
        {
            return tl::unexpected(protocol_error("an NSKeyedArchive array has no NS.objects"));
        }
        Plist::Array items;
        items.reserve(refs->array()->size());
        for (const Plist &ref : *refs->array())
        {
            auto item = resolve(ref, objects, depth + 1);
            if (!item)
            {
                return tl::unexpected(item.error());
            }
            items.push_back(std::move(*item));
        }
        return Plist::array(std::move(items));
    }
    if (name == "NSDictionary" || name == "NSMutableDictionary")
    {
        const Plist *keys = value.find("NS.keys");
        const Plist *refs = value.find("NS.objects");
        if (keys == nullptr || keys->array() == nullptr || refs == nullptr || refs->array() == nullptr)
        {
            return tl::unexpected(protocol_error("an NSKeyedArchive dictionary has no keys"));
        }
        if (keys->array()->size() != refs->array()->size())
        {
            return tl::unexpected(protocol_error("an NSKeyedArchive dictionary has mismatched keys"));
        }
        Plist::Dictionary dictionary;
        for (std::size_t i = 0; i < keys->array()->size(); ++i)
        {
            auto key = resolve((*keys->array())[i], objects, depth + 1);
            if (!key)
            {
                return tl::unexpected(key.error());
            }
            auto item = resolve((*refs->array())[i], objects, depth + 1);
            if (!item)
            {
                return tl::unexpected(item.error());
            }
            dictionary.emplace(key->string_or(), std::move(*item));
        }
        return Plist::dictionary(std::move(dictionary));
    }
    // A custom class object is left as it is: the services here do not decode one.
    return value;
}
Result<Plist> resolve(const Plist &value, const Plist::Array &objects, std::size_t depth)
{
    if (depth > kMaxDepth)
    {
        return tl::unexpected(protocol_error("an NSKeyedArchive is nested too deeply"));
    }
    return resolve_object(value, objects, depth);
}
} // namespace
std::vector<std::byte> KeyedArchive::archive(const Plist &value)
{
    Plist::Array objects;
    objects.reserve(2);
    objects.push_back(Plist(std::string(kNull)));
    const std::size_t root = append_object(value, objects);
    Plist::Dictionary top;
    top.emplace("root", Plist::uid(root));
    Plist::Dictionary skeleton;
    skeleton.emplace("$version", Plist(kVersion));
    skeleton.emplace("$archiver", Plist(std::string(kArchiver)));
    skeleton.emplace("$top", Plist::dictionary(std::move(top)));
    skeleton.emplace("$objects", Plist::array(std::move(objects)));
    return Plist::dictionary(std::move(skeleton)).to_binary();
}
Result<Plist> KeyedArchive::unarchive(std::span<const std::byte> bytes)
{
    auto parsed = Plist::parse_binary(bytes);
    if (!parsed)
    {
        return tl::unexpected(parsed.error());
    }
    const Plist *archiver = parsed->find("$archiver");
    if (parsed->dictionary() == nullptr || archiver == nullptr || archiver->string_or() != kArchiver)
    {
        return tl::unexpected(protocol_error("the bytes are not an NSKeyedArchive"));
    }
    const Plist *version = parsed->find("$version");
    if (version == nullptr || version->integer().value_or(0) != kVersion)
    {
        return tl::unexpected(protocol_error("the NSKeyedArchive has an unknown version"));
    }
    const Plist *objects = parsed->find("$objects");
    if (objects == nullptr || objects->array() == nullptr)
    {
        return tl::unexpected(protocol_error("the NSKeyedArchive has no $objects"));
    }
    const Plist *top = parsed->find("$top");
    const Plist *root = top == nullptr ? nullptr : top->find("root");
    if (root == nullptr || root->uid() == std::nullopt)
    {
        return tl::unexpected(protocol_error("the NSKeyedArchive has no $top root"));
    }
    return resolve(*root, *objects->array(), 0);
}
} // namespace ioscpp::protocol
