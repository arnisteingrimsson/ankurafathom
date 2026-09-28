#pragma once
#include "ankurafathom/hybrid/agent_stocks.hpp"
#include "ankurafathom/hybrid/population_aggregate.hpp"
#include "ankurafathom/abm/population_atomic.hpp"

namespace ankurafathom::hybrid {
template<class Tag> struct AgentStockInput {
    double time;
    std::string channel;
    std::uint64_t revision;
    typename AgentStocks<Tag>::Change change;
};
template<class Tag> struct AgentStockCommit {
    double time;
    std::uint64_t revision;
    std::vector<double> globals;
    std::optional<typename AgentStocks<Tag>::Receipt> discrete;
};
template<class Tag> using DynamicAgentStockMessage=std::variant<AgentStockInput<Tag>,AgentStockCommit<Tag>,
    PopulationSnapshot<Tag>,ScalarPublication,abm::PopulationLifecycleInput<Tag>>;

template<class Tag,class Message=DynamicAgentStockMessage<Tag>>
class DynamicAgentStocksAtomic final:public devs::Atomic<Message> {
public:
    using Core=AgentStocks<Tag>;
    using Input=AgentStockInput<Tag>;
    using Commit=AgentStockCommit<Tag>;
    using Lifecycle=abm::PopulationLifecycleInput<Tag>;
    static constexpr std::uint32_t input_port=0,lifecycle_port=3,snapshot_port=1,commit_port=2;
    DynamicAgentStocksAtomic(Core core,double dt,std::vector<std::string> channels={})
        :state_(std::make_unique<State>(std::move(core),dt)) {
        if(state_->core.time()!=0 || !std::isfinite(dt) || dt<=0)
            throw std::invalid_argument("dynamic agent stocks require time zero and a positive step");
        for(auto& channel:channels) if(channel.empty() || !state_->channels.emplace(std::move(channel),Channel{}).second)
            throw std::invalid_argument("invalid or duplicate agent stock input channel");
    }
    DynamicAgentStocksAtomic(const DynamicAgentStocksAtomic& other):state_(std::make_unique<State>(*other.state_)) {}
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<DynamicAgentStocksAtomic>(*this); }
    double time_advance()const override { return state_->pending?0:state_->next_tick-core().time(); }
    std::optional<double> next_event_time()const override { return state_->pending?core().time():state_->next_tick; }
    std::vector<devs::PortValue<Message>> output()const override {
        if(!state_->pending) return {};
        return {{snapshot_port,Message{PopulationSnapshot<Tag>{core().time(),state_->revision,core().store()}}},
                {commit_port,Message{Commit{core().time(),state_->revision,core().global_state(),state_->receipt}}}};
    }
    void internal_transition()override {
        if(busy_) throw std::logic_error("recursive dynamic agent stock transition");
        if(state_->pending) { state_->pending=false;return; }
        Guard guard(busy_);auto next=std::make_unique<State>(*state_);
        next->core.step_to(next->next_tick);advance_tick(*next);publish(*next);next->receipt.reset();state_.swap(next);
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        external_transition_at(core().time()+elapsed,elapsed,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(state_->pending) throw std::logic_error("dynamic agent stock publication requires confluence");
        if(!std::isfinite(time) || !std::isfinite(elapsed) || elapsed<0 || time<core().time() || time-core().time()!=elapsed)
            throw std::invalid_argument("invalid dynamic agent stock timestamp");
        accept(time,bag);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        accept(state_->pending?core().time():state_->next_tick,bag);
    }
    const Core& core()const noexcept { return state_->core; }
    std::uint64_t revision()const noexcept { return state_->revision; }
    std::optional<std::uint64_t> input_revision(const std::string& channel)const { return state_->channels.at(channel).revision; }
private:
    struct Channel { std::optional<std::size_t> owner;std::optional<std::uint64_t> revision; };
    struct State {
        Core core;
        double dt,next_tick;
        std::uint64_t tick=1,revision=0;
        bool pending=true;
        std::map<std::string,Channel> channels;
        std::set<std::uint64_t> lifecycle_keys;
        std::optional<typename Core::Receipt> receipt;
        State(Core value,double step):core(std::move(value)),dt(step),next_tick(step) {}
    };
    struct Guard { bool& busy;explicit Guard(bool& b):busy(b) { busy=true; }~Guard() { busy=false; } };
    static void advance_tick(State& state) {
        if(state.tick==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("dynamic agent stock tick exhausted");
        ++state.tick;state.next_tick=static_cast<double>(state.tick)*state.dt;
        if(!std::isfinite(state.next_tick) || state.next_tick<=state.core.time()) throw std::overflow_error("dynamic agent stock tick did not advance");
    }
    static void publish(State& state) {
        if(state.revision==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("dynamic agent stock revision exhausted");
        ++state.revision;state.pending=true;
    }
    void accept(double time,const std::vector<devs::Input<Message>>& bag) {
        if(busy_) throw std::logic_error("recursive dynamic agent stock transition");
        if(bag.empty() || time<core().time() || time>state_->next_tick) throw std::invalid_argument("invalid dynamic agent stock event time or bag");
        Guard guard(busy_);auto next=std::make_unique<State>(*state_);
        std::map<std::string,const Input*> inputs;std::map<std::uint64_t,const Lifecycle*> lifecycle;
        for(const auto& input:bag) std::visit([&](const auto& value) {
            using T=std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T,Input>) {
                const auto found=next->channels.find(value.channel);
                if(input.port!=input_port || value.time!=time || found==next->channels.end() || !inputs.emplace(value.channel,&value).second)
                    throw std::invalid_argument("invalid agent stock input channel, port or timestamp");
                auto& channel=found->second;
                if((channel.owner && *channel.owner!=input.source) || (channel.revision && value.revision<=*channel.revision))
                    throw std::invalid_argument("agent stock input owner or revision mismatch");
                channel.owner=input.source;channel.revision=value.revision;
            } else if constexpr(std::is_same_v<T,Lifecycle>) {
                if(input.port!=lifecycle_port || value.time!=time || !value.target ||
                   value.target->store!=core().store().store_id() || value.target->schema!=core().store().schema() ||
                   !next->lifecycle_keys.insert(value.sequence).second)
                    throw std::invalid_argument("invalid agent stock lifecycle target, timestamp or sequence");
                lifecycle.emplace(value.sequence,&value);
            } else throw std::invalid_argument("invalid dynamic agent stock message");
        },input.value);
        typename Core::Change change;
        for(const auto& [channel,input]:inputs) {
            (void)channel;const auto& part=input->change;
            change.updates.insert(change.updates.end(),part.updates.begin(),part.updates.end());
            change.pulses.insert(change.pulses.end(),part.pulses.begin(),part.pulses.end());
            change.retirements.insert(change.retirements.end(),part.retirements.begin(),part.retirements.end());
            change.births.insert(change.births.end(),part.births.begin(),part.births.end());
        }
        for(const auto& [sequence,input]:lifecycle) {
            (void)sequence;
            change.retirements.insert(change.retirements.end(),input->retirements.begin(),input->retirements.end());
            change.births.insert(change.births.end(),input->births.begin(),input->births.end());
        }
        if(time>next->core.time()) next->core.step_to(time);
        next->receipt=next->core.apply(change);
        if(time==next->next_tick) advance_tick(*next);
        publish(*next);state_.swap(next);
    }
    std::unique_ptr<State> state_;
    bool busy_=false;
};
} // namespace ankurafathom::hybrid
