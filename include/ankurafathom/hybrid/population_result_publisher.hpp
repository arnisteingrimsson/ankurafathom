#pragma once
#include "ankurafathom/abm/population_atomic.hpp"
#include "ankurafathom/hybrid/population_aggregate.hpp"
#include <type_traits>

namespace ankurafathom::hybrid {
template<class Tag>
using PopulationAggregateMessage=std::variant<abm::PopulationCommand<Tag>,abm::PopulationResult<Tag>,
    abm::PopulationTopicInput<Tag>,abm::PopulationLifecycleInput<Tag>,abm::PopulationNetworkInput<Tag>,
    PopulationSnapshot<Tag>,ScalarPublication>;

template<class Tag,class Message=PopulationAggregateMessage<Tag>>
class PopulationResultPublisher final:public devs::Atomic<Message> {
public:
    using Store=abm::PopulationStore<Tag>;
    using Result=abm::PopulationResult<Tag>;
    using Snapshot=PopulationSnapshot<Tag>;
    static constexpr std::uint32_t input_port=0,output_port=1;
    explicit PopulationResultPublisher(Store initial):snapshot_{0,0,std::move(initial)} {}
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<PopulationResultPublisher>(*this); }
    double time_advance()const override { return pending_ ? 0 : std::numeric_limits<double>::infinity(); }
    std::optional<double> next_event_time()const override {
        return pending_ ? snapshot_.time : std::numeric_limits<double>::infinity();
    }
    std::vector<devs::PortValue<Message>> output()const override {
        if(!pending_) return {};
        return {{output_port,Message{snapshot_}}};
    }
    void internal_transition()override {
        if(!pending_) throw std::logic_error("ABM result publisher has no pending snapshot");
        pending_=false;
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(bag.size()!=1 || !std::holds_alternative<Result>(bag[0].value))
            throw std::invalid_argument("ABM result publisher requires one result");
        external_transition_at(std::get<Result>(bag[0].value).time,elapsed,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(pending_ || !std::isfinite(time) || !std::isfinite(elapsed) || elapsed<0 ||
           time<snapshot_.time || time-snapshot_.time!=elapsed)
            throw std::invalid_argument("invalid ABM result time or pending snapshot");
        accept(time,bag);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        if(!pending_) throw std::logic_error("ABM result publisher is not imminent");
        accept(snapshot_.time,bag);
    }
    const Snapshot& snapshot()const noexcept { return snapshot_; }
private:
    void accept(double time,const std::vector<devs::Input<Message>>& bag) {
        if(bag.size()!=1 || bag[0].port!=input_port || (owner_ && *owner_!=bag[0].source))
            throw std::invalid_argument("ABM result publisher requires a single producer on its input port");
        const auto* result=std::get_if<Result>(&bag[0].value);
        if(!result || result->time!=time) throw std::invalid_argument("invalid committed ABM result timestamp");
        const auto& next=result->snapshot;
        const auto& previous=snapshot_.population;
        if(next.store_id()!=previous.store_id() || next.first_id()!=previous.first_id() || next.schema()!=previous.schema())
            throw std::invalid_argument("ABM result changed population namespace or schema");
        if(snapshot_.revision==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("ABM snapshot revision exhausted");
        Snapshot candidate{time,snapshot_.revision+1,next};
        static_assert(std::is_nothrow_move_assignable_v<Snapshot>);
        snapshot_=std::move(candidate);
        owner_=bag[0].source;
        pending_=true;
    }
    Snapshot snapshot_;
    std::optional<std::size_t> owner_;
    bool pending_=true;
};
} // namespace ankurafathom::hybrid
