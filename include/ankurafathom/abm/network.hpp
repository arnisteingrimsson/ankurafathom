#pragma once

#include "ankurafathom/rng/philox.hpp"
#include "ankurafathom/rng/integer.hpp"
#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace ankurafathom::abm {

// Contract: docs/SEMANTICS.md, "ABM networks and bounded message topics".
// Row spans survive reads and failed edits; a successful edit invalidates them.
class CsrNetwork {
public:
    using Id=std::uint64_t;
    using Edge=std::pair<Id,Id>;
    CsrNetwork(std::vector<Id> vertices,std::vector<Edge> edges,bool directed=false)
        :vertices_(std::move(vertices)),directed_(directed) {
        std::sort(vertices_.begin(),vertices_.end());
        for(std::size_t i=0;i<vertices_.size();++i)
            if(!indices_.emplace(vertices_[i],i).second) throw std::invalid_argument("duplicate network vertex");
        std::set<Edge> unique;
        for(auto edge:edges)
            if(!unique.insert(canonical(edge)).second) throw std::invalid_argument("duplicate network edge");
        edges_.assign(unique.begin(),unique.end());
        std::vector<std::vector<Id>> rows(vertices_.size());
        for(const auto& [from,to]:edges_) {
            rows[indices_.at(from)].push_back(to);
            if(!directed_) rows[indices_.at(to)].push_back(from);
        }
        offsets_.push_back(0);
        for(auto& row:rows) {
            std::sort(row.begin(),row.end());
            adjacency_.insert(adjacency_.end(),row.begin(),row.end());
            offsets_.push_back(adjacency_.size());
        }
    }
    const std::vector<Id>& vertices()const noexcept { return vertices_; }
    const std::vector<Edge>& edges()const noexcept { return edges_; }
    const std::vector<std::size_t>& offsets()const noexcept { return offsets_; }
    const std::vector<Id>& adjacency()const noexcept { return adjacency_; }
    std::size_t edge_count()const noexcept { return edges_.size(); }
    bool directed()const noexcept { return directed_; }
    std::span<const Id> neighbors(Id id)const {
        const auto index=indices_.at(id);
        return std::span<const Id>(adjacency_).subspan(offsets_[index],offsets_[index+1]-offsets_[index]);
    }
    void edit(const std::vector<Edge>& additions,const std::vector<Edge>& removals) {
        if(additions.empty() && removals.empty()) return;
        std::set<Edge> add,remove,changed(edges_.begin(),edges_.end());
        for(auto edge:removals) {
            edge=canonical(edge);
            if(!remove.insert(edge).second || !changed.erase(edge)) throw std::invalid_argument("duplicate or missing edge removal");
        }
        for(auto edge:additions) {
            edge=canonical(edge);
            if(!add.insert(edge).second || remove.contains(edge) || !changed.insert(edge).second)
                throw std::invalid_argument("duplicate, overlapping, or existing edge addition");
        }
        CsrNetwork candidate(vertices_,std::vector<Edge>(changed.begin(),changed.end()),directed_);
        *this=std::move(candidate);
    }
private:
    Edge canonical(Edge edge)const {
        if(!indices_.contains(edge.first) || !indices_.contains(edge.second) || edge.first==edge.second)
            throw std::invalid_argument("missing network endpoint or self-loop");
        if(!directed_ && edge.first>edge.second) std::swap(edge.first,edge.second);
        return edge;
    }
    std::vector<Id> vertices_;
    bool directed_;
    std::map<Id,std::size_t> indices_;
    std::vector<Edge> edges_;
    std::vector<std::size_t> offsets_;
    std::vector<Id> adjacency_;
};

struct NetworkDraws {
    std::uint64_t seed=0;
    std::uint32_t scenario=0,replication=0,stream=0;
};

namespace detail {
inline std::vector<std::uint64_t> network_vertices(std::vector<std::uint64_t> vertices,const NetworkDraws& draws) {
    (void)rng::pack_counter({draws.scenario,draws.replication,0,0,draws.stream,0});
    if(vertices.size()>(std::uint64_t{1}<<32)) throw std::invalid_argument("generator vertex count exceeds draw bound");
    std::sort(vertices.begin(),vertices.end());
    if(std::adjacent_find(vertices.begin(),vertices.end())!=vertices.end()) throw std::invalid_argument("duplicate generator vertex");
    return vertices;
}
inline void probability(double value) {
    if(!std::isfinite(value) || value<0 || value>1) throw std::invalid_argument("invalid graph probability");
}
class NetworkWordSequence {
public:
    explicit NetworkWordSequence(NetworkDraws context):context_(context) {}
    std::uint32_t next() {
        if(ordinal_>0xFFFFFFFFULL) throw std::overflow_error("network draw ordinal exhausted");
        const auto result=rng::draw(context_.seed,{context_.scenario,context_.replication,0,
            static_cast<std::uint32_t>(ordinal_>>16),context_.stream,static_cast<std::uint32_t>(ordinal_&0xFFFF)})[0];
        ++ordinal_; return result;
    }
    std::uint64_t index(std::uint64_t bound) { return rng::uniform_index(bound,[&] { return next(); }); }
private:
    NetworkDraws context_;
    std::uint64_t ordinal_=0;
};
}

inline CsrNetwork erdos_renyi(std::vector<std::uint64_t> vertices,double probability,NetworkDraws draws={}) {
    vertices=detail::network_vertices(std::move(vertices),draws);
    detail::probability(probability);
    const auto n=static_cast<std::uint64_t>(vertices.size());
    if(n>1 && n*(n-1)/2>(std::uint64_t{1}<<48)) throw std::invalid_argument("ER pair ordinal exceeds 48 bits");
    std::vector<CsrNetwork::Edge> edges;
    std::uint64_t ordinal=0;
    for(std::size_t i=0;i<vertices.size();++i) for(std::size_t j=i+1;j<vertices.size();++j,++ordinal)
        if(rng::bernoulli(probability,rng::draw(draws.seed,{draws.scenario,draws.replication,ordinal,0,draws.stream,0})[0]))
            edges.emplace_back(vertices[i],vertices[j]);
    return CsrNetwork(std::move(vertices),std::move(edges));
}

inline CsrNetwork watts_strogatz(std::vector<std::uint64_t> vertices,std::size_t degree,double probability,NetworkDraws draws={}) {
    vertices=detail::network_vertices(std::move(vertices),draws);
    detail::probability(probability);
    const auto n=vertices.size();
    if(n==0 || degree>=n || degree%2) throw std::invalid_argument("ring degree must be even, nonnegative, and smaller than a positive vertex count");
    std::vector<std::set<std::size_t>> adjacency(n);
    for(std::size_t distance=1;distance<=degree/2;++distance) for(std::size_t from=0;from<n;++from) {
        const auto to=(from+distance)%n; adjacency[from].insert(to); adjacency[to].insert(from);
    }
    detail::NetworkWordSequence random(draws);
    for(std::size_t distance=1;distance<=degree/2;++distance) for(std::size_t from=0;from<n;++from) {
        if(!rng::bernoulli(probability,random.next())) continue;
        std::vector<std::size_t> candidates;
        for(std::size_t to=0;to<n;++to) if(from!=to && !adjacency[from].contains(to)) candidates.push_back(to);
        if(candidates.empty()) continue;
        const auto to=candidates[random.index(candidates.size())],old=(from+distance)%n;
        adjacency[from].erase(old); adjacency[old].erase(from);
        adjacency[from].insert(to); adjacency[to].insert(from);
    }
    std::vector<CsrNetwork::Edge> edges;
    for(std::size_t from=0;from<n;++from) for(auto to:adjacency[from]) if(from<to) edges.emplace_back(vertices[from],vertices[to]);
    return CsrNetwork(std::move(vertices),std::move(edges));
}

inline CsrNetwork barabasi_albert(std::vector<std::uint64_t> vertices,std::size_t m,NetworkDraws draws={}) {
    vertices=detail::network_vertices(std::move(vertices),draws);
    if(m==0 || m>=vertices.size()) throw std::invalid_argument("preferential attachment needs 1 <= m < vertex count");
    std::vector<std::uint64_t> degree(vertices.size());
    std::vector<CsrNetwork::Edge> edges;
    for(std::size_t i=1;i<=m;++i) { edges.emplace_back(vertices[0],vertices[i]); ++degree[0]; ++degree[i]; }
    detail::NetworkWordSequence random(draws);
    for(std::size_t from=m+1;from<vertices.size();++from) {
        std::vector<std::uint64_t> weights(degree.begin(),degree.begin()+static_cast<std::ptrdiff_t>(from));
        std::uint64_t total=0;
        for(auto weight:weights) {
            total+=weight;
            if(total>(std::uint64_t{1}<<32)) throw std::overflow_error("preferential weight exceeds integer draw bound");
        }
        std::vector<std::size_t> selected;
        for(std::size_t edge=0;edge<m;++edge) {
            auto choice=random.index(total);
            std::size_t target=0;
            while(choice>=weights[target]) { choice-=weights[target]; ++target; }
            selected.push_back(target); total-=weights[target]; weights[target]=0;
        }
        for(auto to:selected) { edges.emplace_back(vertices[to],vertices[from]); ++degree[to]; ++degree[from]; }
    }
    return CsrNetwork(std::move(vertices),std::move(edges));
}
} // namespace ankurafathom::abm
