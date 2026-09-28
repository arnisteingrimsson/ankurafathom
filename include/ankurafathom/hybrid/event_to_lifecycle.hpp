#pragma once
#include "ankurafathom/abm/population_atomic.hpp"
#include <algorithm>
#include <set>

namespace ankurafathom::hybrid {

template<class Event,class Tag>
using LifecycleBridgeMessage=std::variant<Event,abm::PopulationCommand<Tag>,abm::PopulationResult<Tag>,
    abm::PopulationTopicInput<Tag>,abm::PopulationLifecycleInput<Tag>,abm::PopulationNetworkInput<Tag>>;

template<class Event,class Tag,class Message=LifecycleBridgeMessage<Event,Tag>>
class EventToLifecycle final:public devs::Atomic<Message> {
public:
    using Store=abm::PopulationStore<Tag>;
    using Reference=typename Store::Reference;
    using Record=typename Store::Record;
    using Publication=abm::PopulationLifecycleInput<Tag>;
    struct Change { std::vector<Reference> retirements; std::vector<Record> births; };
    struct Limits { std::size_t births=1000000,retirements=1000000; };
    using Key=std::function<std::uint64_t(const Event&)>;
    using Project=std::function<Change(const Event&,double)>;
    static constexpr std::uint32_t input_port=0,output_port=1;
    EventToLifecycle(const Store& prototype,Key key,Project project,Limits limits={})
        :store_(prototype.store_id()),schema_(prototype.schema()),key_(std::move(key)),
         project_(std::move(project)),limits_(limits) {
        if(!key_ || !project_) throw std::invalid_argument("lifecycle bridge requires key and projection");
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<EventToLifecycle>(*this); }
    double time_advance()const override { return pending_.empty()?std::numeric_limits<double>::infinity():0; }
    std::optional<double> next_event_time()const override {
        return pending_.empty()?std::numeric_limits<double>::infinity():clock_;
    }
    std::vector<devs::PortValue<Message>> output()const override { return pending_; }
    void internal_transition()override {
        if(pending_.empty()) throw std::logic_error("lifecycle bridge has no publication");
        pending_.clear();
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(elapsed) || elapsed<0 || (elapsed>0 && clock_+elapsed<=clock_))
            throw std::invalid_argument("invalid lifecycle bridge elapsed time");
        external_transition_at(clock_+elapsed,(clock_+elapsed)-clock_,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!pending_.empty() || !std::isfinite(time) || !std::isfinite(elapsed) || elapsed<0 || time<clock_ || time-clock_!=elapsed)
            throw std::invalid_argument("invalid lifecycle bridge timestamp or pending publication");
        auto candidate=*this;
        candidate.accept(time,bag);
        *this=std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        if(pending_.empty()) throw std::logic_error("lifecycle bridge has no confluent publication");
        auto candidate=*this;
        candidate.pending_.clear();candidate.accept(clock_,bag);
        *this=std::move(candidate);
    }
    double time()const noexcept { return clock_; }
    std::size_t event_count()const noexcept { return seen_.size(); }
private:
    void accept(double time,const std::vector<devs::Input<Message>>& bag) {
        if(bag.empty()) throw std::invalid_argument("empty lifecycle event bag");
        std::vector<std::pair<std::uint64_t,const Event*>> ordered;
        for(const auto& input:bag) {
            const auto* event=std::get_if<Event>(&input.value);
            if(input.port!=input_port || !event) throw std::invalid_argument("invalid lifecycle event message or port");
            const auto key=key_(*event);
            if(!seen_.insert(key).second) throw std::invalid_argument("duplicate lifecycle event key");
            ordered.emplace_back(key,event);
        }
        std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b) { return a.first<b.first; });
        std::set<std::uint64_t> retired;
        std::vector<Record> births;
        for(const auto& [key,event]:ordered) {
            auto change=project_(*event,time);
            if(change.births.size()>limits_.births-births.size() || change.retirements.size()>limits_.retirements-retired.size())
                throw std::length_error("lifecycle bridge batch limit exceeded");
            for(const auto ref:change.retirements)
                if(ref.store!=store_ || ref.id>Store::max_entity_id || !retired.insert(ref.id).second)
                    throw std::invalid_argument("invalid or duplicate lifecycle retirement reference");
            births.insert(births.end(),change.births.begin(),change.births.end());
            pending_.push_back({output_port,Message{Publication{time,key,std::move(change.retirements),std::move(change.births),
                abm::PopulationLifecycleTarget{store_,schema_}}}});
        }
        // Validate schema without claiming ownership of membership or allocated IDs.
        typename Store::Records validation(store_,schema_);
        (void)validation.spawn_many(births);
        clock_=time;
    }
    std::uint32_t store_;
    std::vector<des::EntityField> schema_;
    Key key_;
    Project project_;
    Limits limits_;
    std::set<std::uint64_t> seen_;
    std::vector<devs::PortValue<Message>> pending_;
    double clock_=0;
};
} // namespace ankurafathom::hybrid
