#pragma once

#include "ankurafathom/ir/expression.hpp"
#include "ankurafathom/ir/typed_process.hpp"
#include "ankurafathom/ir/typed_abm.hpp"
#include "ankurafathom/ir/agent_stock_sd.hpp"
#include "ankurafathom/des/multi_server.hpp"
#include "ankurafathom/des/resource_pool.hpp"
#include "ankurafathom/sd/lookup_table.hpp"
#include "ankurafathom/sd/delay.hpp"
#include "ankurafathom/sd/model.hpp"
#include "ankurafathom/runtime/data.hpp"
#include "ankurafathom/runtime/data_inputs.hpp"
#include "ankurafathom/runtime/population_init.hpp"
#include "ankurafathom/runtime/experiment.hpp"

#include <map>
#include <functional>
#include <span>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ankurafathom::ir {

class Error final : public std::runtime_error {
public:
    Error(std::string code, std::string pointer, std::string message)
        : std::runtime_error(message), code(std::move(code)), pointer(std::move(pointer)) {}
    std::string code;
    std::string pointer;
};

struct Stock {
    std::string id;
    double initial;
    bool non_negative;
    std::string unit;
    bool clip_outflows = false;
    std::vector<std::string> outflow_order = {};
};

struct Flow {
    std::string id;
    std::size_t component_index;
    std::optional<std::string> source;
    std::optional<std::string> destination;
    Expression expression;
    std::string unit;
    bool non_negative = true;
    bool clip_negative = false;
};

struct Auxiliary {
    std::string id;
    std::size_t component_index;
    Expression expression;
    std::string unit;
};

struct Output {
    std::string id;
    std::string source;
    bool delay;
    std::string metric;
    std::optional<Expression> expression = std::nullopt;
};

struct DesExponential {
    std::size_t count;
    double arrival_rate;
    double service_rate;
    double start;
    std::uint64_t first_id;
    std::uint32_t stream;
};

struct DesServer {
    std::string id;
    std::size_t capacity;
    double service_scale;
    des::QueueOptions queue = {};
    struct Service { double rate; std::uint32_t stream; };
    std::optional<Service> service = std::nullopt;
};

struct DesProcess {
    std::string source_id;
    std::size_t source_component_index;
    std::string sink_id;
    std::vector<des::Entity> schedule;
    std::optional<DesExponential> exponential;
    std::vector<DesServer> servers;
    struct Router {
        struct Probability { double match; std::uint32_t stream; };
        std::string id;
        std::int32_t priority_at_most = 0;
        std::optional<Probability> probability = std::nullopt;
    };
    struct Link { std::string from; std::string to; std::string port; };
    std::vector<Router> routers;
    std::vector<std::string> discards;
    std::vector<Link> links;
};

struct CompletionBridge {
    std::string source;
    std::string stock;
    double amount;
};

struct FeedbackBridge {
    std::string stock;
    std::string destination;
    double threshold;
    double service_duration;
    std::uint64_t first_id;
    std::size_t max_count;
};

struct RateBridge {
    std::string stock;
    std::string destination;
    double base_rate;
    double gain;
    double service_rate;
    std::uint64_t first_id;
    std::size_t max_count;
    std::uint32_t stream;
};

struct AbmPopulation {
    std::string id;
    std::size_t count;
    std::size_t initial_adopters;
    std::string innovation_parameter;
    std::string imitation_parameter;
    std::uint32_t stream;
};

struct AdoptionBridge {
    std::string population;
    std::string stock;
};

struct AgentPoolEngagement {
    std::uint64_t request_id;
    std::size_t units;
    double duration;
};

struct AgentPoolChange {
    double time;
    std::vector<des::Release> releases;
    std::vector<std::pair<std::uint64_t, std::size_t>> updates;
    std::vector<std::uint64_t> departures;
    std::vector<std::size_t> hires;
    std::vector<des::Seize> requests;
    std::vector<std::size_t> phases;
    std::vector<AgentPoolEngagement> engagements;
};

struct AgentPoolPhase {
    std::string id;
    Expression capacity_expression;
};

struct AgentPoolOutput {
    enum class Source { pool, population, agent, request, process, stock } source;
    std::string id;
    std::string metric;
    std::uint64_t reference = 0;
};

struct AgentPoolSpec {
    std::string population_id;
    std::string pool_id;
    std::string unit;
    std::size_t max_request_units;
    std::vector<std::size_t> initial_capacities;
    std::vector<AgentPoolPhase> phases;
    std::vector<AgentPoolChange> schedule;
    std::vector<AgentPoolOutput> outputs;
    std::optional<std::string> delivery_id;
};

struct Table {
    std::string id;
    Dimension input_unit;
    Dimension output_unit;
    sd::LookupTable lookup;
};

struct Delay {
    std::string id;
    std::size_t component_index;
    sd::DelayKind type;
    std::size_t order;
    std::variant<double, Expression> duration;
    double initial;
    Expression input;
    std::string unit;
};

struct ParameterDataReceipt {
    std::string id, source, file_hash, canonical_hash;
    runtime::data::Value source_key;
    std::map<std::string, std::string> columns;
    std::map<std::string, double> values;
};

struct SeriesData {
    std::string id, source;
    std::size_t binding_index;
    Dimension input_unit, output_unit;
    runtime::data::SeriesSpec spec;
    runtime::data::ExogenousSeries series;
};

struct PopulationData {
    std::string id, source, population;
    std::vector<runtime::data::PopulationField> fields;
    runtime::data::PopulationReceipt<AgentTag> receipt;
};

struct InputIdentity {
    std::string path, file_hash, canonical_hash, canonical_json;
};

struct ModelCheck {
    std::string id, kind, when = "always", output, direction, comparison;
    std::size_t index = 0;
    double absolute_tolerance = 0, relative_tolerance = 0;
    std::optional<double> minimum, maximum;
    std::vector<std::string> stocks;
    std::optional<Expression> left, right;
};

struct Model {
    enum class Kind { sd, des, hybrid, abm_sd, agent_pool, agent_pool_sd, abm, agent_stock_sd } kind = Kind::sd;
    sd::Integrator integrator = sd::Integrator::euler;
    std::string name;
    InputIdentity input;
    std::string time_unit;
    double dt;
    double horizon;
    double start = 0;
    std::map<std::string, double> parameters;
    std::map<std::string, Dimension> parameter_units;
    std::vector<ParameterDataReceipt> parameter_data;
    std::vector<SeriesData> series_data;
    std::optional<PopulationData> population_data;
    std::vector<Stock> stocks;
    std::vector<Flow> flows;
    std::vector<Auxiliary> auxiliaries; // Deterministic topological order, standalone SD.
    std::vector<Table> tables;
    std::vector<Delay> delays;
    std::vector<Output> outputs;
    std::vector<ModelCheck> checks;
    std::optional<DesProcess> process;
    std::optional<TypedProcess> typed_process;
    std::optional<TypedAbm> typed_abm;
    std::optional<AgentStockSd> agent_stock_sd;
    std::optional<CompletionBridge> completion_bridge;
    std::optional<FeedbackBridge> feedback_bridge;
    std::optional<RateBridge> rate_bridge;
    std::optional<AbmPopulation> abm_population;
    std::optional<AdoptionBridge> adoption_bridge;
    std::optional<AgentPoolSpec> agent_pool;
};

struct Row {
    double time;
    std::string output_id;
    double value;
};

Model load_file(const std::string& path);
Model load_json(std::string_view json,const std::filesystem::path* base=nullptr);
runtime::Experiment load_experiment_json(std::string_view json,const Model& model,
                                         std::optional<std::uint64_t> seed_override={},InputIdentity* receipt=nullptr);
std::vector<Row> run(const Model& model,
                     const std::map<std::string, double>& parameter_overrides = {},
                     std::uint64_t seed = 0, std::uint32_t scenario = 0,
                     std::uint32_t replication = 0);
// Standalone SD observation barrier. Called with the initial state and after
// each committed integration step, before any subsequent step is computed.
// Arguments are borrowed for the duration of the callback. Blocking the callback
// pauses computation; returning false stops with the computed prefix only.
using SDObserver = std::function<bool(double, const std::map<std::string, double>&,
                                     std::span<const Row>)>;
std::vector<Row> run_observed_sd(const Model& model,
                     const std::map<std::string, double>& parameter_overrides,
                     const SDObserver& observer);
int cli(int argc, char** argv);

} // namespace ankurafathom::ir
