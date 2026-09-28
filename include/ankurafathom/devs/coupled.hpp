#pragma once

#include "ankurafathom/devs/simulator.hpp"

#include <cstddef>
#include <cstdint>
#include <compare>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace ankurafathom::devs {

enum class ComponentKind { boundary, atomic, coupled };
struct Component {
    ComponentKind kind;
    std::size_t id;
};

namespace detail {
// Topology has no model state. Copying it stages a structural edit while the
// running simulator retains ownership of all atomics and their calendars.
struct CouplingTopology {
    enum class PortKind { atomic_input, atomic_output, group_input, group_output };
    struct PortNode {
        PortKind kind;
        std::size_t id;
        std::uint32_t port;
        auto operator<=>(const PortNode&) const = default;
    };
    struct Record { std::size_t parent; bool active = true; };
    std::vector<Record> groups{{0}};
    std::vector<Record> atomics;
    std::map<PortNode, std::vector<PortNode>> links;

    void require_group(Component group) const {
        if (group.kind != ComponentKind::coupled || group.id >= groups.size() || !groups[group.id].active)
            throw std::out_of_range("parent is not an active coupled component");
    }
    void require_child(Component parent, Component child) const {
        if (child.kind == ComponentKind::boundary) return;
        if (child.kind == ComponentKind::atomic && child.id < atomics.size() &&
            atomics[child.id].active && atomics[child.id].parent == parent.id) return;
        if (child.kind == ComponentKind::coupled && child.id > 0 && child.id < groups.size() &&
            groups[child.id].active && groups[child.id].parent == parent.id) return;
        throw std::out_of_range("endpoint is not an active direct child of the parent");
    }
    Component add_coupled(Component parent) {
        require_group(parent);
        groups.push_back({parent.id});
        return {ComponentKind::coupled, groups.size() - 1};
    }
    Component add_atomic(Component parent) {
        require_group(parent);
        atomics.push_back({parent.id});
        return {ComponentKind::atomic, atomics.size() - 1};
    }
    std::pair<PortNode, PortNode> endpoints(Component parent, Component source, std::uint32_t out,
                                           Component destination, std::uint32_t in) const {
        require_group(parent); require_child(parent, source); require_child(parent, destination);
        const PortNode from = source.kind == ComponentKind::boundary
            ? PortNode{PortKind::group_input, parent.id, out} : source.kind == ComponentKind::atomic
            ? PortNode{PortKind::atomic_output, source.id, out} : PortNode{PortKind::group_output, source.id, out};
        const PortNode to = destination.kind == ComponentKind::boundary
            ? PortNode{PortKind::group_output, parent.id, in} : destination.kind == ComponentKind::atomic
            ? PortNode{PortKind::atomic_input, destination.id, in} : PortNode{PortKind::group_input, destination.id, in};
        return {from, to};
    }
    void connect(Component parent, Component source, std::uint32_t out, Component destination, std::uint32_t in) {
        const auto [from, to] = endpoints(parent, source, out, destination, in);
        links[from].push_back(to);
    }
    void disconnect(Component parent, Component source, std::uint32_t out, Component destination, std::uint32_t in) {
        const auto [from, to] = endpoints(parent, source, out, destination, in);
        const auto found = links.find(from);
        if (found == links.end()) throw std::out_of_range("coupling does not exist");
        auto& targets = found->second;
        const auto target = std::find(targets.begin(), targets.end(), to);
        if (target == targets.end()) throw std::out_of_range("coupling does not exist");
        targets.erase(target); // Exactly one declared path.
        if (targets.empty()) links.erase(found);
    }
    std::vector<std::size_t> remove(Component component) {
        if (component.kind == ComponentKind::coupled) {
            require_group(component);
            if (component.id == 0) throw std::invalid_argument("cannot remove the root hierarchy");
            groups[component.id].active = false;
            // Parents always precede children, so a single ascending pass suffices.
            for (std::size_t id = 1; id < groups.size(); ++id)
                if (!groups[groups[id].parent].active) groups[id].active = false;
        } else if (component.kind != ComponentKind::atomic || component.id >= atomics.size() ||
                   !atomics[component.id].active) {
            throw std::out_of_range("component is not active");
        }
        std::vector<std::size_t> removed;
        for (std::size_t id = 0; id < atomics.size(); ++id) {
            if (atomics[id].active && (!groups[atomics[id].parent].active ||
                (component.kind == ComponentKind::atomic && id == component.id))) {
                atomics[id].active = false;
                removed.push_back(id);
            }
        }
        auto active = [&](PortNode node) {
            return (node.kind == PortKind::atomic_input || node.kind == PortKind::atomic_output)
                ? atomics[node.id].active : groups[node.id].active;
        };
        for (auto it = links.begin(); it != links.end();) {
            if (!active(it->first)) { it = links.erase(it); continue; }
            std::erase_if(it->second, [&](PortNode node) { return !active(node); });
            if (it->second.empty()) it = links.erase(it);
            else ++it;
        }
        return removed;
    }
    void trace(PortNode current, std::set<PortNode>& path, std::vector<PortNode>& targets) const {
        if (!path.insert(current).second) throw std::logic_error("coupled boundary contains a routing cycle");
        if (current.kind == PortKind::atomic_input || (current.kind == PortKind::group_output && current.id == 0)) {
            targets.push_back(current);
        } else if (const auto found = links.find(current); found != links.end()) {
            for (const auto next : found->second) trace(next, path, targets);
        }
        path.erase(current);
    }
    Routing flatten() const {
        Routing result;
        for (const auto& [from, unused] : links) {
            (void)unused;
            std::set<PortNode> path;
            std::vector<PortNode> targets;
            // Validate even boundary cycles disconnected from any current source.
            trace(from, path, targets);
            for (const auto target : targets) {
                if (from.kind == PortKind::atomic_output) {
                    if (target.kind == PortKind::atomic_input)
                        result.internal.push_back({from.id, from.port, target.id, target.port});
                    else result.outputs[{from.id, from.port}].push_back(target.port);
                } else if (from.kind == PortKind::group_input && from.id == 0) {
                    if (target.kind == PortKind::atomic_input)
                        result.inputs[from.port].push_back({target.id, target.port});
                    else result.passthrough[from.port].push_back(target.port);
                }
            }
        }
        return result;
    }
    void swap(CouplingTopology& other) noexcept {
        groups.swap(other.groups); atomics.swap(other.atomics); links.swap(other.links);
    }
};
} // namespace detail

template <typename Message> class CoupledBuilder;

template <typename Message>
struct CompiledCoupling {
    // Stepping, observation and direct atomic injection remain available here.
    // Structural mutation is owned by this hierarchy after compilation.
    Simulator<Message> simulator;

    static constexpr Component boundary() noexcept { return {ComponentKind::boundary, 0}; }
    static constexpr Component root() noexcept { return {ComponentKind::coupled, 0}; }

    void inject(double time, std::uint32_t root_port, const Message& value) {
        simulator.inject_root(time, root_port, value);
    }
    std::vector<PortValue<Message>> boundary_outputs(const StepResult<Message>& step) const {
        return step.boundary_emissions;
    }
    Component add_coupled(Component parent) {
        require_editable();
        auto next = topology_;
        const auto id = next.add_coupled(parent);
        commit(next);
        return id;
    }
    Component add_atomic(Component parent, std::unique_ptr<Atomic<Message>> model) {
        require_editable();
        if (!model) throw std::invalid_argument("null atomic model");
        auto next = topology_;
        const auto id = next.add_atomic(parent);
        commit(next, {}, std::move(model));
        return id;
    }
    void connect(Component parent, Component source, std::uint32_t out, Component destination, std::uint32_t in) {
        require_editable();
        auto next = topology_;
        next.connect(parent, source, out, destination, in);
        commit(next);
    }
    void disconnect(Component parent, Component source, std::uint32_t out, Component destination, std::uint32_t in) {
        require_editable();
        auto next = topology_;
        next.disconnect(parent, source, out, destination, in);
        commit(next);
    }
    void remove(Component component) {
        require_editable();
        auto next = topology_;
        const auto removed = next.remove(component);
        commit(next, removed);
    }
private:
    friend class CoupledBuilder<Message>;
    detail::CouplingTopology topology_;
    bool editable_ = false;

    void require_editable() const {
        simulator.require_idle();
        if (!editable_) throw std::logic_error("hierarchy was not compiled from a builder");
    }
    void commit(detail::CouplingTopology& next, const std::vector<std::size_t>& removed = {},
                std::unique_ptr<Atomic<Message>> added = {}) {
        auto routing = next.flatten();
        simulator.apply_structure(std::move(routing), removed, std::move(added));
        topology_.swap(next);
    }
    void finish(detail::CouplingTopology topology) {
        auto routing = topology.flatten();
        simulator.replace_routing(std::move(routing));
        topology_.swap(topology);
        simulator.managed_structure_ = true;
        editable_ = true;
    }
};

// Boundary endpoints are relative to the named parent; every other endpoint
// must be its direct child. Compilation transfers ownership to an editable graph.
template <typename Message>
class CoupledBuilder {
public:
    static constexpr Component boundary() noexcept { return CompiledCoupling<Message>::boundary(); }
    static constexpr Component root() noexcept { return CompiledCoupling<Message>::root(); }
    Component add_coupled(Component parent) {
        require_open();
        return topology_.add_coupled(parent);
    }
    Component add_atomic(Component parent, std::unique_ptr<Atomic<Message>> model) {
        require_open();
        if (!model) throw std::invalid_argument("null atomic model");
        topology_.require_group(parent);
        models_.reserve(models_.size() + 1);
        const auto id = topology_.add_atomic(parent);
        models_.push_back(std::move(model));
        return id;
    }
    void connect(Component parent, Component source, std::uint32_t out, Component destination, std::uint32_t in) {
        require_open();
        topology_.connect(parent, source, out, destination, in);
    }
    CompiledCoupling<Message> compile() {
        require_open();
        (void)topology_.flatten(); // Diagnose invalid topology before moving models.
        CompiledCoupling<Message> result;
        for (auto& model : models_) result.simulator.add(std::move(model));
        result.finish(std::move(topology_));
        compiled_ = true;
        return result;
    }
private:
    void require_open() const {
        if (compiled_) throw std::logic_error("coupled builder was already compiled");
    }
    detail::CouplingTopology topology_;
    std::vector<std::unique_ptr<Atomic<Message>>> models_;
    bool compiled_ = false;
};
} // namespace ankurafathom::devs
