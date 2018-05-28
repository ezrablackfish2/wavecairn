#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace wavecairn {

class EntityId {
public:
    constexpr EntityId() noexcept = default;
    explicit constexpr EntityId(std::uint64_t value) noexcept : value_(value) {}

    constexpr std::uint64_t value() const noexcept { return value_; }
    constexpr bool valid() const noexcept { return value_ != 0; }
    explicit constexpr operator bool() const noexcept { return valid(); }

    std::string to_string() const;
    static std::optional<EntityId> parse(std::string_view text);
    static EntityId from_name(std::string_view category, std::string_view name);

    friend constexpr bool operator==(EntityId, EntityId) noexcept = default;
    friend constexpr auto operator<=>(EntityId, EntityId) noexcept = default;

private:
    std::uint64_t value_{0};
};

struct EntityIdHash {
    std::size_t operator()(EntityId id) const noexcept;
};

using SessionId = EntityId;
using TrackId = EntityId;
using ClipId = EntityId;
using MediaId = EntityId;
using PluginId = EntityId;
using MarkerId = EntityId;
using LaneId = EntityId;
using TakeId = EntityId;

}  // namespace wavecairn

template <>
struct std::hash<wavecairn::EntityId> {
    std::size_t operator()(wavecairn::EntityId id) const noexcept {
        return wavecairn::EntityIdHash{}(id);
    }
};
