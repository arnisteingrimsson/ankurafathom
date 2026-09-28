#pragma once
#include "ankurafathom/hybrid/typed_agent_pool.hpp"
#include "ankurafathom/abm/population_atomic.hpp"

namespace ankurafathom::hybrid {
template<class Tag> struct TypedPoolControl {
    std::uint64_t revision;
    typename TypedAgentPool<Tag>::Change change;
};
template<class Tag> using TypedAgentPoolMessage=std::variant<TypedPoolControl<Tag>,
    typename TypedAgentPool<Tag>::Result,typename TypedAgentPool<Tag>::Notification,
    des::Seize,des::Release,des::Grant,abm::PopulationResult<Tag>>;

template<class Tag,class Message=TypedAgentPoolMessage<Tag>>
class TypedAgentPoolAtomic final:public devs::Atomic<Message> {
public:
    using Core=TypedAgentPool<Tag>;
    using Control=TypedPoolControl<Tag>;
    using Result=typename Core::Result;
    static constexpr std::uint32_t resource_port=0,control_port=2,result_port=65537,
        population_port=65538,notification_port=65539;
    explicit TypedAgentPoolAtomic(Core core):state_(std::make_unique<State>(std::move(core))) {
        if(state_->core.time()!=0) throw std::invalid_argument("typed pool atomic must start at zero");
    }
    TypedAgentPoolAtomic(const TypedAgentPoolAtomic& other):state_(std::make_unique<State>(*other.state_)) {}
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<TypedAgentPoolAtomic>(*this); }
    double time_advance()const override { return state_->pending?0:std::numeric_limits<double>::infinity(); }
    std::optional<double> next_event_time()const override { return state_->pending?time():std::numeric_limits<double>::infinity(); }
    std::vector<devs::PortValue<Message>> output()const override {
        if(!state_->pending) return {};
        const auto& result=*state_->pending;
        std::vector<devs::PortValue<Message>> output;
        for(const auto& grant:result.grants) output.push_back({static_cast<std::uint32_t>(1+(grant.request_id>>48)),Message{grant}});
        output.push_back({result_port,Message{result}});
        output.push_back({population_port,Message{abm::PopulationResult<Tag>{result.time,result.snapshot,{},{},{}}}});
        for(const auto& value:result.notifications) output.push_back({notification_port,Message{value}});
        return output;
    }
    void internal_transition()override {
        if(!state_->pending) throw std::logic_error("typed agent pool has no pending publication");
        state_->pending.reset();
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        external_transition_at(time()+elapsed,elapsed,bag);
    }
    void external_transition_at(double timestamp,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(state_->pending) throw std::logic_error("typed pool pending publication requires confluence");
        if(!std::isfinite(elapsed) || elapsed<0 || !std::isfinite(timestamp) || timestamp<time() || timestamp-time()!=elapsed)
            throw std::invalid_argument("invalid typed pool timestamp");
        accept(timestamp,bag);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        if(!state_->pending) throw std::logic_error("typed pool has no confluent publication");
        accept(time(),bag);
    }
    const Core& core()const noexcept { return state_->core; }
    double time()const noexcept { return core().time(); }
    std::optional<std::uint64_t> revision()const noexcept { return state_->revision; }
private:
    struct State {
        Core core;
        std::optional<Result> pending;
        std::optional<std::size_t> owner;
        std::optional<std::uint64_t> revision;
        explicit State(Core value):core(std::move(value)) {}
    };
    void accept(double timestamp,const std::vector<devs::Input<Message>>& bag) {
        if(busy_) throw std::logic_error("reentrant typed pool atomic transaction");
        struct Guard { bool& busy;explicit Guard(bool& b):busy(b) { busy=true; }~Guard() { busy=false; } } guard(busy_);
        if(bag.empty()) throw std::invalid_argument("empty typed pool input bag");
        auto next=std::make_unique<State>(*state_);
        const Control* control=nullptr;
        std::size_t owner=0;
        std::vector<des::Seize> requests;std::vector<des::Release> releases;
        for(const auto& input:bag) {
            if(const auto* value=std::get_if<Control>(&input.value);input.port==control_port && value) {
                if(control) throw std::invalid_argument("multiple typed pool workforce controls");
                control=value;owner=input.source;
            } else if(const auto* request=std::get_if<des::Seize>(&input.value);input.port==resource_port && request) {
                requests.push_back(*request);
            } else if(const auto* release=std::get_if<des::Release>(&input.value);input.port==resource_port && release) {
                releases.push_back(*release);
            } else throw std::invalid_argument("invalid typed pool input or port");
        }
        typename Core::Change change;
        if(control) {
            if(control->change.time!=timestamp || (next->owner && *next->owner!=owner) ||
               (next->revision && control->revision<=*next->revision))
                throw std::invalid_argument("typed pool control timestamp, owner or revision mismatch");
            change=control->change;next->owner=owner;next->revision=control->revision;
        }
        change.time=timestamp;
        change.requests.insert(change.requests.end(),requests.begin(),requests.end());
        change.releases.insert(change.releases.end(),releases.begin(),releases.end());
        next->pending=next->core.transact(std::move(change));
        state_.swap(next);
    }
    std::unique_ptr<State> state_;
    bool busy_=false;
};
} // namespace ankurafathom::hybrid
