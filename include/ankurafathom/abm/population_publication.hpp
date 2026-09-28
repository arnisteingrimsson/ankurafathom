#pragma once
#include "ankurafathom/des/runtime_entity_store.hpp"
#include <optional>

namespace ankurafathom::abm {
template<class Tag> struct PopulationEmission {
    std::string topic;
    std::optional<typename des::RuntimeEntityStore<Tag>::Reference> receiver;
    double value;
};
template<class Tag> struct PopulationPublication {
    std::string topic;
    typename des::RuntimeEntityStore<Tag>::Reference sender;
    std::optional<typename des::RuntimeEntityStore<Tag>::Reference> receiver;
    double value;
    bool operator==(const PopulationPublication&)const=default;
};
template<class Tag,class Store>
void validate_publications(const Store& store,const std::vector<PopulationPublication<Tag>>& publications) {
    for(const auto& p:publications)
        if(p.topic.empty() || !std::isfinite(p.value) || !store.alive(p.sender) || (p.receiver && !store.alive(*p.receiver)))
            throw std::invalid_argument("publication needs a topic, finite value and live endpoints");
}
} // namespace ankurafathom::abm
