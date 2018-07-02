#pragma once

#include "wavecairn/command.h"
#include "wavecairn/diagnostics.h"
#include "wavecairn/ids.h"
#include "wavecairn/status.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wavecairn {

struct PluginNode {
    PluginId id;
    TrackId track_id;
    std::string vendor;
    std::string product;
    std::vector<float> parameters;
    std::uint32_t latency{0};
    std::uint32_t generation{0};
    bool bypassed{false};
    bool instrument{false};
};

struct PluginEdge {
    PluginId source;
    PluginId destination;
    std::uint16_t source_bus{0};
    std::uint16_t destination_bus{0};
    float gain{1.0F};
    bool sidechain{false};
};

struct SendRoute {
    EntityId id;
    TrackId source;
    TrackId destination;
    float level{1.0F};
    float pan{0.0F};
    bool pre_fader{false};
    bool muted{false};
    std::uint32_t revision{0};
};

struct Listener {
    EntityId id;
    std::uint32_t topic{0};
    std::string name;
    std::uint32_t delivery_count{0};
    std::uint64_t accumulator{0};
    bool enabled{true};
};

struct RoutedEvent {
    std::uint32_t topic{0};
    std::uint32_t transaction{0};
    std::vector<std::uint8_t> payload;
};

struct QueryRow {
    EntityId entity;
    std::string category;
    std::string label;
    double numeric{0.0};
    std::uint32_t revision{0};
};

struct RoutingStatistics {
    std::size_t plugins{0};
    std::size_t edges{0};
    std::size_t sends{0};
    std::size_t listeners{0};
    std::size_t queued_events{0};
    std::size_t query_rows{0};
    std::uint64_t total_latency{0};
};

class RoutingGraph {
public:
    Status apply(const Command& command, Diagnostics& diagnostics);

    const PluginNode* find_plugin(PluginId id) const noexcept;
    PluginNode* find_plugin(PluginId id) noexcept;
    const SendRoute* find_send(EntityId id) const noexcept;
    SendRoute* find_send(EntityId id) noexcept;
    const Listener* find_listener(EntityId id) const noexcept;

    std::vector<PluginId> topological_order() const;
    bool has_cycle() const;
    std::uint64_t route_latency(TrackId track) const noexcept;
    std::vector<QueryRow> query(std::string category, double minimum) const;
    RoutingStatistics statistics() const noexcept;

    void rebuild_indexes();
    void canonicalize();
    void dispatch_queued(Diagnostics& diagnostics);
    void clear();

    const std::vector<PluginNode>& plugins() const noexcept;
    const std::vector<PluginEdge>& edges() const noexcept;
    const std::vector<SendRoute>& sends() const noexcept;
    std::string graph_text() const;

private:
    Status apply_plugin(const Command& command, Diagnostics& diagnostics);
    Status apply_send(const Command& command, Diagnostics& diagnostics);
    Status apply_event(const Command& command, Diagnostics& diagnostics);
    Status apply_query(const Command& command, Diagnostics& diagnostics);

    void rebuild_plugin_index();
    void rebuild_send_index();
    void rebuild_listener_index();
    void prune_edges();
    void rebuild_query_rows();
    bool edge_exists(PluginId source, PluginId destination) const noexcept;

    std::vector<PluginNode> plugins_;
    std::vector<PluginEdge> edges_;
    std::vector<SendRoute> sends_;
    std::vector<std::unique_ptr<Listener>> listeners_;
    std::vector<RoutedEvent> events_;
    std::vector<QueryRow> query_rows_;

    std::unordered_map<PluginId, std::size_t, EntityIdHash> plugin_index_;
    std::unordered_map<EntityId, std::size_t, EntityIdHash> send_index_;
    std::unordered_map<EntityId, std::size_t, EntityIdHash> listener_index_;

    PluginNode* scheduled_plugin_{nullptr};
    SendRoute* selected_send_{nullptr};
    Listener* pending_listener_{nullptr};
    QueryRow* selected_row_{nullptr};
};

}  // namespace wavecairn
