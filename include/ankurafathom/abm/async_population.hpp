#pragma once
#include "ankurafathom/abm/typed_population.hpp"
#include <algorithm>
#include <limits>
#include <tuple>

namespace ankurafathom::abm {

// Contract: docs/SEMANTICS.md, "Typed populations and asynchronous timers".
// One step owns the whole timestamp transaction, including zero-time follow-ups.
template<class Tag>
class AsyncPopulation {
public:
    using Store=PopulationStore<Tag>;
    using Reference=typename Store::Reference;
    using Record=typename Store::Record;
    struct Wakeup { double time; std::string kind; std::uint64_t generation=0; };
    struct Schedule { double time; Reference agent; std::string kind; std::uint64_t generation=0; };
    struct Timer {
        double time;
        Reference agent;
        std::uint64_t id;
        std::string kind;
        std::uint64_t generation=0;
        bool operator==(const Timer&)const=default;
    };
    struct Birth { Record value; std::vector<Wakeup> timers; };
    using Publication=PopulationPublication<Tag>;
    struct Effects {
        std::vector<std::uint64_t> cancellations;
        std::vector<std::pair<Reference,Record>> updates;
        std::vector<Reference> retirements;
        std::vector<Birth> births;
        std::vector<Schedule> schedules;
        std::vector<Publication> publications;
    };
    struct Applied { std::vector<Reference> born; std::vector<std::uint64_t> timers; };
    using Rule=std::function<Effects(const Timer&,const Store&)>;
    using Validator=typename TypedPopulation<Tag>::Validator;
    explicit AsyncPopulation(Store store,Rule rule,std::size_t timestamp_budget=100000,Validator validator={})
        :core_{std::move(store),{},0,0,{}},rule_(std::move(rule)),budget_(timestamp_budget),validator_(std::move(validator)) {
        if(!rule_ || budget_==0) throw std::invalid_argument("async population needs a rule and a positive event budget");
        validate(core_.store);
    }
    const Store& store()const noexcept { return core_.store; }
    const std::vector<Publication>& pending_publications()const noexcept { return core_.outbox; }
    std::vector<Publication> take_publications() {
        detail::PopulationBusy lock(busy_); std::vector<Publication> result; result.swap(core_.outbox); return result;
    }
    double now()const noexcept { return core_.now; }
    std::size_t pending_count()const noexcept { return core_.heap.size(); }
    std::size_t timestamp_budget()const noexcept { return budget_; }
    double next_time()const noexcept { return core_.heap.empty() ? std::numeric_limits<double>::infinity() : core_.heap.front().time; }
    Applied apply(const Effects& effects) {
        detail::PopulationBusy lock(busy_);
        auto candidate=core_;
        auto result=apply_to(candidate,effects);
        validate(candidate.store);
        core_=std::move(candidate);
        return result;
    }
    void edit_network(const std::vector<typename Store::Edge>& additions,const std::vector<typename Store::Edge>& removals) {
        detail::PopulationBusy lock(busy_);
        auto candidate=core_.store; candidate.edit_network(additions,removals); validate(candidate); core_.store=std::move(candidate);
    }
    Reference spawn(const Record& value) { Effects e; e.births.push_back({value,{}}); return apply(e).born.front(); }
    void retire(Reference agent) { Effects e; e.retirements.push_back(agent); (void)apply(e); }
    std::uint64_t schedule(const Schedule& timer) { Effects e; e.schedules.push_back(timer); return apply(e).timers.front(); }
    void cancel(std::uint64_t timer) { Effects e; e.cancellations.push_back(timer); (void)apply(e); }
    std::vector<Timer> step() {
        detail::PopulationBusy lock(busy_);
        return step_unlocked();
    }
    void run_until(double horizon) {
        detail::PopulationBusy lock(busy_);
        if(!std::isfinite(horizon) || horizon<core_.now) throw std::invalid_argument("invalid async horizon");
        while(next_time()<=horizon) (void)step_unlocked();
        core_.now=horizon;
    }
private:
    struct Core { Store store; std::vector<Timer> heap; std::uint64_t next_id; double now; std::vector<Publication> outbox; };
    struct Later {
        bool operator()(const Timer& a,const Timer& b)const {
            return std::tie(a.time,a.agent.id,a.id)>std::tie(b.time,b.agent.id,b.id);
        }
    };
    static std::uint64_t insert(Core& c,const Schedule& schedule) {
        if(!std::isfinite(schedule.time) || schedule.time<c.now || schedule.kind.empty())
            throw std::invalid_argument("invalid timer time or kind");
        if(!c.store.alive(schedule.agent)) throw std::invalid_argument("timer agent is inactive");
        if(c.next_id==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("timer identity exhausted");
        const auto id=c.next_id++;
        c.heap.push_back({schedule.time,schedule.agent,id,schedule.kind,schedule.generation});
        std::push_heap(c.heap.begin(),c.heap.end(),Later{});
        return id;
    }
    static Applied apply_to(Core& c,const Effects& effects) {
        std::set<std::uint64_t> cancelled,retired,updated;
        for(auto id:effects.cancellations) {
            if(!cancelled.insert(id).second || std::none_of(c.heap.begin(),c.heap.end(),[&](const auto& t) { return t.id==id; }))
                throw std::invalid_argument("duplicate or missing timer cancellation");
        }
        for(const auto& [agent,value]:effects.updates) {
            (void)value;
            if(!updated.insert(agent.id).second) throw std::invalid_argument("duplicate agent update");
        }
        for(auto agent:effects.retirements) {
            if(!c.store.alive(agent) || !retired.insert(agent.id).second || updated.contains(agent.id))
                throw std::invalid_argument("inactive, duplicate or updated retiring agent");
        }
        c.store.update_many(effects.updates);
        for(auto agent:effects.retirements) c.store.retire(agent);
        std::erase_if(c.heap,[&](const auto& t) { return cancelled.contains(t.id) || retired.contains(t.agent.id); });
        std::make_heap(c.heap.begin(),c.heap.end(),Later{});
        Applied result;
        for(const auto& birth:effects.births) {
            const auto agent=c.store.spawn(birth.value); result.born.push_back(agent);
            for(const auto& timer:birth.timers) result.timers.push_back(insert(c,{timer.time,agent,timer.kind,timer.generation}));
        }
        for(const auto& timer:effects.schedules) result.timers.push_back(insert(c,timer));
        c.outbox.insert(c.outbox.end(),effects.publications.begin(),effects.publications.end());
        validate_publications(c.store,c.outbox);
        return result;
    }
    std::vector<Timer> step_unlocked() {
        if(core_.heap.empty()) return {};
        auto candidate=core_;
        const double time=candidate.heap.front().time;
        candidate.now=time;
        std::vector<Timer> trace;
        while(!candidate.heap.empty() && candidate.heap.front().time==time) {
            if(trace.size()>=budget_) throw std::runtime_error("async timestamp event budget exceeded");
            std::pop_heap(candidate.heap.begin(),candidate.heap.end(),Later{});
            auto timer=std::move(candidate.heap.back()); candidate.heap.pop_back();
            const auto effects=rule_(timer,candidate.store);
            (void)apply_to(candidate,effects);
            validate(candidate.store);
            trace.push_back(std::move(timer));
        }
        core_=std::move(candidate);
        return trace;
    }
    Core core_;
    Rule rule_;
    std::size_t budget_;
    Validator validator_;
    void validate(const Store& store)const { if(validator_) validator_(store); }
    bool busy_=false;
};
} // namespace ankurafathom::abm
