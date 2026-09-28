#pragma once

#include "ankurafathom/abm/typed_population.hpp"
#include <limits>
#include <numeric>

namespace ankurafathom::abm::models {

// Sequential random activation workload. See docs/SEMANTICS.md.
class WealthExchange {
public:
    struct Agent {};
    using Population=TypedPopulation<Agent>;
    using Store=Population::Store;
    struct Draws {
        std::uint64_t seed=0;
        std::uint32_t scenario=0,replication=0,order_stream=1001,recipient_stream=1002;
    };
    WealthExchange(std::vector<std::int64_t> wealth,std::optional<CsrNetwork> network,
                   Draws draws,std::uint64_t first_id=0)
        :population_(initial(wealth,std::move(network),first_id)),draws_(draws) {
        if(draws_.order_stream==draws_.recipient_stream) throw std::invalid_argument("wealth streams overlap");
        for(auto stream:{draws_.order_stream,draws_.recipient_stream})
            (void)rng::pack_counter({draws_.scenario,draws_.replication,first_id,0,stream,0});
    }
    const Store& store()const noexcept { return population_.store(); }
    std::uint32_t sweeps()const noexcept { return sweeps_; }
    std::vector<std::int64_t> wealth()const {
        std::vector<std::int64_t> result;
        for(auto id=store().first_id();id<store().next_id();++id)
            result.push_back(std::get<std::int64_t>(store().field({0,id},0)));
        return result;
    }
    void step() {
        check_step();
        std::vector<std::uint64_t> order(store().size());
        std::iota(order.begin(),order.end(),store().first_id());
        for(auto remaining=order.size();remaining>1;--remaining) {
            const auto j=index(remaining,store().first_id()+remaining-1,draws_.order_stream);
            std::swap(order[remaining-1],order[j]);
        }
        sweep(order,[&](std::size_t,std::uint64_t donor,const std::vector<std::uint64_t>& eligible) {
            return eligible[index(eligible.size(),donor,draws_.recipient_stream)];
        });
    }
    void scripted_step(const std::vector<std::uint64_t>& order,const std::vector<std::uint64_t>& recipients) {
        check_step();
        if(recipients.size()!=order.size()) throw std::invalid_argument("one scripted recipient per activation required");
        sweep(order,[&](std::size_t i,std::uint64_t,const auto&) { return recipients[i]; });
    }
private:
    static Population initial(const std::vector<std::int64_t>& wealth,std::optional<CsrNetwork> network,std::uint64_t first) {
        if(wealth.size()>1000000) throw std::invalid_argument("wealth population exceeds allocation limit");
        std::int64_t total=0;
        std::vector<Store::Record> records;
        for(auto value:wealth) {
            if(value<0 || value>std::numeric_limits<std::int64_t>::max()-total)
                throw std::invalid_argument("wealth must be nonnegative with bounded total");
            total+=value; records.push_back({value});
        }
        Store store(0,{{"wealth",des::FieldKind::integer}},first);
        store.spawn_many(records);
        if(network) store.configure_network(std::move(*network));
        return Population(std::move(store),[total,n=wealth.size()](const Store& s) {
            if(s.active_count()!=n || s.size()!=n) throw std::logic_error("wealth membership changed");
            std::int64_t sum=0;
            for(auto id=s.first_id();id<s.next_id();++id) {
                const auto value=std::get<std::int64_t>(s.field({0,id},0));
                if(value<0 || value>total-sum) throw std::logic_error("wealth invariant failed");
                sum+=value;
            }
            if(sum!=total) throw std::logic_error("wealth is not conserved");
        });
    }
    void check_step()const {
        if(sweeps_>65535) throw std::out_of_range("wealth sweep exceeds draw address");
    }
    std::size_t index(std::uint64_t bound,std::uint64_t entity,std::uint32_t stream)const {
        std::uint32_t retry=0;
        return static_cast<std::size_t>(rng::uniform_index(bound,[&] {
            return rng::draw(draws_.seed,{draws_.scenario,draws_.replication,entity,sweeps_,stream,retry++})[0];
        }));
    }
    template<class Select>
    void sweep(const std::vector<std::uint64_t>& order,Select select) {
        auto sorted=order; std::sort(sorted.begin(),sorted.end());
        if(sorted.size()!=store().size()) throw std::invalid_argument("wealth activation must include every agent once");
        for(std::size_t i=0;i<sorted.size();++i)
            if(sorted[i]!=store().first_id()+i) throw std::invalid_argument("wealth activation is not an ID permutation");
        auto candidate=population_;
        for(std::size_t i=0;i<order.size();++i) {
            const auto donor=order[i];
            const auto current=std::get<std::int64_t>(candidate.store().field({0,donor},0));
            if(current==0) continue;
            std::vector<std::uint64_t> eligible;
            if(candidate.store().network()) {
                const auto neighbors=candidate.store().network()->neighbors(donor);
                eligible.assign(neighbors.begin(),neighbors.end());
            } else eligible=sorted;
            if(eligible.empty()) continue;
            const auto recipient=select(i,donor,eligible);
            if(!std::binary_search(eligible.begin(),eligible.end(),recipient)) throw std::invalid_argument("ineligible wealth recipient");
            if(recipient==donor) continue;
            const auto received=std::get<std::int64_t>(candidate.store().field({0,recipient},0));
            candidate.apply({{{0,donor},{current-1}},{{0,recipient},{received+1}}},{},{});
        }
        population_=std::move(candidate); ++sweeps_;
    }
    Population population_;
    Draws draws_;
    std::uint32_t sweeps_=0;
};

} // namespace ankurafathom::abm::models
