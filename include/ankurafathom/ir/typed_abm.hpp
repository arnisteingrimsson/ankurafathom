#pragma once
#include "ankurafathom/abm/population_store.hpp"
#include "ankurafathom/ir/expression.hpp"
#include "ankurafathom/abm/spatial_snapshot.hpp"
#include "ankurafathom/abm/network.hpp"
#include <array>
#include <optional>

namespace ankurafathom::ir {
struct AgentTag {};
using AgentStore=abm::PopulationStore<AgentTag>;
using AgentAssignments=std::vector<std::pair<std::size_t,Expression>>;
struct TypedAbm {
    bool asynchronous=false;
    std::uint64_t agent_limit=1000000;
    std::vector<des::EntityField> fields;
    std::map<std::string,Dimension> units;
    std::vector<AgentStore::Record> agents;
    struct Lifecycle { double time; std::uint64_t sequence; std::vector<std::uint64_t> retire; std::vector<AgentStore::Record> births; };
    std::vector<Lifecycle> lifecycle;
    struct Publish { std::string topic,target; std::uint64_t receiver; Expression value; };
    struct Birth { AgentStore::Record record; std::optional<Expression> guard; AgentAssignments assignments; };
    struct Behavior { std::optional<Expression> retire; std::vector<Birth> births; };
    struct Phase { AgentAssignments assignments; std::vector<Publish> publish; Behavior lifecycle; };
    std::vector<Phase> phases;
    std::optional<abm::SpatialSnapshot<AgentTag>::Binding> space;
    std::optional<abm::CsrNetwork> network;
    struct NetworkGenerator { std::string kind; std::uint32_t stream; std::optional<Expression> probability,degree,m; };
    std::optional<NetworkGenerator> network_generator;
    struct NetworkUpdate { double time; std::uint64_t sequence; std::vector<abm::CsrNetwork::Edge> add,remove; };
    std::vector<NetworkUpdate> network_updates;
    struct Query {
        std::string id,source,op;
        std::size_t field=0;
        double radius=0;
        bool moore=true,include_self=false;
    };
    std::vector<Query> queries;
    std::array<std::string,3> chart_fields;
    std::vector<std::int64_t> states;
    std::int64_t initial=0;
    struct Transition {
        std::string id,trigger,event,pointer;
        std::int64_t source=0,target=0;
        int priority=0;
        std::uint32_t stream=0;
        std::optional<Expression> value,guard;
        AgentAssignments assignments;
        std::vector<Publish> publish;
        Behavior lifecycle;
    };
    struct Topic {
        std::string id;
        std::size_t capacity;
        Dimension unit;
        std::optional<Expression> guard;
        AgentAssignments assignments;
        std::vector<Publish> publish;
    };
    struct Publication {
        double time;
        std::string topic;
        std::uint64_t sender,sequence;
        std::optional<std::uint64_t> receiver;
        double value;
    };
    std::vector<Topic> topics;
    std::vector<Publication> publications;
    std::size_t delivery_budget=100000;
    struct Message { double time; std::uint64_t agent,sequence; std::string event; };
    struct Output { std::string id,metric; std::size_t field=0; std::uint64_t agent=0; std::int64_t state=0; std::size_t query=0; std::optional<double> inactive_value; };
    std::vector<Transition> transitions;
    std::vector<Message> messages;
    std::vector<Output> outputs;
};
} // namespace ankurafathom::ir
