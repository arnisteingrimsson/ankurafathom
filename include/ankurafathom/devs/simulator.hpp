#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ankurafathom::devs {

template <typename Message> struct CompiledCoupling;

struct Routing {
    struct Connection {
        std::size_t source;
        std::uint32_t source_port;
        std::size_t destination;
        std::uint32_t destination_port;
    };
    std::vector<Connection> internal;
    std::map<std::uint32_t, std::vector<std::pair<std::size_t, std::uint32_t>>> inputs;
    std::map<std::pair<std::size_t, std::uint32_t>, std::vector<std::uint32_t>> outputs;
    std::map<std::uint32_t, std::vector<std::uint32_t>> passthrough;
};

template <typename Message>
struct PortValue {
    std::uint32_t port;
    Message value;
};

template <typename Message>
struct Input {
    std::size_t source;
    std::uint32_t port;
    Message value;
};

template <typename Message>
class Atomic {
public:
    virtual ~Atomic() = default;
    // Opt in to whole-step rollback. The clone must own all mutable state that
    // a transition may change; external side effects cannot be restored.
    virtual std::unique_ptr<Atomic<Message>> clone() const { return {}; }
    virtual double time_advance() const = 0;
    // Calendar owners may supply their exact absolute deadline to avoid rounding
    // it through last_transition + (deadline - last_transition).
    virtual std::optional<double> next_event_time() const { return std::nullopt; }
    virtual std::vector<PortValue<Message>> output() const = 0;
    virtual void internal_transition() = 0;
    virtual void external_transition(double elapsed, const std::vector<Input<Message>>& bag) = 0;
    // Timestamp-aware atomics can validate or preserve the exact event clock.
    // Existing elapsed-time atomics retain their original transition contract.
    virtual void external_transition_at(double /* time */, double elapsed,
                                        const std::vector<Input<Message>>& bag) {
        external_transition(elapsed, bag);
    }
    virtual void confluent_transition(const std::vector<Input<Message>>& bag) = 0;
};

template <typename Message>
struct Emission {
    std::size_t source;
    std::uint32_t port;
    Message value;
};

template <typename Message>
struct Injection {
    std::size_t destination;
    std::uint32_t port;
    Message value;
};

template <typename Message>
struct StepResult {
    double time;
    std::vector<std::size_t> imminent;
    std::vector<std::size_t> transitioned;
    std::vector<Emission<Message>> emissions;
    std::vector<Injection<Message>> injections;
    std::vector<PortValue<Message>> boundary_injections;
    std::vector<PortValue<Message>> boundary_emissions;
};

// One typed payload per coupled model. Different payload families can be joined
// later by an explicit adapter, never an unchecked void* cast.
template <typename Message>
class Simulator {
public:
    static constexpr std::size_t external_source = std::numeric_limits<std::size_t>::max();

    std::size_t add(std::unique_ptr<Atomic<Message>> model) {
        require_unmanaged();
        return add_impl(std::move(model));
    }

    void connect(std::size_t source, std::uint32_t source_port,
                 std::size_t destination, std::uint32_t destination_port) {
        require_unmanaged();
        if (source >= nodes_.size() || destination >= nodes_.size() ||
            !nodes_[source].active || !nodes_[destination].active)
            throw std::out_of_range("coupling component ID does not exist");
        couplings_[source].push_back(Coupling{source_port, destination, destination_port});
        auto& links = couplings_[source];
        std::stable_sort(links.begin(), links.end(), [](const Coupling& a, const Coupling& b) {
            if (a.destination != b.destination) return a.destination < b.destination;
            return a.destination_port < b.destination_port;
        });
    }

    // Remove one declared coupling path, preserving any duplicate paths.
    // Like connect/remove, this operation is only legal between event steps.
    void disconnect(std::size_t source, std::uint32_t source_port,
                    std::size_t destination, std::uint32_t destination_port) {
        require_unmanaged();
        if (source >= nodes_.size() || destination >= nodes_.size() ||
            !nodes_[source].active || !nodes_[destination].active)
            throw std::out_of_range("coupling component ID does not exist");
        auto& links = couplings_[source];
        const auto found = std::find_if(links.begin(), links.end(), [&](const Coupling& link) {
            return link.source_port == source_port && link.destination == destination &&
                   link.destination_port == destination_port;
        });
        if (found == links.end()) throw std::out_of_range("coupling does not exist");
        links.erase(found);
    }

    void remove(std::size_t id) {
        require_unmanaged();
        if (id >= nodes_.size() || !nodes_[id].active)
            throw std::out_of_range("component ID is not active");
        nodes_[id].active = false;
        ++nodes_[id].generation;
        couplings_[id].clear();
        for (auto& links : couplings_) {
            links.erase(std::remove_if(links.begin(), links.end(), [id](const Coupling& link) {
                return link.destination == id;
            }), links.end());
        }
        for (auto& [port, targets] : root_inputs_) {
            (void)port;
            std::erase_if(targets, [id](const auto& target) { return target.first == id; });
        }
        std::erase_if(root_outputs_, [id](const auto& item) { return item.first.first == id; });
    }

    // Install a complete flat routing table without changing any atomic clock.
    void replace_routing(Routing routing) {
        require_unmanaged();
        apply_structure(std::move(routing), {}, {});
    }

    void inject_root(double time, std::uint32_t port, Message value) {
        require_idle();
        if (!std::isfinite(time) || time < now_)
            throw std::invalid_argument("external event time must be finite and not in the past");
        const auto input = root_inputs_.find(port);
        const auto output = passthrough_.find(port);
        if ((input == root_inputs_.end() || input->second.empty()) &&
            (output == passthrough_.end() || output->second.empty()))
            throw std::out_of_range("root input port is not connected");
        if (injection_sequence_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("external event sequence exhausted");
        external_events_.push(ExternalEvent{time, external_source, port, std::move(value), injection_sequence_, true});
        ++injection_sequence_;
    }

    void inject(double time, std::size_t destination, std::uint32_t port, Message value) {
        if (in_step_) throw std::logic_error("cannot inject during an event step");
        if (!std::isfinite(time) || time < now_)
            throw std::invalid_argument("external event time must be finite and not in the past");
        if (destination >= nodes_.size() || !nodes_[destination].active)
            throw std::out_of_range("external event destination is not active");
        if (injection_sequence_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("external event sequence exhausted");
        external_events_.push(ExternalEvent{time, destination, port, std::move(value), injection_sequence_, false});
        ++injection_sequence_;
    }

    double now() const noexcept { return now_; }

    const Atomic<Message>& model(std::size_t id) const {
        if (id >= nodes_.size() || !nodes_[id].active)
            throw std::out_of_range("component ID is not active");
        return *nodes_[id].model;
    }

    double next_time() {
        initialize();
        discard_stale();
        const double internal = events_.empty() ? std::numeric_limits<double>::infinity() : events_.top().time;
        const double external = external_events_.empty() ? std::numeric_limits<double>::infinity()
                                                         : external_events_.top().time;
        return std::min(internal, external);
    }

    std::optional<StepResult<Message>> step() {
        if (in_step_) throw std::logic_error("recursive event step");
        struct StepGuard {
            explicit StepGuard(bool& flag) : flag(flag) { flag = true; }
            ~StepGuard() { flag = false; }
            bool& flag;
        } guard(in_step_);
        const double time = next_time();
        if (!std::isfinite(time)) return std::nullopt;
        if (time < now_) throw std::logic_error("event time moved backward");
        now_ = time;

        StepResult<Message> result;
        result.time = time;
        while (true) {
            discard_stale();
            if (events_.empty() || events_.top().time != time) break;
            result.imminent.push_back(events_.top().id);
            events_.pop();
        }
        std::sort(result.imminent.begin(), result.imminent.end());

        std::vector<std::vector<Input<Message>>> bags(nodes_.size());
        std::vector<bool> imminent(nodes_.size(), false);
        while (true) {
            discard_stale();
            if (external_events_.empty() || external_events_.top().time != time) break;
            const auto& event = external_events_.top();
            auto deliver = [&](std::size_t destination, std::uint32_t port) {
                bags[destination].push_back(Input<Message>{external_source, port, event.value});
                result.injections.push_back(Injection<Message>{destination, port, event.value});
            };
            if (event.root) {
                result.boundary_injections.push_back({event.port, event.value});
                if (const auto found = root_inputs_.find(event.port); found != root_inputs_.end())
                    for (const auto& [destination, port] : found->second) deliver(destination, port);
                if (const auto found = passthrough_.find(event.port); found != passthrough_.end())
                    for (const auto port : found->second) result.boundary_emissions.push_back({port, event.value});
            } else deliver(event.destination, event.port);
            external_events_.pop();
        }
        for (const std::size_t id : result.imminent) {
            imminent[id] = true;
            for (auto& output : nodes_[id].model->output()) {
                result.emissions.push_back(Emission<Message>{id, output.port, output.value});
                if (const auto found = root_outputs_.find({id, output.port}); found != root_outputs_.end())
                    for (const auto port : found->second) result.boundary_emissions.push_back({port, output.value});
                for (const auto& link : couplings_[id]) {
                    if (link.source_port == output.port) {
                        bags[link.destination].push_back(Input<Message>{id, link.destination_port, output.value});
                    }
                }
            }
        }

        for (std::size_t id = 0; id < nodes_.size(); ++id) {
            if (!imminent[id] && bags[id].empty()) continue;
            Node& node = nodes_[id];
            if (imminent[id] && !bags[id].empty()) node.model->confluent_transition(bags[id]);
            else if (imminent[id]) node.model->internal_transition();
            else node.model->external_transition_at(time, time - node.last_transition, bags[id]);
            node.last_transition = time;
            ++node.generation;
            schedule(id);
            result.transitioned.push_back(id);
        }
        return result;
    }

    std::optional<StepResult<Message>> step_transactional() {
        require_idle();
        std::vector<Node> saved_nodes;
        saved_nodes.reserve(nodes_.size());
        for (const auto& node : nodes_) {
            std::unique_ptr<Atomic<Message>> copy;
            if (node.active) {
                copy = node.model->clone();
                if (!copy)
                    throw std::logic_error("transactional step requires cloneable atomics");
            }
            saved_nodes.push_back(Node{std::move(copy), node.last_transition,
                                       node.generation, node.active});
        }
        auto saved_events = events_;
        auto saved_external = external_events_;
        const auto saved_sequence = injection_sequence_;
        const auto saved_time = now_;
        const auto saved_started = started_;
        try {
            return step();
        } catch (...) {
            nodes_ = std::move(saved_nodes);
            events_ = std::move(saved_events);
            external_events_ = std::move(saved_external);
            injection_sequence_ = saved_sequence;
            now_ = saved_time;
            started_ = saved_started;
            throw;
        }
    }

    std::vector<StepResult<Message>> run_until(double horizon, std::size_t max_steps = 1000000) {
        if (!std::isfinite(horizon) || horizon < now_) throw std::invalid_argument("invalid horizon");
        if (max_steps == 0) throw std::invalid_argument("max_steps must be positive");
        std::vector<StepResult<Message>> trace;
        while (next_time() <= horizon) {
            if (trace.size() >= max_steps) throw std::runtime_error("transition limit exceeded; possible zero-time cycle");
            trace.push_back(*step());
        }
        return trace;
    }

    std::vector<StepResult<Message>> run_until_transactional(
        double horizon, std::size_t max_steps = 1000000) {
        if (!std::isfinite(horizon) || horizon < now_) throw std::invalid_argument("invalid horizon");
        if (max_steps == 0) throw std::invalid_argument("max_steps must be positive");
        std::vector<StepResult<Message>> trace;
        while (next_time() <= horizon) {
            if (trace.size() >= max_steps)
                throw std::runtime_error("transition limit exceeded; possible zero-time cycle");
            trace.push_back(*step_transactional());
        }
        return trace;
    }

private:
    friend struct CompiledCoupling<Message>;

    void require_idle() const {
        if (in_step_) throw std::logic_error("cannot change structure during an event step");
    }
    void require_unmanaged() const {
        require_idle();
        if (managed_structure_) throw std::logic_error("edit structure through the compiled hierarchy");
    }

    std::size_t add_impl(std::unique_ptr<Atomic<Message>> model) {
        require_idle();
        if (!model) throw std::invalid_argument("null atomic model");
        const auto id = nodes_.size();
        const auto time = started_ ? event_time(*model, now_) : std::numeric_limits<double>::infinity();
        nodes_.reserve(id + 1);
        couplings_.reserve(id + 1);
        if (std::isfinite(time)) events_.push(Event{time, id, 0});
        nodes_.push_back(Node{std::move(model), now_, 0, true});
        couplings_.emplace_back();
        return id;
    }

    // All validation/allocation precedes the commit. Surviving atomics and their
    // pending events are retained; only removed IDs are invalidated.
    void apply_structure(Routing routing, const std::vector<std::size_t>& removed,
                         std::unique_ptr<Atomic<Message>> added) {
        require_idle();
        const auto count = nodes_.size() + (added ? 1 : 0);
        std::vector<bool> active(count, true);
        for (std::size_t id = 0; id < nodes_.size(); ++id) active[id] = nodes_[id].active;
        for (const auto id : removed) {
            if (id >= nodes_.size() || !active[id]) throw std::out_of_range("component ID is not active");
            active[id] = false;
        }
        auto validate = [&](std::size_t id) {
            if (id >= count || !active[id]) throw std::out_of_range("routing endpoint is not active");
        };
        std::vector<std::vector<Coupling>> links(count);
        for (const auto& edge : routing.internal) {
            validate(edge.source); validate(edge.destination);
            links[edge.source].push_back({edge.source_port, edge.destination, edge.destination_port});
        }
        for (auto& edges : links)
            std::stable_sort(edges.begin(), edges.end(), [](const Coupling& a, const Coupling& b) {
                return std::pair{a.destination, a.destination_port} < std::pair{b.destination, b.destination_port};
            });
        for (const auto& [port, targets] : routing.inputs) {
            (void)port;
            for (const auto& target : targets) validate(target.first);
        }
        for (const auto& [source, ports] : routing.outputs) { (void)ports; validate(source.first); }
        if (added) (void)add_impl(std::move(added));
        for (const auto id : removed) { nodes_[id].active = false; ++nodes_[id].generation; }
        couplings_.swap(links);
        root_inputs_.swap(routing.inputs);
        root_outputs_.swap(routing.outputs);
        passthrough_.swap(routing.passthrough);
    }

    struct Node {
        std::unique_ptr<Atomic<Message>> model;
        double last_transition;
        std::uint64_t generation;
        bool active;
    };
    struct Coupling {
        std::uint32_t source_port;
        std::size_t destination;
        std::uint32_t destination_port;
    };
    struct Event {
        double time;
        std::size_t id;
        std::uint64_t generation;
    };
    struct Later {
        bool operator()(const Event& a, const Event& b) const {
            if (a.time != b.time) return a.time > b.time;
            if (a.id != b.id) return a.id > b.id;
            return a.generation > b.generation;
        }
    };
    struct ExternalEvent {
        double time;
        std::size_t destination;
        std::uint32_t port;
        Message value;
        std::uint64_t sequence;
        bool root;
    };
    struct LaterExternal {
        bool operator()(const ExternalEvent& a, const ExternalEvent& b) const {
            if (a.time != b.time) return a.time > b.time;
            return a.sequence > b.sequence;
        }
    };

    void initialize() {
        if (started_) return;
        started_ = true;
        for (std::size_t id = 0; id < nodes_.size(); ++id)
            if (nodes_[id].active) schedule(id);
    }

    void schedule(std::size_t id) {
        const auto time = event_time(*nodes_[id].model, nodes_[id].last_transition);
        if (std::isfinite(time)) events_.push(Event{time, id, nodes_[id].generation});
    }

    double event_time(const Atomic<Message>& model, double last_transition) const {
        const double advance = model.time_advance();
        if (std::isnan(advance) || advance < 0 || advance == -std::numeric_limits<double>::infinity())
            throw std::invalid_argument("atomic returned an invalid time advance");
        const auto absolute = model.next_event_time();
        if (!absolute && std::isinf(advance)) return std::numeric_limits<double>::infinity();
        const double time = absolute ? *absolute : last_transition + advance;
        if (absolute && time == std::numeric_limits<double>::infinity()) return time;
        if (!std::isfinite(time) || time < now_ || time < last_transition)
            throw std::overflow_error("invalid scheduled event time");
        return time;
    }

    void discard_stale() {
        while (!events_.empty() && (!nodes_[events_.top().id].active ||
               events_.top().generation != nodes_[events_.top().id].generation))
            events_.pop();
        while (!external_events_.empty() && !external_events_.top().root &&
               !nodes_[external_events_.top().destination].active)
            external_events_.pop();
    }

    std::vector<Node> nodes_;
    std::vector<std::vector<Coupling>> couplings_;
    std::map<std::uint32_t, std::vector<std::pair<std::size_t, std::uint32_t>>> root_inputs_;
    std::map<std::pair<std::size_t, std::uint32_t>, std::vector<std::uint32_t>> root_outputs_;
    std::map<std::uint32_t, std::vector<std::uint32_t>> passthrough_;
    std::priority_queue<Event, std::vector<Event>, Later> events_;
    std::priority_queue<ExternalEvent, std::vector<ExternalEvent>, LaterExternal> external_events_;
    std::uint64_t injection_sequence_ = 0;
    double now_ = 0;
    bool started_ = false;
    bool in_step_ = false;
    bool managed_structure_ = false;
};

} // namespace ankurafathom::devs
