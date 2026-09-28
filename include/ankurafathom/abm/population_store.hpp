#pragma once
#include "ankurafathom/des/runtime_entity_store.hpp"
#include "ankurafathom/abm/network.hpp"
#include <optional>

namespace ankurafathom::abm {
// Value-owned records and topology. Only read-only access to the underlying
// entity store is exposed, so mutations cannot bypass membership maintenance.
template<class Tag>
class PopulationStore {
public:
    using Records=des::RuntimeEntityStore<Tag>;
    using Reference=typename Records::Reference;
    using Value=typename Records::Value;
    using Record=typename Records::Record;
    using Column=typename Records::Column;
    using Edge=std::pair<Reference,Reference>;
    static constexpr auto max_entity_id=Records::max_entity_id;
    PopulationStore(std::uint32_t id,std::vector<des::EntityField> fields,std::uint64_t first=0)
        :records_(id,std::move(fields),first) {}
    PopulationStore(Records records):records_(std::move(records)) {}
    operator const Records&()const noexcept { return records_; }
    std::uint32_t store_id()const noexcept { return records_.store_id(); }
    std::uint64_t first_id()const noexcept { return records_.first_id(); }
    std::uint64_t next_id()const noexcept { return records_.next_id(); }
    std::size_t size()const noexcept { return records_.size(); }
    std::size_t active_count()const noexcept { return records_.active_count(); }
    const auto& schema()const noexcept { return records_.schema(); }
    const auto& live_rows()const noexcept { return records_.live_rows(); }
    const Column& column(std::size_t index)const { return records_.column(index); }
    std::size_t field_index(const std::string& name)const { return records_.field_index(name); }
    bool alive(Reference ref)const { return records_.alive(ref); }
    Value field(Reference ref,std::size_t index)const { return records_.field(ref,index); }
    Value field(Reference ref,const std::string& name)const { return records_.field(ref,name); }
    Record record(Reference ref)const { return records_.record(ref); }
    void update(Reference ref,const Record& value) { records_.update(ref,value); }
    void update_many(const std::vector<std::pair<Reference,Record>>& updates) { records_.update_many(updates); }
    Reference spawn(const Record& value) { return spawn_many({value}).front(); }
    std::vector<Reference> spawn_many(const std::vector<Record>& values) {
        if(values.empty()) return {};
        auto candidate=*this;
        const auto refs=candidate.records_.spawn_many(values);
        candidate.reconcile();
        *this=std::move(candidate); return refs;
    }
    void retire(Reference ref) {
        auto candidate=*this; candidate.records_.retire(ref); candidate.reconcile(); *this=std::move(candidate);
    }
    const std::optional<CsrNetwork>& network()const noexcept { return network_; }
    void configure_network(CsrNetwork network) {
        if(network.vertices()!=vertices()) throw std::invalid_argument("network vertices must equal live agent IDs");
        network_=std::move(network);
    }
    void edit_network(const std::vector<Edge>& additions,const std::vector<Edge>& removals) {
        if(!network_) throw std::invalid_argument("population has no network");
        const auto edges=[&](const auto& entries) {
            std::vector<CsrNetwork::Edge> result;
            for(const auto& [a,b]:entries) {
                if(!alive(a) || !alive(b)) throw std::invalid_argument("network endpoint is inactive");
                result.emplace_back(a.id,b.id);
            }
            return result;
        };
        // CsrNetwork::edit stages all edits, including duplicate/overlap checks.
        network_->edit(edges(additions),edges(removals));
    }
private:
    std::vector<std::uint64_t> vertices()const {
        std::vector<std::uint64_t> ids;
        for(auto id=first_id();id<next_id();++id) if(alive({store_id(),id})) ids.push_back(id);
        return ids;
    }
    void reconcile() {
        if(!network_) return;
        auto edges=network_->edges();
        std::erase_if(edges,[&](const auto& e) { return !alive({store_id(),e.first}) || !alive({store_id(),e.second}); });
        network_=CsrNetwork(vertices(),std::move(edges),network_->directed());
    }
    Records records_;
    std::optional<CsrNetwork> network_;
};
} // namespace ankurafathom::abm
