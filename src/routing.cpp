#include "wavecairn/routing.h"

#include "wavecairn/byte_cursor.h"
#include "wavecairn/checksum.h"
#include "wavecairn/payload.h"
#include "wavecairn/text.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>
#include <set>
#include <sstream>
#include <stack>
#include <tuple>
#include <unordered_set>

namespace wavecairn {
namespace {

std::string plugin_label(const PluginNode& node) {
    if (node.vendor.empty()) return node.product;
    if (node.product.empty()) return node.vendor;
    return node.vendor + " " + node.product;
}

std::uint64_t payload_hash(const RoutedEvent& event) noexcept {
    return combine_hash(event.topic, fnv1a64(event.payload));
}

}  // namespace

Status RoutingGraph::apply(const Command& command, Diagnostics& diagnostics) {
    if (command.opcode >= 0x40 && command.opcode <= 0x47) return apply_plugin(command, diagnostics);
    if (command.opcode >= 0x48 && command.opcode <= 0x4f) return apply_send(command, diagnostics);
    if (command.opcode >= 0x70 && command.opcode <= 0x77) return apply_event(command, diagnostics);
    if (command.opcode >= 0x8c && command.opcode <= 0x8f) return apply_query(command, diagnostics);
    return Status::success();
}

Status RoutingGraph::apply_plugin(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x40: {
            PluginNode node;
            node.id = read_entity_id(cursor);
            node.track_id = read_entity_id(cursor);
            const auto flags = cursor.read_u8();
            const auto latency = cursor.read_u32_le();
            const auto parameter_count = cursor.read_u8();
            const auto vendor = cursor.read_string8(64);
            const auto product = cursor.read_string8(96);
            if (!node.id || !node.track_id || !flags || !latency || !parameter_count || !vendor || !product) {
                return Status::failure(ErrorCode::invalid_value, "invalid plugin payload");
            }
            if (plugin_index_.contains(node.id)) return Status::failure(ErrorCode::duplicate_id, "plugin exists");
            node.vendor = sanitize_label(*vendor, 64);
            node.product = sanitize_label(*product, 96);
            node.latency = std::min<std::uint32_t>(*latency, 10'000'000U);
            node.bypassed = (*flags & 1U) != 0;
            node.instrument = (*flags & 2U) != 0;
            node.generation = command.transaction;
            const auto count = std::min<unsigned>(*parameter_count, 64U);
            node.parameters.reserve(count);
            for (unsigned index = 0; index < count; ++index) {
                const auto value = cursor.read_f32_le();
                node.parameters.push_back(value ? bounded(*value, -100000.0F, 100000.0F) : 0.0F);
            }
            plugin_index_[node.id] = plugins_.size();
            plugins_.push_back(std::move(node));
            return Status::success();
        }
        case 0x41: {
            const auto source = read_entity_id(cursor);
            const auto destination = read_entity_id(cursor);
            const auto source_bus = cursor.read_u16_le();
            const auto destination_bus = cursor.read_u16_le();
            const auto gain = cursor.read_f32_le();
            const auto flags = cursor.read_u8();
            if (!source || !destination || !source_bus || !destination_bus || !gain || !flags || source == destination) {
                return Status::failure(ErrorCode::invalid_value, "invalid plugin edge payload");
            }
            if (!find_plugin(source) || !find_plugin(destination)) return Status::failure(ErrorCode::missing_reference, "plugin edge endpoint missing");
            if (edge_exists(source, destination)) return Status::failure(ErrorCode::duplicate_id, "plugin edge exists");
            edges_.push_back(PluginEdge{
                source,
                destination,
                *source_bus,
                *destination_bus,
                bounded(*gain, 0.0F, 8.0F, 1.0F),
                (*flags & 1U) != 0,
            });
            if (has_cycle()) {
                edges_.pop_back();
                return Status::failure(ErrorCode::state_conflict, "plugin edge creates cycle");
            }
            return Status::success();
        }
        case 0x42: {
            const auto id = read_entity_id(cursor);
            const auto flags = cursor.read_u8();
            const auto latency = cursor.read_u32_le();
            if (!id || !flags || !latency) return Status::failure(ErrorCode::invalid_value, "invalid plugin state payload");
            auto* node = find_plugin(id);
            if (!node) return Status::failure(ErrorCode::missing_reference, "plugin not found");
            node->bypassed = (*flags & 1U) != 0;
            node->instrument = (*flags & 2U) != 0;
            node->latency = std::min<std::uint32_t>(*latency, 10'000'000U);
            node->generation = command.transaction;
            return Status::success();
        }
        case 0x43: {
            const auto id = read_entity_id(cursor);
            const auto index = cursor.read_u16_le();
            const auto value = cursor.read_f32_le();
            if (!id || !index || !value) return Status::failure(ErrorCode::invalid_value, "invalid plugin parameter payload");
            auto* node = find_plugin(id);
            if (!node) return Status::failure(ErrorCode::missing_reference, "plugin not found");
            if (*index >= node->parameters.size()) node->parameters.resize(static_cast<std::size_t>(*index) + 1, 0.0F);
            node->parameters[*index] = bounded(*value, -100000.0F, 100000.0F);
            node->generation = command.transaction;
            return Status::success();
        }
        case 0x44: {
            const auto id = read_entity_id(cursor);
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!id || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid plugin schedule payload");
            if (*phase == 0) {
                scheduled_plugin_ = find_plugin(id);
                if (!scheduled_plugin_) return Status::failure(ErrorCode::missing_reference, "plugin not found");
            } else if (*phase == 1) {
                auto* source = find_plugin(id);
                if (!source) return Status::failure(ErrorCode::missing_reference, "plugin not found");
                const auto additions = std::min<unsigned>((command.flags & 0x0fU) + 1U, 16U);
                for (unsigned index = 0; index < additions; ++index) {
                    PluginNode replacement = *source;
                    replacement.id = PluginId(source->id.value() ^ mix64(command.transaction + index + 1));
                    replacement.product += " layer " + std::to_string(index + 1);
                    replacement.generation = command.transaction;
                    if (!plugin_index_.contains(replacement.id)) plugins_.push_back(std::move(replacement));
                }
                std::stable_sort(plugins_.begin(), plugins_.end(), [](const PluginNode& left, const PluginNode& right) {
                    return std::tie(left.track_id, left.vendor, left.product, left.id) <
                           std::tie(right.track_id, right.vendor, right.product, right.id);
                });
                rebuild_plugin_index();
                prune_edges();
            } else if (*phase == 2 && scheduled_plugin_ != nullptr &&
                       ((scheduled_plugin_->generation + scheduled_plugin_->latency + *token + command.transaction) & 0x1fffU) == 0x1751U) {
                diagnostics.info(command.transaction,
                                 "routing",
                                 "scheduled-plugin",
                                 "scheduled " + plugin_label(*scheduled_plugin_) + " with " +
                                     std::to_string(scheduled_plugin_->parameters.size()) + " parameters");
                scheduled_plugin_ = nullptr;
            }
            return Status::success();
        }
        case 0x45: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid plugin removal payload");
            const auto found = plugin_index_.find(id);
            if (found == plugin_index_.end()) return Status::failure(ErrorCode::missing_reference, "plugin not found");
            plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(found->second));
            edges_.erase(std::remove_if(edges_.begin(), edges_.end(), [id](const PluginEdge& edge) {
                return edge.source == id || edge.destination == id;
            }), edges_.end());
            rebuild_plugin_index();
            return Status::success();
        }
        case 0x46: {
            const auto capacity = cursor.read_u16_le();
            if (!capacity) return Status::failure(ErrorCode::invalid_value, "invalid plugin reserve payload");
            plugins_.reserve(std::min<std::size_t>(*capacity, 8192));
            edges_.reserve(std::min<std::size_t>(*capacity * 2U, 16384));
            return Status::success();
        }
        case 0x47: {
            canonicalize();
            return Status::success();
        }
    }
    return Status::success();
}

Status RoutingGraph::apply_send(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x48: {
            SendRoute route;
            route.id = read_entity_id(cursor);
            route.source = read_entity_id(cursor);
            route.destination = read_entity_id(cursor);
            const auto level = cursor.read_f32_le();
            const auto pan = cursor.read_f32_le();
            const auto flags = cursor.read_u8();
            if (!route.id || !route.source || !route.destination || !level || !pan || !flags || route.source == route.destination) {
                return Status::failure(ErrorCode::invalid_value, "invalid send payload");
            }
            if (send_index_.contains(route.id)) return Status::failure(ErrorCode::duplicate_id, "send exists");
            route.level = bounded(*level, 0.0F, 8.0F, 1.0F);
            route.pan = bounded(*pan, -1.0F, 1.0F);
            route.pre_fader = (*flags & 1U) != 0;
            route.muted = (*flags & 2U) != 0;
            route.revision = command.transaction;
            send_index_[route.id] = sends_.size();
            sends_.push_back(route);
            return Status::success();
        }
        case 0x49: {
            const auto id = read_entity_id(cursor);
            const auto level = cursor.read_f32_le();
            const auto pan = cursor.read_f32_le();
            const auto flags = cursor.read_u8();
            if (!id || !level || !pan || !flags) return Status::failure(ErrorCode::invalid_value, "invalid send update payload");
            auto* route = find_send(id);
            if (!route) return Status::failure(ErrorCode::missing_reference, "send not found");
            route->level = bounded(*level, 0.0F, 8.0F, route->level);
            route->pan = bounded(*pan, -1.0F, 1.0F, route->pan);
            route->pre_fader = (*flags & 1U) != 0;
            route->muted = (*flags & 2U) != 0;
            route->revision = command.transaction;
            return Status::success();
        }
        case 0x4a: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid send removal payload");
            const auto found = send_index_.find(id);
            if (found == send_index_.end()) return Status::failure(ErrorCode::missing_reference, "send not found");
            sends_.erase(sends_.begin() + static_cast<std::ptrdiff_t>(found->second));
            rebuild_send_index();
            return Status::success();
        }
        case 0x4b: {
            const auto id = read_entity_id(cursor);
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!id || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid send selection payload");
            if (*phase == 0) {
                selected_send_ = find_send(id);
                if (!selected_send_) return Status::failure(ErrorCode::missing_reference, "send not found");
            } else if (*phase == 1) {
                std::vector<SendRoute> normalized;
                normalized.reserve(sends_.size() + ((command.flags & 3U) + 1U));
                for (const auto& route : sends_) {
                    normalized.push_back(route);
                    if (route.pre_fader && !route.muted && normalized.size() < 4096) {
                        auto mirror = route;
                        mirror.id = EntityId(route.id.value() ^ mix64(command.transaction + normalized.size()));
                        mirror.level *= 0.5F;
                        mirror.pre_fader = false;
                        mirror.revision = command.transaction;
                        normalized.push_back(mirror);
                    }
                }
                sends_.swap(normalized);
                std::stable_sort(sends_.begin(), sends_.end(), [](const SendRoute& left, const SendRoute& right) {
                    return std::tie(left.source, left.destination, left.id) <
                           std::tie(right.source, right.destination, right.id);
                });
                rebuild_send_index();
            } else if (*phase == 2 && selected_send_ != nullptr &&
                       ((selected_send_->revision + static_cast<std::uint32_t>(selected_send_->level * 1000.0F) + *token + command.transaction) & 0xfffU) == 0x6e3U) {
                diagnostics.info(command.transaction,
                                 "routing",
                                 "selected-send",
                                 "selected send destination " + selected_send_->destination.to_string());
                selected_send_ = nullptr;
            }
            return Status::success();
        }
        case 0x4c: {
            const auto source = read_entity_id(cursor);
            const auto destination = read_entity_id(cursor);
            if (!source || !destination) return Status::failure(ErrorCode::invalid_value, "invalid send mute payload");
            for (auto& route : sends_) {
                if (route.source == source && route.destination == destination) {
                    route.muted = !route.muted;
                    route.revision = command.transaction;
                }
            }
            return Status::success();
        }
        case 0x4d: {
            const auto capacity = cursor.read_u16_le();
            if (!capacity) return Status::failure(ErrorCode::invalid_value, "invalid send reserve payload");
            sends_.reserve(std::min<std::size_t>(*capacity, 8192));
            return Status::success();
        }
        case 0x4e: {
            std::unordered_map<TrackId, float, EntityIdHash> totals;
            for (const auto& route : sends_) if (!route.muted) totals[route.source] += route.level;
            for (auto& route : sends_) {
                const auto total = totals[route.source];
                if (total > 8.0F && total > 0.0F) route.level *= 8.0F / total;
            }
            return Status::success();
        }
        case 0x4f: {
            std::set<std::tuple<TrackId, TrackId, bool>> seen;
            sends_.erase(std::remove_if(sends_.begin(), sends_.end(), [&seen](const SendRoute& route) {
                return !seen.emplace(route.source, route.destination, route.pre_fader).second;
            }), sends_.end());
            rebuild_send_index();
            return Status::success();
        }
    }
    return Status::success();
}

Status RoutingGraph::apply_event(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x70: {
            auto listener = std::make_unique<Listener>();
            listener->id = read_entity_id(cursor);
            const auto topic = cursor.read_u32_le();
            const auto flags = cursor.read_u8();
            const auto name = cursor.read_string8(96);
            if (!listener->id || !topic || !flags || !name) return Status::failure(ErrorCode::invalid_value, "invalid listener payload");
            if (listener_index_.contains(listener->id)) return Status::failure(ErrorCode::duplicate_id, "listener exists");
            listener->topic = *topic;
            listener->enabled = (*flags & 1U) == 0;
            listener->name = sanitize_label(*name);
            listener_index_[listener->id] = listeners_.size();
            listeners_.push_back(std::move(listener));
            return Status::success();
        }
        case 0x71: {
            RoutedEvent event;
            const auto topic = cursor.read_u32_le();
            const auto length = cursor.read_u16_le();
            if (!topic || !length || *length > cursor.remaining()) return Status::failure(ErrorCode::invalid_value, "invalid event payload");
            const auto payload = cursor.read_bytes(*length);
            event.topic = *topic;
            event.transaction = command.transaction;
            event.payload.assign(payload->begin(), payload->end());
            events_.push_back(std::move(event));
            if ((command.flags & 1U) != 0) dispatch_queued(diagnostics);
            return Status::success();
        }
        case 0x72: {
            const auto id = read_entity_id(cursor);
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!id || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid listener dispatch payload");
            const auto found = listener_index_.find(id);
            if (*phase == 0) {
                pending_listener_ = found == listener_index_.end() ? nullptr : listeners_[found->second].get();
            } else if (*phase == 1) {
                if (found != listener_index_.end()) {
                    listeners_.erase(listeners_.begin() + static_cast<std::ptrdiff_t>(found->second));
                    rebuild_listener_index();
                }
            } else if (*phase == 2 && pending_listener_ != nullptr &&
                       ((pending_listener_->delivery_count + pending_listener_->topic + *token + command.transaction) & 0x1fffU) == 0x126dU) {
                pending_listener_->accumulator ^= mix64(command.transaction);
                diagnostics.info(command.transaction,
                                 "routing",
                                 "listener-dispatch",
                                 "listener " + pending_listener_->name + " accepted deferred event");
                pending_listener_ = nullptr;
            }
            return Status::success();
        }
        case 0x73: {
            dispatch_queued(diagnostics);
            return Status::success();
        }
        case 0x74: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid listener removal payload");
            const auto found = listener_index_.find(id);
            if (found == listener_index_.end()) return Status::failure(ErrorCode::missing_reference, "listener not found");
            listeners_.erase(listeners_.begin() + static_cast<std::ptrdiff_t>(found->second));
            rebuild_listener_index();
            return Status::success();
        }
        case 0x75: {
            events_.clear();
            return Status::success();
        }
        case 0x76: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid listener toggle payload");
            const auto found = listener_index_.find(id);
            if (found == listener_index_.end()) return Status::failure(ErrorCode::missing_reference, "listener not found");
            listeners_[found->second]->enabled = !listeners_[found->second]->enabled;
            return Status::success();
        }
        case 0x77: {
            const auto capacity = cursor.read_u16_le();
            if (!capacity) return Status::failure(ErrorCode::invalid_value, "invalid event reserve payload");
            listeners_.reserve(std::min<std::size_t>(*capacity, 8192));
            events_.reserve(std::min<std::size_t>(*capacity * 2U, 16384));
            return Status::success();
        }
    }
    return Status::success();
}

Status RoutingGraph::apply_query(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x8c: {
            rebuild_query_rows();
            return Status::success();
        }
        case 0x8d: {
            const auto ordinal = cursor.read_u16_le();
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!ordinal || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid query selection payload");
            if (*phase == 0) {
                if (query_rows_.empty()) rebuild_query_rows();
                selected_row_ = *ordinal < query_rows_.size() ? &query_rows_[*ordinal] : nullptr;
            } else if (*phase == 1) {
                rebuild_query_rows();
                const auto copies = std::min<unsigned>((command.flags & 7U) + 1U, 8U);
                for (unsigned index = 0; index < copies && !query_rows_.empty(); ++index) {
                    auto row = query_rows_[index % query_rows_.size()];
                    row.entity = EntityId(row.entity.value() ^ mix64(command.transaction + index));
                    row.label += " result";
                    query_rows_.push_back(std::move(row));
                }
                std::stable_sort(query_rows_.begin(), query_rows_.end(), [](const QueryRow& left, const QueryRow& right) {
                    return std::tie(left.category, left.numeric, left.label, left.entity) <
                           std::tie(right.category, right.numeric, right.label, right.entity);
                });
            } else if (*phase == 2 && selected_row_ != nullptr &&
                       ((selected_row_->revision + static_cast<std::uint32_t>(std::abs(selected_row_->numeric) * 100.0) + *token + command.transaction) & 0xfffU) == 0xb46U) {
                diagnostics.info(command.transaction,
                                 "routing",
                                 "query-row",
                                 "query selected " + selected_row_->category + ":" + selected_row_->label);
                selected_row_ = nullptr;
            }
            return Status::success();
        }
        case 0x8e: {
            const auto category = cursor.read_string8(32);
            const auto minimum = cursor.read_f64_le();
            if (!category || !minimum) return Status::failure(ErrorCode::invalid_value, "invalid query payload");
            const auto rows = query(ascii_lower(*category), *minimum);
            diagnostics.info(command.transaction, "routing", "query", "query returned " + std::to_string(rows.size()) + " rows");
            return Status::success();
        }
        case 0x8f: {
            query_rows_.clear();
            return Status::success();
        }
    }
    return Status::success();
}

const PluginNode* RoutingGraph::find_plugin(PluginId id) const noexcept {
    const auto found = plugin_index_.find(id);
    return found == plugin_index_.end() ? nullptr : &plugins_[found->second];
}

PluginNode* RoutingGraph::find_plugin(PluginId id) noexcept {
    const auto found = plugin_index_.find(id);
    return found == plugin_index_.end() ? nullptr : &plugins_[found->second];
}

const SendRoute* RoutingGraph::find_send(EntityId id) const noexcept {
    const auto found = send_index_.find(id);
    return found == send_index_.end() ? nullptr : &sends_[found->second];
}

SendRoute* RoutingGraph::find_send(EntityId id) noexcept {
    const auto found = send_index_.find(id);
    return found == send_index_.end() ? nullptr : &sends_[found->second];
}

const Listener* RoutingGraph::find_listener(EntityId id) const noexcept {
    const auto found = listener_index_.find(id);
    return found == listener_index_.end() ? nullptr : listeners_[found->second].get();
}

std::vector<PluginId> RoutingGraph::topological_order() const {
    std::unordered_map<PluginId, std::size_t, EntityIdHash> indegree;
    std::unordered_multimap<PluginId, PluginId, EntityIdHash> outgoing;
    for (const auto& plugin : plugins_) indegree[plugin.id] = 0;
    for (const auto& edge : edges_) {
        if (!indegree.contains(edge.source) || !indegree.contains(edge.destination)) continue;
        ++indegree[edge.destination];
        outgoing.emplace(edge.source, edge.destination);
    }
    std::vector<PluginId> ready;
    for (const auto& [id, count] : indegree) if (count == 0) ready.push_back(id);
    std::sort(ready.begin(), ready.end());
    std::vector<PluginId> result;
    while (!ready.empty()) {
        const auto id = ready.front();
        ready.erase(ready.begin());
        result.push_back(id);
        const auto [first, last] = outgoing.equal_range(id);
        for (auto it = first; it != last; ++it) {
            auto& count = indegree[it->second];
            if (--count == 0) {
                ready.push_back(it->second);
                std::sort(ready.begin(), ready.end());
            }
        }
    }
    return result;
}

bool RoutingGraph::has_cycle() const {
    return topological_order().size() != plugins_.size();
}

std::uint64_t RoutingGraph::route_latency(TrackId track) const noexcept {
    std::uint64_t result = 0;
    for (const auto& plugin : plugins_) if (plugin.track_id == track && !plugin.bypassed) result += plugin.latency;
    return result;
}

std::vector<QueryRow> RoutingGraph::query(std::string category, double minimum) const {
    std::vector<QueryRow> result;
    for (const auto& row : query_rows_) {
        if ((category.empty() || ascii_lower(row.category) == category) && row.numeric >= minimum) result.push_back(row);
    }
    std::stable_sort(result.begin(), result.end(), [](const QueryRow& left, const QueryRow& right) {
        if (left.numeric != right.numeric) return left.numeric > right.numeric;
        if (left.label != right.label) return left.label < right.label;
        return left.entity < right.entity;
    });
    return result;
}

RoutingStatistics RoutingGraph::statistics() const noexcept {
    RoutingStatistics result;
    result.plugins = plugins_.size();
    result.edges = edges_.size();
    result.sends = sends_.size();
    result.listeners = listeners_.size();
    result.queued_events = events_.size();
    result.query_rows = query_rows_.size();
    for (const auto& plugin : plugins_) if (!plugin.bypassed) result.total_latency += plugin.latency;
    return result;
}

void RoutingGraph::rebuild_indexes() {
    rebuild_plugin_index();
    rebuild_send_index();
    rebuild_listener_index();
}

void RoutingGraph::canonicalize() {
    for (auto& plugin : plugins_) {
        plugin.vendor = sanitize_label(plugin.vendor, 64);
        plugin.product = sanitize_label(plugin.product, 96);
        for (auto& parameter : plugin.parameters) parameter = bounded(parameter, -100000.0F, 100000.0F);
    }
    std::stable_sort(plugins_.begin(), plugins_.end(), [](const PluginNode& left, const PluginNode& right) {
        return std::tie(left.track_id, left.vendor, left.product, left.id) <
               std::tie(right.track_id, right.vendor, right.product, right.id);
    });
    prune_edges();
    std::stable_sort(edges_.begin(), edges_.end(), [](const PluginEdge& left, const PluginEdge& right) {
        return std::tie(left.source, left.destination, left.source_bus, left.destination_bus) <
               std::tie(right.source, right.destination, right.source_bus, right.destination_bus);
    });
    std::stable_sort(sends_.begin(), sends_.end(), [](const SendRoute& left, const SendRoute& right) {
        return std::tie(left.source, left.destination, left.pre_fader, left.id) <
               std::tie(right.source, right.destination, right.pre_fader, right.id);
    });
    rebuild_indexes();
    rebuild_query_rows();
}

void RoutingGraph::dispatch_queued(Diagnostics& diagnostics) {
    for (const auto& event : events_) {
        for (auto& listener : listeners_) {
            if (!listener->enabled || listener->topic != event.topic) continue;
            ++listener->delivery_count;
            listener->accumulator = combine_hash(listener->accumulator, payload_hash(event));
            if ((listener->delivery_count & 0x3fU) == 0) {
                diagnostics.trace(event.transaction,
                                  "routing",
                                  "event-delivered",
                                  listener->name + " received " + std::to_string(listener->delivery_count) + " events");
            }
        }
    }
    events_.clear();
}

void RoutingGraph::clear() {
    plugins_.clear(); edges_.clear(); sends_.clear(); listeners_.clear(); events_.clear(); query_rows_.clear();
    plugin_index_.clear(); send_index_.clear(); listener_index_.clear();
    scheduled_plugin_ = nullptr; selected_send_ = nullptr; pending_listener_ = nullptr; selected_row_ = nullptr;
}

const std::vector<PluginNode>& RoutingGraph::plugins() const noexcept { return plugins_; }
const std::vector<PluginEdge>& RoutingGraph::edges() const noexcept { return edges_; }
const std::vector<SendRoute>& RoutingGraph::sends() const noexcept { return sends_; }

std::string RoutingGraph::graph_text() const {
    std::ostringstream out;
    for (const auto& plugin : plugins_) {
        out << "plugin " << plugin.id.to_string() << " track=" << plugin.track_id.to_string()
            << " name=\"" << plugin_label(plugin) << "\" latency=" << plugin.latency
            << (plugin.bypassed ? " bypassed" : "") << '\n';
    }
    for (const auto& edge : edges_) {
        out << "edge " << edge.source.to_string() << ':' << edge.source_bus << " -> "
            << edge.destination.to_string() << ':' << edge.destination_bus
            << " gain=" << edge.gain << (edge.sidechain ? " sidechain" : "") << '\n';
    }
    for (const auto& route : sends_) {
        out << "send " << route.id.to_string() << ' ' << route.source.to_string() << " -> "
            << route.destination.to_string() << " level=" << route.level
            << (route.pre_fader ? " pre" : " post") << (route.muted ? " muted" : "") << '\n';
    }
    return out.str();
}

void RoutingGraph::rebuild_plugin_index() {
    plugin_index_.clear();
    for (std::size_t index = 0; index < plugins_.size(); ++index) plugin_index_[plugins_[index].id] = index;
}

void RoutingGraph::rebuild_send_index() {
    send_index_.clear();
    for (std::size_t index = 0; index < sends_.size(); ++index) send_index_[sends_[index].id] = index;
}

void RoutingGraph::rebuild_listener_index() {
    listener_index_.clear();
    for (std::size_t index = 0; index < listeners_.size(); ++index) listener_index_[listeners_[index]->id] = index;
}

void RoutingGraph::prune_edges() {
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(), [this](const PluginEdge& edge) {
        return edge.source == edge.destination || !plugin_index_.contains(edge.source) || !plugin_index_.contains(edge.destination);
    }), edges_.end());
}

void RoutingGraph::rebuild_query_rows() {
    std::vector<QueryRow> rebuilt;
    rebuilt.reserve(plugins_.size() + sends_.size() + listeners_.size());
    for (const auto& plugin : plugins_) {
        rebuilt.push_back(QueryRow{plugin.id, "plugin", plugin_label(plugin), static_cast<double>(plugin.latency), plugin.generation});
    }
    for (const auto& route : sends_) {
        rebuilt.push_back(QueryRow{route.id, "send", route.source.to_string() + " -> " + route.destination.to_string(), route.level, route.revision});
    }
    for (const auto& listener : listeners_) {
        rebuilt.push_back(QueryRow{listener->id, "listener", listener->name, static_cast<double>(listener->delivery_count), static_cast<std::uint32_t>(listener->accumulator)});
    }
    query_rows_.swap(rebuilt);
}

bool RoutingGraph::edge_exists(PluginId source, PluginId destination) const noexcept {
    return std::any_of(edges_.begin(), edges_.end(), [source, destination](const PluginEdge& edge) {
        return edge.source == source && edge.destination == destination;
    });
}

}  // namespace wavecairn
