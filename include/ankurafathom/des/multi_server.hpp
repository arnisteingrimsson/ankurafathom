#pragma once

#include "ankurafathom/des/single_server.hpp"
#include "ankurafathom/des/queue_discipline.hpp"
#include "ankurafathom/rng/philox.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ankurafathom::des {

struct QueueOptions {
    std::optional<std::size_t> capacity = std::nullopt; // Waiting places, excluding service.
    QueueDiscipline discipline = QueueDiscipline::fifo;
};

struct RejectedArrival {
    Entity entity;
    double rejected_at;
};

// One stream per station. Entity identity, not admission or service-start order,
// addresses each draw. The original Entity::service_duration remains unchanged.
struct ExponentialService {
    double rate;
    std::uint64_t seed = 0;
    std::uint32_t scenario = 0;
    std::uint32_t replication = 0;
    std::uint32_t stream = 0;

    double duration(std::uint64_t entity) const {
        return rng::exponential(rate, rng::draw(seed, {scenario, replication, entity, 0, stream, 0})[0]);
    }
};

template <typename Message = Entity>
class MultiServer final : public devs::Atomic<Message> {
public:
    static constexpr std::uint32_t input_port = 0;
    static constexpr std::uint32_t output_port = 1;
    static constexpr std::uint32_t rejection_port = 2;

    explicit MultiServer(std::size_t capacity, double service_scale = 1,
                         QueueOptions queue = {}, bool emit_rejections = false,
                         std::optional<ExponentialService> service = std::nullopt)
        : servers_(capacity), service_scale_(service_scale), queue_(queue),
          emit_rejections_(emit_rejections), service_(service) {
        if (capacity == 0 || !std::isfinite(service_scale) || service_scale <= 0)
            throw std::invalid_argument("server capacity and service scale must be positive");
        if (!valid_queue_discipline(queue.discipline))
            throw std::invalid_argument("unsupported queue discipline");
        if (service_) {
            if (!std::isfinite(service_->rate) || service_->rate <= 0)
                throw std::invalid_argument("exponential service rate must be finite and positive");
            (void)rng::pack_counter({service_->scenario, service_->replication, 0, 0, service_->stream, 0});
        }
    }

    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<MultiServer>(*this);
    }

    double time_advance() const override {
        return pending_rejections_.empty() ? service_deadline() - clock_ : 0;
    }

    std::optional<double> next_event_time() const override {
        return pending_rejections_.empty() ? service_deadline() : clock_;
    }

private:
    double service_deadline() const {
        double next = std::numeric_limits<double>::infinity();
        for (const auto& server : servers_)
            if (server) next = std::min(next, server->deadline);
        return next;
    }

public:
    std::vector<devs::PortValue<Message>> output() const override {
        const double next = *next_event_time();
        if (!std::isfinite(next)) return {};
        std::vector<devs::PortValue<Message>> result;
        for (const auto& server : servers_) {
            if (server && server->deadline == next) {
                Entity completed = server->entity;
                completed.completed_at = next;
                result.push_back({output_port, Message{completed}});
            }
        }
        for (const auto& entity : pending_rejections_)
            result.push_back({rejection_port, Message{entity}});
        return result;
    }

    void internal_transition() override {
        const double next = *next_event_time();
        if (!std::isfinite(next)) throw std::logic_error("internal transition while all servers are idle");
        MultiServer candidate(*this);
        candidate.advance_to(next);
        candidate.pending_rejections_.clear();
        if (candidate.service_deadline() == candidate.clock_) candidate.finish_due();
        candidate.start_queued();
        *this = std::move(candidate);
    }

    void external_transition(double elapsed, const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || (elapsed > 0 && clock_ + elapsed <= clock_))
            throw std::invalid_argument("invalid server elapsed time");
        external_transition_at(clock_ + elapsed, (clock_ + elapsed) - clock_, bag);
    }

    void external_transition_at(double time, double elapsed,
                                const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || time - clock_ != elapsed)
            throw std::invalid_argument("inconsistent server timestamp");
        MultiServer candidate(*this);
        candidate.advance_to(time);
        candidate.accept(bag);
        candidate.start_queued();
        *this = std::move(candidate);
    }

    void confluent_transition(const std::vector<devs::Input<Message>>& bag) override {
        const double next = *next_event_time();
        if (!std::isfinite(next)) throw std::logic_error("confluent transition while all servers are idle");
        MultiServer candidate(*this);
        candidate.advance_to(next);
        candidate.pending_rejections_.clear();
        if (candidate.service_deadline() == candidate.clock_) candidate.finish_due();
        candidate.accept(bag);
        candidate.start_queued();
        *this = std::move(candidate);
    }

    std::size_t capacity() const noexcept { return servers_.size(); }
    double service_scale() const noexcept { return service_scale_; }
    std::size_t waiting() const noexcept { return waiting_.size(); }
    std::size_t busy_servers() const noexcept {
        return static_cast<std::size_t>(std::count_if(servers_.begin(), servers_.end(),
            [](const auto& server) { return server.has_value(); }));
    }
    std::uint64_t accepted_count() const noexcept { return accepted_; }
    std::uint64_t completed_count() const noexcept { return completed_; }
    std::size_t rejected_count() const noexcept { return rejected_.size(); }
    const std::vector<RejectedArrival>& rejected() const noexcept { return rejected_; }
    const QueueOptions& queue_options() const noexcept { return queue_; }
    double total_waiting_time() const noexcept { return total_waiting_time_; }

    double mean_queue_length(double horizon) const {
        validate_horizon(horizon);
        return (queue_area_ + static_cast<double>(waiting_.size()) * (horizon - clock_)) / horizon;
    }

    double utilization(double horizon) const {
        validate_horizon(horizon);
        return (busy_area_ + static_cast<double>(busy_servers()) * (horizon - clock_)) /
               (static_cast<double>(capacity()) * horizon);
    }

private:
    double service_duration(const Entity& entity) const {
        const double duration = (service_ ? service_->duration(entity.id) : entity.service_duration) * service_scale_;
        if (!std::isfinite(duration) || duration <= 0)
            throw std::invalid_argument("service duration must be finite and positive");
        return duration;
    }

    struct Active {
        Entity entity;
        double deadline;
    };

    void validate_horizon(double horizon) const {
        if (!std::isfinite(horizon) || horizon <= 0 || horizon < clock_)
            throw std::invalid_argument("statistics horizon must be positive and not precede the last transition");
    }

    void advance_to(double time) {
        if (!std::isfinite(time) || time < clock_ || time > *next_event_time())
            throw std::invalid_argument("invalid server event time");
        const double elapsed = time - clock_;
        queue_area_ += static_cast<double>(waiting_.size()) * elapsed;
        busy_area_ += static_cast<double>(busy_servers()) * elapsed;
        if (!std::isfinite(queue_area_) || !std::isfinite(busy_area_))
            throw std::overflow_error("server time-weighted statistics overflow");
        clock_ = time;
    }

    static const Entity& as_entity(const Message& message) {
        if constexpr (std::is_same_v<Message, Entity>) return message;
        else return std::get<Entity>(message);
    }

    void accept(const std::vector<devs::Input<Message>>& bag) {
        std::vector<Entity> incoming;
        std::set<std::uint64_t> new_ids;
        for (const auto& input : bag) {
            const Entity& entity = as_entity(input.value);
            const bool prior_completion = std::isfinite(entity.completed_at);
            (void)service_duration(entity);
            if (input.port != input_port || !std::isfinite(entity.arrived_at) ||
                entity.arrived_at < 0 || !std::isfinite(entity.service_duration) || entity.service_duration <= 0 ||
                entity.arrived_at > clock_ ||
                (!std::isnan(entity.completed_at) && !prior_completion) ||
                (prior_completion && (entity.completed_at < entity.arrived_at ||
                                      entity.completed_at > clock_)) ||
                seen_ids_.contains(entity.id) || !new_ids.insert(entity.id).second)
                throw std::invalid_argument("invalid or duplicate arrival entity or port");
            incoming.push_back(entity);
        }
        // Admission is local to this DEVS bag. Previously accepted work is never evicted.
        if (queue_.discipline == QueueDiscipline::priority)
            std::sort(incoming.begin(), incoming.end(), [](const Entity& a, const Entity& b) {
                return a.priority != b.priority ? a.priority < b.priority : a.id < b.id;
            });
        const auto idle = capacity() - busy_servers();
        for (const auto& entity : incoming) {
            seen_ids_.insert(entity.id);
            // Subtraction avoids overflowing capacity + queue capacity.
            if (queue_.capacity && waiting_.size() >= idle &&
                waiting_.size() - idle >= *queue_.capacity) {
                rejected_.push_back({entity, clock_});
                if (emit_rejections_) pending_rejections_.push_back(entity);
                continue;
            }
            Entity admitted = entity;
            admitted.completed_at = std::numeric_limits<double>::quiet_NaN();
            admitted.entered_at = clock_;
            waiting_.push_back(admitted);
            ++accepted_;
        }
        // Stable ties preserve earlier transition admission; simultaneous new ties use ID.
        if (queue_.discipline == QueueDiscipline::priority)
            std::stable_sort(waiting_.begin(), waiting_.end(), [](const Entity& a, const Entity& b) {
                return a.priority < b.priority;
            });
    }

    void finish_due() {
        bool finished = false;
        for (auto& server : servers_) {
            if (server && server->deadline == clock_) {
                server.reset();
                ++completed_;
                finished = true;
            }
        }
        if (!finished) throw std::logic_error("no server completed at internal event");
    }

    void start_queued() {
        for (auto& server : servers_) {
            if (server || waiting_.empty()) continue;
            Entity entity = queue_.discipline == QueueDiscipline::lifo ? waiting_.back() : waiting_.front();
            if (queue_.discipline == QueueDiscipline::lifo) waiting_.pop_back();
            else waiting_.pop_front();
            total_waiting_time_ += clock_ - entity.entered_at;
            if (!std::isfinite(total_waiting_time_))
                throw std::overflow_error("server waiting-time overflow");
            const double deadline = clock_ + service_duration(entity);
            if (!std::isfinite(deadline) || deadline <= clock_)
                throw std::overflow_error("service deadline is not representable");
            server = Active{entity, deadline};
        }
    }

    double clock_ = 0;
    double queue_area_ = 0;
    double busy_area_ = 0;
    double total_waiting_time_ = 0;
    std::uint64_t accepted_ = 0;
    std::uint64_t completed_ = 0;
    std::deque<Entity> waiting_;
    std::vector<std::optional<Active>> servers_;
    std::set<std::uint64_t> seen_ids_;
    double service_scale_ = 1;
    QueueOptions queue_;
    std::vector<RejectedArrival> rejected_;
    bool emit_rejections_ = false;
    std::vector<Entity> pending_rejections_;
    std::optional<ExponentialService> service_;
};

} // namespace ankurafathom::des
