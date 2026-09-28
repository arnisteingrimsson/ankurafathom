#pragma once
#include "ankurafathom/abm/population_store.hpp"
#include <functional>
#include "ankurafathom/abm/population_publication.hpp"

namespace ankurafathom::abm {
namespace detail {
class PopulationBusy {
public:
    explicit PopulationBusy(bool& busy):busy_(busy) {
        if(busy_) throw std::logic_error("recursive population mutation");
        busy_=true;
    }
    ~PopulationBusy() { busy_=false; }
    PopulationBusy(const PopulationBusy&)=delete;
    PopulationBusy& operator=(const PopulationBusy&)=delete;
private:
    bool& busy_;
};
}

// Contract: docs/SEMANTICS.md, "Typed populations and asynchronous timers".
template<class Tag>
class TypedPopulation {
public:
    using Store=PopulationStore<Tag>;
    using Reference=typename Store::Reference;
    using Record=typename Store::Record;
    using Rule=std::function<Record(Reference,const Store&)>;
    using Publication=PopulationPublication<Tag>;
    using Emission=PopulationEmission<Tag>;
    struct Lifecycle { bool retire=false; std::vector<Record> births; };
    using LifecycleRule=std::function<Lifecycle(Reference,const Store&)>;
    using Publisher=std::function<std::vector<Emission>(Reference,const Store&)>;
    using Validator=std::function<void(const Store&)>;
    explicit TypedPopulation(Store store,Validator validator={})
        :store_(std::move(store)),validator_(std::move(validator)) { validate(store_); }
    const Store& store()const noexcept { return store_; }
    const std::vector<Publication>& pending_publications()const noexcept { return outbox_; }
    std::vector<Publication> take_publications() {
        detail::PopulationBusy lock(busy_); std::vector<Publication> result; result.swap(outbox_); return result;
    }
    Reference spawn(const Record& value) { return apply({}, {}, {value}).front(); }
    void retire(Reference agent) { (void)apply({}, {agent}, {}); }
    void update(Reference agent,const Record& value) { (void)apply({{agent,value}}, {}, {}); }
    std::vector<Reference> apply(const std::vector<std::pair<Reference,Record>>& updates,
                                 const std::vector<Reference>& retirements,const std::vector<Record>& births,const std::vector<Publication>& publications={}) {
        detail::PopulationBusy lock(busy_);
        auto candidate=store_;
        std::set<std::uint64_t> updated,retired;
        for(const auto& [agent,value]:updates) {
            (void)value;
            if(!updated.insert(agent.id).second) throw std::invalid_argument("duplicate population update");
        }
        for(auto agent:retirements)
            if(!candidate.alive(agent) || !retired.insert(agent.id).second || updated.contains(agent.id))
                throw std::invalid_argument("inactive, duplicate or updated retiring agent");
        candidate.update_many(updates);
        for(auto agent:retirements) candidate.retire(agent);
        auto result=candidate.spawn_many(births);
        validate(candidate);
        auto outbox=outbox_; outbox.insert(outbox.end(),publications.begin(),publications.end());
        validate_publications(candidate,outbox);
        store_=std::move(candidate); outbox_=std::move(outbox);
        return result;
    }
    void edit_network(const std::vector<typename Store::Edge>& additions,const std::vector<typename Store::Edge>& removals) {
        detail::PopulationBusy lock(busy_);
        auto candidate=store_; candidate.edit_network(additions,removals); validate(candidate); store_=std::move(candidate);
    }
    void add_phase(Rule rule,Publisher publisher={},LifecycleRule lifecycle={}) {
        detail::PopulationBusy lock(busy_);
        if(!rule) throw std::invalid_argument("empty typed population phase");
        phases_.push_back({std::move(rule),std::move(publisher),std::move(lifecycle)});
    }
    void step() {
        detail::PopulationBusy lock(busy_);
        Store candidate=store_;
        auto outbox=outbox_;
        for(const auto& phase:phases_) {
            std::vector<std::pair<Reference,Record>> updates;
            std::vector<Reference> retirements;
            std::vector<Record> births;
            for(auto id=candidate.first_id();id<candidate.next_id();++id) {
                const Reference agent{candidate.store_id(),id};
                if(candidate.alive(agent)) {
                    auto life=phase.lifecycle ? phase.lifecycle(agent,candidate) : Lifecycle{};
                    births.insert(births.end(),life.births.begin(),life.births.end());
                    if(life.retire) { retirements.push_back(agent); continue; }
                    updates.emplace_back(agent,phase.rule(agent,candidate));
                    if(phase.publish) for(const auto& e:phase.publish(agent,candidate)) outbox.push_back({e.topic,agent,e.receiver,e.value});
                }
            }
            candidate.update_many(updates);
            for(auto agent:retirements) candidate.retire(agent);
            candidate.spawn_many(births);
            validate(candidate);
            validate_publications(candidate,outbox);
        }
        store_=std::move(candidate); outbox_=std::move(outbox);
    }
private:
    void validate(const Store& store)const { if(validator_) validator_(store); }
    Store store_;
    Validator validator_;
    struct Phase { Rule rule; Publisher publish; LifecycleRule lifecycle; };
    std::vector<Phase> phases_;
    std::vector<Publication> outbox_;
    bool busy_=false;
};
} // namespace ankurafathom::abm
