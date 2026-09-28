#pragma once
#include "ankurafathom/abm/async_population.hpp"
#include "ankurafathom/rng/philox.hpp"

namespace ankurafathom::abm {

// Contract: docs/SEMANTICS.md, "Flat typed-agent statecharts".
// Definitions are immutable; all dynamic chart state lives in agent columns.
template<class Tag>
class Statechart {
public:
    using Population=AsyncPopulation<Tag>;
    using Store=typename Population::Store;
    using Reference=typename Store::Reference;
    using Record=typename Store::Record;
    using Effects=typename Population::Effects;
    using Emission=PopulationEmission<Tag>;
    using Lifecycle=typename TypedPopulation<Tag>::Lifecycle;
    using LifecycleRule=typename TypedPopulation<Tag>::LifecycleRule;
    using Timer=typename Population::Timer;
    enum class Trigger { message, timeout, rate };
    struct Fields { std::string state,entered,generation; };
    struct Draws { std::uint64_t seed=0; std::uint32_t scenario=0,replication=0; };
    struct Transition {
        std::string name;
        std::int64_t source=0,target=0;
        Trigger trigger=Trigger::message;
        std::string message;
        double value=0;
        std::uint32_t stream=0;
        int priority=0;
        std::function<bool(Reference,const Store&)> guard;
        std::function<Record(Reference,const Store&)> action;
        std::function<std::vector<Emission>(Reference,const Store&)> publish;
        LifecycleRule lifecycle;
    };
    Statechart(const Store& schema,Fields fields,std::vector<std::int64_t> states,
               std::vector<Transition> transitions,std::string timer_prefix="statechart/",std::optional<std::int64_t> birth_initial={})
        :schema_(schema.schema()),transitions_(std::move(transitions)),prefix_(std::move(timer_prefix)),
         state_(schema.field_index(fields.state)),entered_(schema.field_index(fields.entered)),generation_(schema.field_index(fields.generation)),birth_initial_(birth_initial) {
        if(prefix_.empty() || state_==entered_ || state_==generation_ || entered_==generation_ ||
           schema_[state_].kind!=des::FieldKind::integer || schema_[entered_].kind!=des::FieldKind::real ||
           schema_[generation_].kind!=des::FieldKind::integer)
            throw std::invalid_argument("invalid statechart fields or timer prefix");
        for(auto state:states)
            if(state<0 || !states_.insert(state).second) throw std::invalid_argument("invalid or duplicate state");
        if(states_.empty()) throw std::invalid_argument("statechart has no states");
        if(birth_initial_ && !states_.contains(*birth_initial_)) throw std::invalid_argument("unknown newborn initial state");
        std::set<std::string> names;
        std::set<std::uint32_t> streams;
        for(const auto& t:transitions_) {
            if(t.name.empty() || !names.insert(t.name).second || !states_.contains(t.source) || !states_.contains(t.target))
                throw std::invalid_argument("invalid statechart transition identity or endpoint");
            switch(t.trigger) {
                case Trigger::message:
                    if(t.message.empty() || t.value!=0 || t.stream!=0) throw std::invalid_argument("invalid message transition parameters");
                    break;
                case Trigger::timeout:
                    if(!std::isfinite(t.value) || t.value<0 || !t.message.empty() || t.stream!=0)
                        throw std::invalid_argument("invalid timeout parameters");
                    break;
                case Trigger::rate:
                    if(!std::isfinite(t.value) || t.value<0 || !t.message.empty() || !streams.insert(t.stream).second)
                        throw std::invalid_argument("invalid or duplicate rate stream");
                    (void)rng::pack_counter({0,0,0,0,t.stream,0});
                    break;
                default: throw std::invalid_argument("unknown transition trigger");
            }
        }
        std::sort(transitions_.begin(),transitions_.end(),[](const auto& a,const auto& b) {
            return std::tie(a.priority,a.name)<std::tie(b.priority,b.name);
        });
    }
    Effects start(Reference agent,const Store& store,std::int64_t initial,double time,Draws draws={})const {
        check_store(store); check_time(time); check_draws(draws);
        const auto value=store.record(agent);
        if(std::get<std::int64_t>(value[generation_])!=-1) throw std::invalid_argument("statechart is already initialized");
        return enter(agent,store,value,initial,time,draws);
    }
    Effects message(Reference agent,const Store& store,const std::string& event,double time,Draws draws={})const {
        check_store(store); check_time(time); check_draws(draws);
        const auto value=store.record(agent); check_frame(value,time);
        if(event.empty()) throw std::invalid_argument("empty statechart message");
        const auto state=std::get<std::int64_t>(value[state_]);
        for(const auto& t:transitions_)
            if(t.source==state && t.trigger==Trigger::message && t.message==event && (!t.guard || t.guard(agent,store)))
                return transition(t,agent,store,time,draws);
        return {};
    }
    Effects on_timer(const Timer& timer,const Store& store,Draws draws={})const {
        check_store(store); check_time(timer.time); check_draws(draws);
        if(!timer.kind.starts_with(prefix_)) throw std::invalid_argument("timer belongs to another statechart");
        const auto name=timer.kind.substr(prefix_.size());
        const auto found=std::find_if(transitions_.begin(),transitions_.end(),[&](const auto& t) { return t.name==name; });
        if(found==transitions_.end() || found->trigger==Trigger::message || (found->trigger==Trigger::rate && found->value==0))
            throw std::invalid_argument("timer does not name an enabled timed transition");
        const auto value=store.record(timer.agent); check_frame(value,timer.time);
        if(timer.generation!=static_cast<std::uint64_t>(std::get<std::int64_t>(value[generation_])) ||
           found->source!=std::get<std::int64_t>(value[state_])) return {};
        if(timer.time!=deadline(*found,timer.agent,value,draws)) throw std::invalid_argument("statechart timer deadline differs");
        if(found->guard && !found->guard(timer.agent,store)) return {};
        return transition(*found,timer.agent,store,timer.time,draws);
    }
private:
    static void check_time(double time) {
        if(!std::isfinite(time) || time<0) throw std::invalid_argument("invalid statechart time");
    }
    static void check_draws(Draws draws) { (void)rng::pack_counter({draws.scenario,draws.replication,0,0,0,0}); }
    void check_store(const Store& store)const {
        if(store.schema()!=schema_) throw std::invalid_argument("statechart population schema differs");
    }
    void check_frame(const Record& value,double time)const {
        const auto generation=std::get<std::int64_t>(value[generation_]);
        const auto entered=std::get<double>(value[entered_]);
        if(!states_.contains(std::get<std::int64_t>(value[state_])) || generation<0 || generation>65535 || entered<0 || entered>time)
            throw std::invalid_argument("invalid statechart frame or backwards event");
    }
    double deadline(const Transition& t,Reference agent,const Record& value,Draws draws)const {
        const double entered=std::get<double>(value[entered_]);
        double duration=t.value;
        if(t.trigger==Trigger::rate) duration=rng::exponential(t.value,rng::draw(draws.seed,
            {draws.scenario,draws.replication,agent.id,static_cast<std::uint32_t>(std::get<std::int64_t>(value[generation_])),t.stream,0})[0]);
        const double result=entered+duration;
        if(!std::isfinite(result) || (t.value>0 && !(result>entered)))
            throw std::overflow_error("statechart deadline overflows or cannot advance time");
        return result;
    }
    Effects transition(const Transition& t,Reference agent,const Store& store,double time,Draws draws)const {
        const auto life=t.lifecycle ? t.lifecycle(agent,store) : Lifecycle{};
        Effects effects;
        if(life.retire) effects.retirements.push_back(agent);
        else {
            auto value=t.action ? t.action(agent,store) : store.record(agent);
            // Actions cannot choose the generation or overwrite the engine's clock.
            if(value.size()!=schema_.size()) throw std::invalid_argument("statechart action returned wrong record width");
            value[generation_]=store.field(agent,generation_);
            effects=enter(agent,store,std::move(value),t.target,time,draws);
            if(t.publish) for(const auto& e:t.publish(agent,store)) effects.publications.push_back({e.topic,agent,e.receiver,e.value});
        }
        if(!life.births.empty()) {
            if(!birth_initial_) throw std::invalid_argument("statechart birth needs an initial state");
            auto staged=store;
            staged.update_many(effects.updates);
            for(auto ref:effects.retirements) staged.retire(ref);
            const auto born=staged.spawn_many(life.births);
            for(auto ref:born) {
                const auto initialized=start(ref,staged,*birth_initial_,time,draws);
                typename Population::Birth birth{initialized.updates.at(0).second,{}};
                for(const auto& timer:initialized.schedules) birth.timers.push_back({timer.time,timer.kind,timer.generation});
                effects.births.push_back(std::move(birth));
            }
        }
        validate_publications(store,effects.publications);
        return effects;
    }
    Effects enter(Reference agent,const Store& store,Record value,std::int64_t target,double time,Draws draws)const {
        if(!states_.contains(target)) throw std::invalid_argument("unknown target state");
        const auto previous=std::get<std::int64_t>(value[generation_]);
        if(previous<-1 || previous>=65535) throw std::overflow_error("statechart entry generation exhausted");
        value[state_]=target; value[entered_]=time; value[generation_]=previous+1;
        // Reuse the column store's complete type/finite-value validation before
        // returning effects, even when the caller has not applied them yet.
        auto candidate=store; candidate.update(agent,value);
        Effects effects;
        effects.updates.push_back({agent,value});
        for(const auto& t:transitions_) {
            if(t.source!=target || t.trigger==Trigger::message || (t.trigger==Trigger::rate && t.value==0)) continue;
            effects.schedules.push_back({deadline(t,agent,value,draws),agent,prefix_+t.name,static_cast<std::uint64_t>(previous+1)});
        }
        return effects;
    }
    std::vector<des::EntityField> schema_;
    std::vector<Transition> transitions_;
    std::set<std::int64_t> states_;
    std::string prefix_;
    std::size_t state_,entered_,generation_;
    std::optional<std::int64_t> birth_initial_;
};
} // namespace ankurafathom::abm
