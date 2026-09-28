#include "ankurafathom/ir/model.hpp"
#include "typed_des.hpp"
#include "typed_abm.hpp"
#include "agent_stock_sd.hpp"
#include "data_bindings.hpp"
#include "provenance.hpp"
#include "validation.hpp"
#include "explanation.hpp"
#include "visualization.hpp"
#include "ankurafathom/des/process.hpp"
#include "ankurafathom/des/routing.hpp"
#include "ankurafathom/des/multi_server.hpp"
#include "ankurafathom/hybrid/clocked_sd.hpp"
#include "ankurafathom/hybrid/entity_to_pulse.hpp"
#include "ankurafathom/hybrid/stock_to_entity.hpp"
#include "ankurafathom/hybrid/rate_driven_source.hpp"
#include "ankurafathom/hybrid/population_to_stock.hpp"
#include "ankurafathom/hybrid/transactional_agent_pool.hpp"
#include "ankurafathom/hybrid/agent_pool_process.hpp"
#include "ankurafathom/hybrid/agent_pool_process_atomic.hpp"
#include "ankurafathom/hybrid/agent_pool_completion_to_pulse.hpp"
#include "ankurafathom/abm/sync_population.hpp"
#include "ankurafathom/sd/model.hpp"
#include "ankurafathom/runtime/experiment.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include "ankurafathom/runtime/outputs.hpp"
#include "ankurafathom/runtime/design.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
#include <string>
#include <utility>
#include <variant>

namespace ankurafathom::ir {

static void evaluate_auxiliaries(const Model& model, std::map<std::string,double>& values,
        const std::map<std::string,std::function<double(double)>>& functions) {
    for (const auto& auxiliary : model.auxiliaries) {
        try { values.insert_or_assign(auxiliary.id,auxiliary.expression.evaluate(values,functions)); }
        catch (const std::exception& error) {
            throw Error("IR_AUX_RUNTIME","/components/"+std::to_string(auxiliary.component_index)+"/expr",error.what());
        }
    }
}
namespace {

using Json = nlohmann::json;

using DelayState = std::variant<sd::EulerDelay, sd::FixedDelay, sd::HistoryDelay2>;

void validate_delay_duration(const Delay& delay, double duration, double dt) {
    if (delay.type == sd::DelayKind::fixed) {
        (void)sd::FixedDelay::ticks(duration, dt);
    } else if (!std::isfinite(duration) || duration <= 0 || dt > duration / static_cast<double>(delay.order)) {
        throw std::invalid_argument("duration must cover at least one step per stage");
    }
}

DelayState make_delay(const Delay& delay, double duration, double dt) {
    if (delay.type == sd::DelayKind::fixed) return sd::FixedDelay(duration, dt, delay.initial);
    if (delay.type == sd::DelayKind::history2) return sd::HistoryDelay2(duration, delay.initial, dt);
    return sd::EulerDelay(delay.type, delay.order, duration, delay.initial);
}

std::string pointer_token(std::string token) {
    std::string escaped;
    for (char character : token) {
        if (character == '~') escaped += "~0";
        else if (character == '/') escaped += "~1";
        else escaped += character;
    }
    return escaped;
}

void reject_unknown(const Json& object, std::initializer_list<const char*> permitted,
                    const std::string& pointer) {
    if (!object.is_object()) throw Error("IR_TYPE", pointer, "expected an object");
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const char* key : permitted) known |= it.key() == key;
        if (!known)
            throw Error("IR_FIELD", pointer + "/" + pointer_token(it.key()),
                        "unknown field: " + it.key());
    }
}

const Json& field(const Json& object, const char* key, const std::string& pointer) {
    if (!object.is_object() || !object.contains(key))
        throw Error("IR_MISSING", pointer + "/" + key, "required field is missing");
    return object.at(key);
}

std::string string_field(const Json& object, const char* key, const std::string& pointer) {
    const auto& value = field(object, key, pointer);
    if (!value.is_string() || value.get<std::string>().empty())
        throw Error("IR_TYPE", pointer + "/" + key, "expected a nonempty string");
    return value.get<std::string>();
}

double number_field(const Json& object, const char* key, const std::string& pointer) {
    const auto& value = field(object, key, pointer);
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        throw Error("IR_TYPE", pointer + "/" + key, "expected a finite number");
    return value.get<double>();
}

std::uint64_t unsigned_field(const Json& object, const char* key,
                             const std::string& pointer, std::uint64_t maximum) {
    const auto& value = field(object, key, pointer);
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() > maximum)
        throw Error("IR_TYPE", pointer + "/" + key, "expected a bounded unsigned integer");
    return value.get<std::uint64_t>();
}

std::vector<double> number_array_field(const Json& object, const char* key,
                                       const std::string& pointer) {
    const auto& values = field(object, key, pointer);
    if (!values.is_array()) throw Error("IR_TYPE", pointer + "/" + key, "expected an array");
    std::vector<double> result;
    result.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!values[i].is_number() || !std::isfinite(values[i].get<double>()))
            throw Error("IR_TYPE", pointer + "/" + key + "/" + std::to_string(i),
                        "expected a finite number");
        result.push_back(values[i].get<double>());
    }
    return result;
}

Dimension unit_field(const Json& object, const char* key, const std::string& pointer) {
    const auto text = string_field(object, key, pointer);
    try { return Dimension::parse(text); }
    catch (const std::invalid_argument& error) {
        throw Error("IR_UNIT", pointer + "/" + key, error.what());
    }
}

std::string id_field(const Json& object, const std::string& pointer, std::set<std::string>& ids) {
    const auto id = string_field(object, "id", pointer);
    if (id == "t" || id == "dt" || Expression::builtin(id) || !ids.insert(id).second)
        throw Error("IR_ID", pointer + "/id", "reserved or duplicate identifier");
    for (char c : id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            throw Error("IR_ID", pointer + "/id", "identifier must be alphanumeric or underscore");
    }
    if (std::isdigit(static_cast<unsigned char>(id[0])))
        throw Error("IR_ID", pointer + "/id", "identifier cannot start with a digit");
    return id;
}

std::optional<std::string> endpoint(const Json& object, const char* key, const std::string& pointer) {
    if (!object.contains(key) || object.at(key).is_null()) return std::nullopt;
    if (!object.at(key).is_string() || object.at(key).get<std::string>().empty())
        throw Error("IR_TYPE", pointer + "/" + key, "endpoint must be a stock ID or null");
    return object.at(key).get<std::string>();
}

Dimension expression_unit(const Expression& expression,
                          const std::map<std::string, Dimension>& symbols,
                          const std::map<std::string, std::pair<Dimension, Dimension>>& functions,
                          const std::string& pointer) {
    for (const auto& symbol : expression.symbols())
        if (!symbols.contains(symbol))
            throw Error("IR_SYMBOL", pointer, "unknown symbol: " + symbol);
    for (const auto& function : expression.functions())
        if (!Expression::builtin(function) && !functions.contains(function))
            throw Error("IR_FUNCTION", pointer, "unknown lookup table or series: " + function);
    try { return expression.infer_unit(symbols, functions); }
    catch (const std::invalid_argument& error) {
        throw Error("IR_UNIT", pointer, error.what());
    }
}

std::int32_t priority_field(const Json& object, const std::string& key, const std::string& pointer) {
    const auto& value = field(object, key.c_str(), pointer);
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() > 2147483647ULL) ||
        value.get<std::int64_t>() < -2147483648LL || value.get<std::int64_t>() > 2147483647LL)
        throw Error("IR_DES", pointer + "/" + key, "priority must be a signed 32-bit integer");
    return value.get<std::int32_t>();
}

void parse_des(const Json& document, Model& model, std::set<std::string>& ids) {
    if (!model.parameters.empty())
        throw Error("IR_DES", "/parameters", "this DES subset has no parameter expressions");
    const auto& components = field(document, "components", "");
    if (!components.is_array() || components.size() < 3)
        throw Error("IR_DES", "/components", "DES subset requires a source, servers, and a sink");
    DesProcess process;
    std::map<std::string, std::string> kinds;
    std::set<std::uint64_t> entity_ids;
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto pointer = "/components/" + std::to_string(i);
        const auto& item = components[i];
        const auto id = id_field(item, pointer, ids);
        const auto kind = string_field(item, "kind", pointer);
        kinds.emplace(id, kind);
        if (kind == "source") {
            reject_unknown(item, {"id", "kind", "schedule", "exponential"}, pointer);
            if (!process.source_id.empty())
                throw Error("IR_DES", pointer + "/kind", "duplicate process source");
            process.source_id = id;
            process.source_component_index = i;
            if (item.contains("schedule") == item.contains("exponential"))
                throw Error("IR_DES", pointer, "source needs exactly one schedule or exponential field");
            if (item.contains("schedule")) {
                const auto& entries = field(item, "schedule", pointer);
                if (!entries.is_array())
                    throw Error("IR_TYPE", pointer + "/schedule", "schedule must be an array");
                double previous = 0;
                for (std::size_t j = 0; j < entries.size(); ++j) {
                    const auto entry_pointer = pointer + "/schedule/" + std::to_string(j);
                    const auto& entry = entries[j];
                    reject_unknown(entry, {"id", "arrival", "service", "priority"}, entry_pointer);
                    const auto& entity_id = field(entry, "id", entry_pointer);
                    if (!entity_id.is_number_unsigned())
                        throw Error("IR_TYPE", entry_pointer + "/id", "entity ID must be unsigned integer");
                    const auto number = entity_id.get<std::uint64_t>();
                    if (!entity_ids.insert(number).second)
                        throw Error("IR_DES", entry_pointer + "/id", "duplicate entity ID");
                    const double arrival = number_field(entry, "arrival", entry_pointer);
                    const double service = number_field(entry, "service", entry_pointer);
                    if (arrival < previous || arrival < 0 || arrival > model.horizon)
                        throw Error("IR_DES", entry_pointer + "/arrival", "arrival must be ordered and within horizon");
                    if (service <= 0)
                        throw Error("IR_DES", entry_pointer + "/service", "service must be positive");
                    des::Entity entity{number, arrival, service};
                    if (entry.contains("priority"))
                        entity.priority = priority_field(entry, "priority", entry_pointer);
                    process.schedule.push_back(entity);
                    previous = arrival;
                }
            } else {
                const auto exponential_pointer = pointer + "/exponential";
                const auto& generator = field(item, "exponential", pointer);
                reject_unknown(generator,
                    {"count", "arrival_rate", "service_rate", "start", "first_id", "stream"},
                    exponential_pointer);
                const auto& count = field(generator, "count", exponential_pointer);
                const auto& first_id = field(generator, "first_id", exponential_pointer);
                const auto& stream = field(generator, "stream", exponential_pointer);
                if (!count.is_number_unsigned() || count.get<std::uint64_t>() > 1000000)
                    throw Error("IR_DES", exponential_pointer + "/count", "count must be at most one million");
                if (!first_id.is_number_unsigned() || first_id.get<std::uint64_t>() > 0xFFFFFFFFFFFFULL)
                    throw Error("IR_DES", exponential_pointer + "/first_id", "first ID must fit in 48 bits");
                if (!stream.is_number_unsigned() || stream.get<std::uint64_t>() >= 65535)
                    throw Error("IR_DES", exponential_pointer + "/stream", "stream pair must fit in 16 bits");
                const double arrival_rate = number_field(generator, "arrival_rate", exponential_pointer);
                const double service_rate = number_field(generator, "service_rate", exponential_pointer);
                const double start = number_field(generator, "start", exponential_pointer);
                if (arrival_rate <= 0 || service_rate <= 0 || start < 0 || start > model.horizon)
                    throw Error("IR_DES", exponential_pointer, "rates must be positive and start within horizon");
                const auto first = first_id.get<std::uint64_t>();
                const auto amount = count.get<std::uint64_t>();
                if (amount > 0xFFFFFFFFFFFFULL - first + 1)
                    throw Error("IR_DES", exponential_pointer + "/count", "entity ID range exceeds 48 bits");
                process.exponential = DesExponential{static_cast<std::size_t>(amount), arrival_rate,
                    service_rate, start, first, stream.get<std::uint32_t>()};
            }
        } else if (kind == "server") {
            reject_unknown(item, {"id", "kind", "capacity", "service_scale", "queue_capacity", "discipline", "service"}, pointer);
            const auto& capacity = field(item, "capacity", pointer);
            if (!capacity.is_number_unsigned() || capacity.get<std::uint64_t>() == 0 ||
                capacity.get<std::uint64_t>() > 1000000)
                throw Error("IR_DES", pointer + "/capacity", "server capacity must be 1..1000000");
            const double scale = item.contains("service_scale") ?
                number_field(item, "service_scale", pointer) : 1;
            if (scale <= 0)
                throw Error("IR_DES", pointer + "/service_scale", "service scale must be positive");
            des::QueueOptions queue;
            if (item.contains("queue_capacity")) {
                const auto& limit = item.at("queue_capacity");
                if (!limit.is_number_unsigned() || limit.get<std::uint64_t>() > 1000000)
                    throw Error("IR_DES", pointer + "/queue_capacity", "queue capacity must be 0..1000000");
                queue.capacity = limit.get<std::size_t>();
            }
            if (item.contains("discipline")) {
                const auto discipline = string_field(item, "discipline", pointer);
                if (discipline == "priority") queue.discipline = des::QueueDiscipline::priority;
                else if (discipline == "lifo") queue.discipline = des::QueueDiscipline::lifo;
                else if (discipline != "fifo")
                    throw Error("IR_DES", pointer + "/discipline", "discipline must be fifo, lifo, or priority");
            }
            std::optional<DesServer::Service> service;
            if (item.contains("service")) {
                const auto& spec = item.at("service");
                const auto service_pointer = pointer + "/service";
                reject_unknown(spec, {"kind", "rate", "stream"}, service_pointer);
                if (string_field(spec, "kind", service_pointer) != "exponential")
                    throw Error("IR_DES", service_pointer + "/kind", "unsupported station service distribution");
                const double rate = number_field(spec, "rate", service_pointer);
                if (rate <= 0)
                    throw Error("IR_DES", service_pointer + "/rate", "service rate must be positive");
                const auto& stream = field(spec, "stream", service_pointer);
                if (!stream.is_number_unsigned() || stream.get<std::uint64_t>() > 65535)
                    throw Error("IR_DES", service_pointer + "/stream", "stream must fit in 16 bits");
                service = DesServer::Service{rate, stream.get<std::uint32_t>()};
            }
            process.servers.push_back(DesServer{id, capacity.get<std::size_t>(), scale, queue, service});
        } else if (kind == "router") {
            reject_unknown(item, {"id", "kind", "priority_at_most", "probability"}, pointer);
            if (item.contains("priority_at_most") == item.contains("probability"))
                throw Error("IR_DES", pointer, "router requires exactly one priority or probability rule");
            if (item.contains("priority_at_most")) {
                process.routers.push_back({id, priority_field(item, "priority_at_most", pointer)});
            } else {
                const auto probability_pointer = pointer + "/probability";
                const auto& probability = item.at("probability");
                reject_unknown(probability, {"match", "stream"}, probability_pointer);
                const auto chance = number_field(probability, "match", probability_pointer);
                if (chance < 0 || chance > 1)
                    throw Error("IR_DES", probability_pointer + "/match", "probability must be in [0,1]");
                const auto& stream = field(probability, "stream", probability_pointer);
                if (!stream.is_number_unsigned() || stream.get<std::uint64_t>() > 65535)
                    throw Error("IR_DES", probability_pointer + "/stream", "stream must fit in 16 bits");
                process.routers.push_back({id, 0, DesProcess::Router::Probability{chance, stream.get<std::uint32_t>()}});
            }
        } else if (kind == "discard") {
            reject_unknown(item, {"id", "kind"}, pointer);
            process.discards.push_back(id);
        } else if (kind == "sink") {
            reject_unknown(item, {"id", "kind"}, pointer);
            if (!process.sink_id.empty())
                throw Error("IR_DES", pointer + "/kind", "duplicate process sink");
            process.sink_id = id;
        } else {
            throw Error("IR_KIND", pointer + "/kind", "unsupported DES process component");
        }
    }
    if (process.source_id.empty() || process.servers.empty() || process.sink_id.empty())
        throw Error("IR_DES", "/components", "source, server, and sink are all required");

    // Stream ownership is explicit, including the source's reserved service stream.
    std::set<std::uint32_t> streams;
    if (process.exponential) {
        streams.insert(process.exponential->stream);
        streams.insert(process.exponential->stream + 1);
    }
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto& item = components[i];
        if (item.at("kind") == "server" && item.contains("service")) {
            if (!streams.insert(item.at("service").at("stream").get<std::uint32_t>()).second)
                throw Error("IR_DES", "/components/" + std::to_string(i) + "/service/stream",
                            "station service stream overlaps a source, station, or router stream");
        }
        if (item.at("kind") == "router" && item.contains("probability")) {
            if (!streams.insert(item.at("probability").at("stream").get<std::uint32_t>()).second)
                throw Error("IR_DES", "/components/" + std::to_string(i) + "/probability/stream",
                            "routing stream overlaps a source, station, or router stream");
        }
    }

    if (std::any_of(process.servers.begin(), process.servers.end(), [](const auto& server) { return server.service.has_value(); }) ||
        std::any_of(process.routers.begin(), process.routers.end(), [](const auto& router) { return router.probability.has_value(); }))
        for (std::size_t i = 0; i < process.schedule.size(); ++i)
            if (process.schedule[i].id > 0xFFFFFFFFFFFFULL)
                throw Error("IR_DES", "/components/" + std::to_string(process.source_component_index) +
                            "/schedule/" + std::to_string(i) + "/id", "stochastic service/routing requires 48-bit entity IDs");

    const auto& links = field(document, "links", "");
    if (!links.is_array()) throw Error("IR_LINK", "/links", "links must be an array");
    std::map<std::pair<std::string, std::string>, std::string> destinations;
    std::map<std::string, std::vector<DesProcess::Link>> outgoing;
    std::map<std::string, std::size_t> indegree;
    for (const auto& [id, kind] : kinds) { (void)kind; indegree[id] = 0; }
    for (std::size_t i = 0; i < links.size(); ++i) {
        const auto pointer = "/links/" + std::to_string(i);
        reject_unknown(links[i], {"from", "to", "port"}, pointer);
        const auto from = string_field(links[i], "from", pointer);
        const auto to = string_field(links[i], "to", pointer);
        const auto port = links[i].contains("port") ? string_field(links[i], "port", pointer) : "out";
        if (!kinds.contains(from) || !kinds.contains(to) || to == process.source_id)
            throw Error("IR_LINK", pointer, "unknown endpoint or link into source");
        const auto& kind = kinds.at(from);
        const bool valid = ((kind == "source" || kind == "server") && port == "out") ||
            (kind == "server" && port == "rejected") ||
            (kind == "router" && (port == "match" || port == "otherwise"));
        if (!valid || !destinations.emplace(std::make_pair(from, port), to).second)
            throw Error("IR_LINK", pointer, "unsupported port or duplicate output route");
        outgoing[from].push_back({from, to, port});
        ++indegree[to];
        process.links.push_back({from, to, port});
    }
    for (const auto& [id, kind] : kinds) {
        if ((kind == "source" || kind == "server") && !destinations.contains({id, "out"}))
            throw Error("IR_LINK", "/links", "source and server need an out route");
        if (kind == "router" && (!destinations.contains({id, "match"}) ||
                                 !destinations.contains({id, "otherwise"})))
            throw Error("IR_LINK", "/links", "router needs match and otherwise routes");
    }
    // Topological traversal validates cycles and reachability, and tracks whether
    // every possible input to the completion sink has actually completed service.
    std::set<std::string> ready;
    for (const auto& [id, degree] : indegree) if (degree == 0) ready.insert(id);
    std::map<std::string, unsigned> states;
    states[process.source_id] = 1; // 1: uncompleted/rejected, 2: completed.
    std::vector<std::string> order;
    while (!ready.empty()) {
        const auto id = *ready.begin();
        ready.erase(ready.begin());
        if (states[id] == 0) throw Error("IR_LINK", "/links", "unreachable process component");
        order.push_back(id);
        for (const auto& link : outgoing[id]) {
            const auto state = kinds.at(id) == "server" ? (link.port == "out" ? 2U : 1U) : states[id];
            states[link.to] |= state;
            if (--indegree[link.to] == 0) ready.insert(link.to);
        }
    }
    if (order.size() != kinds.size()) throw Error("IR_LINK", "/links", "process graph has a cycle");
    if (states[process.sink_id] != 2)
        throw Error("IR_LINK", "/links", "completion sink can only receive completed work");
    std::vector<DesServer> ordered;
    for (const auto& id : order)
        for (const auto& server : process.servers) if (server.id == id) ordered.push_back(server);
    process.servers = std::move(ordered);
    std::sort(process.routers.begin(), process.routers.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(process.discards.begin(), process.discards.end());
    std::sort(process.links.begin(), process.links.end(), [](const auto& a, const auto& b) {
        return std::tie(a.from, a.port, a.to) < std::tie(b.from, b.port, b.to);
    });

    const auto& outputs = field(document, "outputs", "");
    if (!outputs.is_array() || outputs.empty())
        throw Error("IR_TYPE", "/outputs", "outputs must be a nonempty array");
    std::set<std::string> output_ids;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto pointer = "/outputs/" + std::to_string(i);
        reject_unknown(outputs[i], {"id", "component", "metric"}, pointer);
        const auto id = id_field(outputs[i], pointer, output_ids);
        const auto component = string_field(outputs[i], "component", pointer);
        const auto metric = string_field(outputs[i], "metric", pointer);
        const bool server = std::any_of(process.servers.begin(), process.servers.end(),
            [&component](const DesServer& stage) { return stage.id == component; });
        const bool valid = (component == process.source_id && metric == "emitted") ||
            (server &&
                (metric == "accepted" || metric == "rejected" || metric == "completed" || metric == "waiting" ||
                 metric == "busy" || metric == "queue_mean" || metric == "utilization")) ||
            (kinds.contains(component) && kinds.at(component) == "router" &&
                (metric == "received" || metric == "matched" || metric == "otherwise")) ||
            (kinds.contains(component) && kinds.at(component) == "discard" && metric == "discarded") ||
            (component == process.sink_id &&
                (metric == "completed" || metric == "cycle_total" || metric == "cycle_mean"));
        if (!valid) throw Error("IR_OUTPUT", pointer + "/metric", "unsupported process component or metric");
        model.outputs.push_back(Output{id, component, false, metric});
    }
    model.process = std::move(process);
}

} // namespace

Model load_document(const Json& document, const std::filesystem::path* base = nullptr);

Model load_hybrid(const Json& document) {
    reject_unknown(document,
        {"ir_version", "name", "mode", "time", "sd", "des", "bridges", "outputs"}, "");
    if (string_field(document, "ir_version", "") != "0.1")
        throw Error("IR_VERSION", "/ir_version", "only IR version 0.1 is supported");
    const auto name = string_field(document, "name", "");
    const auto& time = field(document, "time", "");
    const auto& sd_section = field(document, "sd", "");
    const auto& des_section = field(document, "des", "");
    reject_unknown(sd_section, {"parameters", "components"}, "/sd");
    reject_unknown(des_section, {"components", "links"}, "/des");
    const auto& sd_components = field(sd_section, "components", "/sd");
    const auto& des_components = field(des_section, "components", "/des");
    if (!sd_components.is_array() || !des_components.is_array())
        throw Error("IR_TYPE", "/components", "hybrid component sections must be arrays");

    std::set<std::string> all_ids;
    if (sd_section.contains("parameters")) {
        const auto& parameters = sd_section.at("parameters");
        if (!parameters.is_array()) throw Error("IR_TYPE", "/sd/parameters", "parameters must be an array");
        for (std::size_t i = 0; i < parameters.size(); ++i)
            (void)id_field(parameters[i], "/sd/parameters/" + std::to_string(i), all_ids);
    }
    std::string first_stock;
    for (std::size_t i = 0; i < sd_components.size(); ++i) {
        const auto pointer = "/sd/components/" + std::to_string(i);
        const auto id = id_field(sd_components[i], pointer, all_ids);
        const auto kind = string_field(sd_components[i], "kind", pointer);
        if (kind != "stock" && kind != "flow")
            throw Error("IR_KIND", pointer + "/kind", "hybrid SD subset supports stock and flow");
        if (kind == "stock" && first_stock.empty()) first_stock = id;
    }
    if (first_stock.empty()) throw Error("IR_STOCK", "/sd/components", "hybrid model needs an SD stock");
    std::string first_sink;
    for (std::size_t i = 0; i < des_components.size(); ++i) {
        const auto pointer = "/des/components/" + std::to_string(i);
        const auto id = id_field(des_components[i], pointer, all_ids);
        const auto kind = string_field(des_components[i], "kind", pointer);
        if (kind == "sink") first_sink = id;
    }
    Json sd_document = {{"ir_version", "0.1"}, {"name", name}, {"mode", "sd"},
        {"time", time}, {"components", sd_components},
        {"outputs", Json::array({Json{{"id", "__hybrid_stock"}, {"stock", first_stock}}})}};
    if (sd_section.contains("parameters")) sd_document["parameters"] = sd_section.at("parameters");
    Model sd_model;
    try { sd_model = load_document(sd_document); }
    catch (const Error& error) { throw Error(error.code, "/sd" + error.pointer, error.what()); }
    for (const auto& flow : sd_model.flows)
        for (const auto& symbol : flow.expression.symbols())
            if (symbol == "dt")
                throw Error("IR_HYBRID", "/sd/components/" +
                    std::to_string(flow.component_index) + "/expr",
                    "hybrid flow cannot depend on dt because off-grid events split steps");

    Json des_document = {{"ir_version", "0.1"}, {"name", name}, {"mode", "des"},
        {"time", time}, {"components", des_components},
        {"links", field(des_section, "links", "/des")},
        {"outputs", Json::array({Json{{"id", "__hybrid_sink"},
            {"component", first_sink}, {"metric", "completed"}}})}};
    Model des_model;
    try { des_model = load_document(des_document); }
    catch (const Error& error) { throw Error(error.code, "/des" + error.pointer, error.what()); }
    if (!des_model.process->routers.empty() || !des_model.process->discards.empty() ||
        std::any_of(des_model.process->links.begin(), des_model.process->links.end(),
                    [](const auto& link) { return link.port != "out"; }))
        throw Error("IR_HYBRID", "/des/links", "hybrid subset currently requires a linear process");

    const auto& bridges = field(document, "bridges", "");
    if (!bridges.is_array() || bridges.empty() || bridges.size() > 2)
        throw Error("IR_HYBRID", "/bridges", "hybrid subset needs one completion bridge and at most one feedback bridge");
    std::optional<CompletionBridge> completion;
    std::optional<FeedbackBridge> feedback;
    std::optional<RateBridge> rate_bridge;
    const auto check_source_id_overlap = [&](std::uint64_t first, std::uint64_t count,
                                             const std::string& pointer) {
        const auto last = first + count - 1;
        for (const auto& entity : des_model.process->schedule)
            if (entity.id >= first && entity.id <= last)
                throw Error("IR_HYBRID", pointer, "bridge IDs overlap scheduled source IDs");
        if (des_model.process->exponential) {
            const auto& generator = *des_model.process->exponential;
            if (generator.count > 0) {
                const auto source_last = generator.first_id + generator.count - 1;
                if (first <= source_last && generator.first_id <= last)
                    throw Error("IR_HYBRID", pointer, "bridge IDs overlap exponential source IDs");
            }
        }
    };
    for (std::size_t i = 0; i < bridges.size(); ++i) {
        const auto pointer = "/bridges/" + std::to_string(i);
        const auto& bridge = bridges[i];
        const auto kind = string_field(bridge, "kind", pointer);
        const auto from = string_field(bridge, "from", pointer);
        const auto to = string_field(bridge, "to", pointer);
        if (kind == "completion_to_stock") {
            reject_unknown(bridge, {"kind", "from", "to", "amount", "unit"}, pointer);
            if (completion) throw Error("IR_HYBRID", pointer + "/kind", "duplicate completion bridge");
            const double amount = number_field(bridge, "amount", pointer);
            if (from != des_model.process->sink_id || amount <= 0)
                throw Error("IR_HYBRID", pointer, "bridge must originate at sink with positive amount");
            const auto stock = std::find_if(sd_model.stocks.begin(), sd_model.stocks.end(),
                [&to](const Stock& item) { return item.id == to; });
            if (stock == sd_model.stocks.end())
                throw Error("IR_REF", pointer + "/to", "bridge target must be an SD stock");
            if (!(unit_field(bridge, "unit", pointer) == Dimension::parse(stock->unit)))
                throw Error("IR_UNIT", pointer + "/unit", "bridge amount unit must match target stock");
            completion = CompletionBridge{from, to, amount};
        } else if (kind == "stock_to_arrival") {
            reject_unknown(bridge, {"kind", "from", "to", "threshold", "unit",
                "service", "first_id", "max_count"}, pointer);
            if (feedback) throw Error("IR_HYBRID", pointer + "/kind", "duplicate feedback bridge");
            const auto stock = std::find_if(sd_model.stocks.begin(), sd_model.stocks.end(),
                [&from](const Stock& item) { return item.id == from; });
            if (stock == sd_model.stocks.end())
                throw Error("IR_REF", pointer + "/from", "feedback source must be an SD stock");
            if (to != des_model.process->servers.front().id)
                throw Error("IR_REF", pointer + "/to", "feedback target must be first DES station");
            if (!(unit_field(bridge, "unit", pointer) == Dimension::parse(stock->unit)))
                throw Error("IR_UNIT", pointer + "/unit", "feedback threshold unit must match source stock");
            const double threshold = number_field(bridge, "threshold", pointer);
            const double service = number_field(bridge, "service", pointer);
            if (service <= 0) throw Error("IR_HYBRID", pointer + "/service", "service duration must be positive");
            const auto& first_json = field(bridge, "first_id", pointer);
            const auto& count_json = field(bridge, "max_count", pointer);
            constexpr std::uint64_t max_id = hybrid::StockToEntity<std::variant<des::Entity, double>>::max_entity_id;
            if (!first_json.is_number_unsigned() || first_json.get<std::uint64_t>() > max_id)
                throw Error("IR_HYBRID", pointer + "/first_id", "feedback ID must fit in 48 bits");
            if (!count_json.is_number_unsigned() || count_json.get<std::uint64_t>() == 0 ||
                count_json.get<std::uint64_t>() > 1000000)
                throw Error("IR_HYBRID", pointer + "/max_count", "max_count must be 1..1000000");
            const auto first = first_json.get<std::uint64_t>();
            const auto count = count_json.get<std::uint64_t>();
            if (count - 1 > max_id - first)
                throw Error("IR_HYBRID", pointer + "/max_count", "feedback ID range exceeds 48 bits");
            check_source_id_overlap(first, count, pointer + "/first_id");
            feedback = FeedbackBridge{from, to, threshold, service, first,
                static_cast<std::size_t>(count)};
        } else if (kind == "stock_to_rate") {
            reject_unknown(bridge, {"kind", "from", "to", "base_rate", "gain", "rate_unit",
                "gain_unit", "service_rate", "service_rate_unit", "first_id", "max_count", "stream"}, pointer);
            if (rate_bridge) throw Error("IR_HYBRID", pointer + "/kind", "duplicate rate bridge");
            const auto stock = std::find_if(sd_model.stocks.begin(), sd_model.stocks.end(),
                [&from](const Stock& item) { return item.id == from; });
            if (stock == sd_model.stocks.end())
                throw Error("IR_REF", pointer + "/from", "rate source must be an SD stock");
            if (to != des_model.process->servers.front().id)
                throw Error("IR_REF", pointer + "/to", "rate target must be first DES station");
            const auto reciprocal_time = Dimension::parse("1/" + sd_model.time_unit);
            if (!(unit_field(bridge, "rate_unit", pointer) == reciprocal_time))
                throw Error("IR_UNIT", pointer + "/rate_unit", "arrival rate must be reciprocal model time");
            if (!(unit_field(bridge, "service_rate_unit", pointer) == reciprocal_time))
                throw Error("IR_UNIT", pointer + "/service_rate_unit", "service rate must be reciprocal model time");
            if (!(unit_field(bridge, "gain_unit", pointer) ==
                    reciprocal_time.divided(Dimension::parse(stock->unit))))
                throw Error("IR_UNIT", pointer + "/gain_unit", "gain must map stock to arrival rate");
            const double base_rate = number_field(bridge, "base_rate", pointer);
            const double gain = number_field(bridge, "gain", pointer);
            const double service_rate = number_field(bridge, "service_rate", pointer);
            if (base_rate < 0 || service_rate <= 0)
                throw Error("IR_HYBRID", pointer, "base rate must be nonnegative and service rate positive");
            const auto& first_json = field(bridge, "first_id", pointer);
            const auto& count_json = field(bridge, "max_count", pointer);
            const auto& stream_json = field(bridge, "stream", pointer);
            constexpr std::uint64_t max_id = hybrid::RateDrivenSource<std::variant<des::Entity, double>>::max_entity_id;
            if (!first_json.is_number_unsigned() || first_json.get<std::uint64_t>() > max_id)
                throw Error("IR_HYBRID", pointer + "/first_id", "rate-source ID must fit in 48 bits");
            if (!count_json.is_number_unsigned() || count_json.get<std::uint64_t>() == 0 ||
                count_json.get<std::uint64_t>() > 1000000)
                throw Error("IR_HYBRID", pointer + "/max_count", "max_count must be 1..1000000");
            if (!stream_json.is_number_unsigned() || stream_json.get<std::uint64_t>() >= 65535)
                throw Error("IR_HYBRID", pointer + "/stream", "stream pair must fit in 16 bits");
            const auto first = first_json.get<std::uint64_t>();
            const auto count = count_json.get<std::uint64_t>();
            const auto stream = stream_json.get<std::uint32_t>();
            if (count - 1 > max_id - first)
                throw Error("IR_HYBRID", pointer + "/max_count", "rate-source ID range exceeds 48 bits");
            check_source_id_overlap(first, count, pointer + "/first_id");
            if (des_model.process->exponential) {
                const auto original_stream = des_model.process->exponential->stream;
                if (stream <= original_stream + 1 && original_stream <= stream + 1)
                    throw Error("IR_HYBRID", pointer + "/stream", "rate-source streams overlap source streams");
            }
            for (const auto& server : des_model.process->servers)
                if (server.service && stream <= server.service->stream && server.service->stream <= stream + 1)
                    throw Error("IR_HYBRID", pointer + "/stream", "rate-source streams overlap station service stream");
            rate_bridge = RateBridge{from, to, base_rate, gain, service_rate,
                first, static_cast<std::size_t>(count), stream};
        } else {
            throw Error("IR_HYBRID", pointer + "/kind", "unsupported bridge kind");
        }
    }
    if (!completion) throw Error("IR_HYBRID", "/bridges", "completion-to-stock bridge is required");
    if (feedback && rate_bridge)
        throw Error("IR_HYBRID", "/bridges", "hybrid subset supports one SD-to-DES bridge");

    sd_model.kind = Model::Kind::hybrid;
    sd_model.process = std::move(des_model.process);
    sd_model.completion_bridge = std::move(completion);
    sd_model.feedback_bridge = std::move(feedback);
    sd_model.rate_bridge = std::move(rate_bridge);
    sd_model.outputs.clear();
    const auto& outputs = field(document, "outputs", "");
    if (!outputs.is_array() || outputs.empty())
        throw Error("IR_TYPE", "/outputs", "outputs must be a nonempty array");
    std::set<std::string> output_ids;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto pointer = "/outputs/" + std::to_string(i);
        reject_unknown(outputs[i], {"id", "stock", "component", "metric"}, pointer);
        const auto id = id_field(outputs[i], pointer, output_ids);
        if (outputs[i].contains("stock") == outputs[i].contains("component"))
            throw Error("IR_OUTPUT", pointer, "output needs exactly one stock or component");
        if (outputs[i].contains("stock")) {
            if (outputs[i].contains("metric"))
                throw Error("IR_OUTPUT", pointer + "/metric", "stock output has no process metric");
            const auto source = string_field(outputs[i], "stock", pointer);
            if (std::none_of(sd_model.stocks.begin(), sd_model.stocks.end(),
                    [&source](const Stock& item) { return item.id == source; }))
                throw Error("IR_REF", pointer + "/stock", "unknown SD stock");
            sd_model.outputs.push_back(Output{id, source, false, ""});
        } else {
            const auto source = string_field(outputs[i], "component", pointer);
            const auto metric = string_field(outputs[i], "metric", pointer);
            const auto& process = *sd_model.process;
            const bool server = std::any_of(process.servers.begin(), process.servers.end(),
                [&source](const DesServer& item) { return item.id == source; });
            const bool valid = (source == process.source_id && metric == "emitted") ||
                (server && (metric == "accepted" || metric == "rejected" || metric == "completed" ||
                    metric == "waiting" || metric == "busy" || metric == "queue_mean" ||
                    metric == "utilization")) ||
                (source == process.sink_id && (metric == "completed" ||
                    metric == "cycle_total" || metric == "cycle_mean"));
            if (!valid) throw Error("IR_OUTPUT", pointer + "/metric", "unsupported process metric");
            sd_model.outputs.push_back(Output{id, source, false, metric});
        }
    }
    return sd_model;
}

Model load_abm_sd(const Json& document) {
    reject_unknown(document, {"ir_version", "name", "mode", "time", "sd", "abm", "bridges", "outputs"}, "");
    if (string_field(document, "ir_version", "") != "0.1")
        throw Error("IR_VERSION", "/ir_version", "only IR version 0.1 is supported");
    const auto name = string_field(document, "name", "");
    const auto& time = field(document, "time", "");
    const auto& sd_section = field(document, "sd", "");
    const auto& abm_section = field(document, "abm", "");
    reject_unknown(sd_section, {"parameters", "components"}, "/sd");
    reject_unknown(abm_section, {"population"}, "/abm");
    const auto& components = field(sd_section, "components", "/sd");
    if (!components.is_array()) throw Error("IR_TYPE", "/sd/components", "SD components must be an array");
    std::string first_stock;
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto pointer = "/sd/components/" + std::to_string(i);
        const auto kind = string_field(components[i], "kind", pointer);
        if (kind != "stock" && kind != "flow")
            throw Error("IR_KIND", pointer + "/kind", "ABM-SD subset supports stocks and flows");
        if (kind == "stock" && first_stock.empty())
            first_stock = string_field(components[i], "id", pointer);
    }
    if (first_stock.empty()) throw Error("IR_STOCK", "/sd/components", "ABM-SD model needs a stock");
    Json sd_document = { {"ir_version", "0.1"}, {"name", name}, {"mode", "sd"},
        {"time", time}, {"components", components},
        {"outputs", Json::array({Json{{"id", "__abm_stock"}, {"stock", first_stock}}})} };
    if (sd_section.contains("parameters")) sd_document["parameters"] = sd_section.at("parameters");
    Model model;
    try { model = load_document(sd_document); }
    catch (const Error& error) { throw Error(error.code, "/sd" + error.pointer, error.what()); }
    if (std::llround(model.horizon / model.dt) > 65535)
        throw Error("IR_ABM", "/time/horizon", "ABM step index must fit in 16 bits");
    for (const auto& flow : model.flows)
        if (flow.expression.symbols().contains("dt"))
            throw Error("IR_ABM", "/sd/components/" +
                std::to_string(flow.component_index) + "/expr",
                "clocked SD flow cannot depend on nominal dt");

    const auto& population = field(abm_section, "population", "/abm");
    reject_unknown(population, {"id", "count", "initial_adopters", "innovation_param",
        "imitation_param", "stream"}, "/abm/population");
    std::set<std::string> ids;
    for (const auto& [id, _] : model.parameters) ids.insert(id);
    for (const auto& stock : model.stocks) ids.insert(stock.id);
    for (const auto& flow : model.flows) ids.insert(flow.id);
    const auto population_id = id_field(population, "/abm/population", ids);
    const auto& count_json = field(population, "count", "/abm/population");
    const auto& initial_json = field(population, "initial_adopters", "/abm/population");
    const auto& stream_json = field(population, "stream", "/abm/population");
    if (!count_json.is_number_unsigned() || count_json.get<std::uint64_t>() == 0 ||
        count_json.get<std::uint64_t>() > 1000000)
        throw Error("IR_ABM", "/abm/population/count", "population count must be 1..1000000");
    if (!initial_json.is_number_unsigned() ||
        initial_json.get<std::uint64_t>() > count_json.get<std::uint64_t>())
        throw Error("IR_ABM", "/abm/population/initial_adopters", "initial adopters exceed count");
    if (!stream_json.is_number_unsigned() || stream_json.get<std::uint64_t>() > 65535)
        throw Error("IR_ABM", "/abm/population/stream", "stream must fit in 16 bits");
    const auto innovation = string_field(population, "innovation_param", "/abm/population");
    const auto imitation = string_field(population, "imitation_param", "/abm/population");
    const auto rate_unit = Dimension::parse("1/" + model.time_unit);
    for (const auto& [id, field_name] : {
            std::pair<std::string, const char*>{innovation, "innovation_param"},
            std::pair<std::string, const char*>{imitation, "imitation_param"}}) {
        if (!model.parameters.contains(id))
            throw Error("IR_REF", "/abm/population/" + std::string(field_name), "unknown adoption parameter");
        if (!(model.parameter_units.at(id) == rate_unit))
            throw Error("IR_UNIT", "/abm/population/" + std::string(field_name),
                "adoption parameter must be reciprocal model time");
    }
    const double p = model.parameters.at(innovation);
    const double q = model.parameters.at(imitation);
    if (p < 0 || q < 0 || !std::isfinite((p + q) * model.dt) || (p + q) * model.dt > 1)
        throw Error("IR_ABM", "/abm/population", "adoption probability bound exceeds one");
    model.abm_population = AbmPopulation{population_id,
        count_json.get<std::size_t>(), initial_json.get<std::size_t>(),
        innovation, imitation, stream_json.get<std::uint32_t>()};

    const auto& bridges = field(document, "bridges", "");
    if (!bridges.is_array() || bridges.size() != 1)
        throw Error("IR_ABM", "/bridges", "ABM-SD subset needs one adoption-to-stock bridge");
    const auto& bridge = bridges[0];
    reject_unknown(bridge, {"kind", "from", "to", "unit"}, "/bridges/0");
    if (string_field(bridge, "kind", "/bridges/0") != "adoption_to_stock")
        throw Error("IR_ABM", "/bridges/0/kind", "unsupported ABM bridge kind");
    const auto from = string_field(bridge, "from", "/bridges/0");
    const auto to = string_field(bridge, "to", "/bridges/0");
    if (from != population_id)
        throw Error("IR_REF", "/bridges/0/from", "bridge source must be the ABM population");
    const auto stock = std::find_if(model.stocks.begin(), model.stocks.end(),
        [&to](const Stock& item) { return item.id == to; });
    if (stock == model.stocks.end())
        throw Error("IR_REF", "/bridges/0/to", "bridge target must be an SD stock");
    if (!(unit_field(bridge, "unit", "/bridges/0") == Dimension::parse(stock->unit)))
        throw Error("IR_UNIT", "/bridges/0/unit", "bridge unit must match target stock");
    if (stock->initial != 0 || !stock->non_negative)
        throw Error("IR_ABM", "/bridges/0/to", "aggregate target must start at zero and be nonnegative");
    for (const auto& flow : model.flows)
        if (flow.source == to || flow.destination == to)
            throw Error("IR_ABM", "/sd/components/" + std::to_string(flow.component_index),
                "aggregate target cannot be a flow endpoint");
    model.adoption_bridge = AdoptionBridge{from, to};
    model.kind = Model::Kind::abm_sd;

    model.outputs.clear();
    const auto& outputs = field(document, "outputs", "");
    if (!outputs.is_array() || outputs.empty())
        throw Error("IR_TYPE", "/outputs", "outputs must be a nonempty array");
    std::set<std::string> output_ids;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto pointer = "/outputs/" + std::to_string(i);
        reject_unknown(outputs[i], {"id", "stock", "population", "metric"}, pointer);
        const auto id = id_field(outputs[i], pointer, output_ids);
        if (outputs[i].contains("stock") == outputs[i].contains("population"))
            throw Error("IR_OUTPUT", pointer, "output needs exactly one stock or population");
        if (outputs[i].contains("stock")) {
            if (outputs[i].contains("metric"))
                throw Error("IR_OUTPUT", pointer + "/metric", "stock output has no metric");
            const auto source = string_field(outputs[i], "stock", pointer);
            if (std::none_of(model.stocks.begin(), model.stocks.end(),
                    [&source](const Stock& item) { return item.id == source; }))
                throw Error("IR_REF", pointer + "/stock", "unknown SD stock");
            model.outputs.push_back(Output{id, source, false, ""});
        } else {
            const auto source = string_field(outputs[i], "population", pointer);
            const auto metric = string_field(outputs[i], "metric", pointer);
            if (source != population_id || (metric != "active" && metric != "adopted"))
                throw Error("IR_OUTPUT", pointer + "/metric", "unsupported population metric");
            model.outputs.push_back(Output{id, source, false, metric});
        }
    }
    return model;
}

Model load_agent_pool(const Json& document) {
    const bool coupled = document.at("mode") == "agent_pool_sd";
    if (coupled)
        reject_unknown(document, {"ir_version", "name", "mode", "time", "agent_pool", "sd", "bridges", "outputs"}, "");
    else reject_unknown(document, {"ir_version", "name", "mode", "time", "agent_pool", "outputs"}, "");
    if (string_field(document, "ir_version", "") != "0.1")
        throw Error("IR_VERSION", "/ir_version", "only IR version 0.1 is supported");
    Model model;
    model.kind = Model::Kind::agent_pool;
    model.name = string_field(document, "name", "");
    const auto& time = field(document, "time", "");
    reject_unknown(time, {"unit", "dt", "horizon"}, "/time");
    model.time_unit = string_field(time, "unit", "/time");
    (void)unit_field(time, "unit", "/time");
    model.dt = number_field(time, "dt", "/time");
    model.horizon = number_field(time, "horizon", "/time");
    const double ratio = model.horizon / model.dt;
    if (model.dt <= 0 || model.horizon <= 0 || !std::isfinite(ratio) || ratio > 1000000 ||
        std::abs(ratio - std::round(ratio)) > 1e-9)
        throw Error("IR_TIME", "/time", "horizon must be a positive integer number of dt steps");

    const auto& section = field(document, "agent_pool", "");
    reject_unknown(section, {"population", "pool", "bridge", "schedule", "phases", "delivery"}, "/agent_pool");
    const auto& population = field(section, "population", "/agent_pool");
    const auto& pool = field(section, "pool", "/agent_pool");
    const auto& bridge = field(section, "bridge", "/agent_pool");
    reject_unknown(population, {"id", "unit", "agents"}, "/agent_pool/population");
    reject_unknown(pool, {"id", "unit", "max_request_units"}, "/agent_pool/pool");
    reject_unknown(bridge, {"kind", "from", "to", "unit"}, "/agent_pool/bridge");
    std::set<std::string> ids;
    if (coupled) {
        const auto& sd_section = field(document, "sd", "");
        reject_unknown(sd_section, {"parameters", "components"}, "/sd");
        const auto& components = field(sd_section, "components", "/sd");
        if (!components.is_array()) throw Error("IR_TYPE", "/sd/components", "expected an array");
        if (sd_section.contains("parameters")) {
            const auto& parameters = sd_section.at("parameters");
            if (!parameters.is_array()) throw Error("IR_TYPE", "/sd/parameters", "expected an array");
            for (std::size_t i = 0; i < parameters.size(); ++i)
                (void)id_field(parameters[i], "/sd/parameters/" + std::to_string(i), ids);
        }
        std::string first_stock;
        for (std::size_t i = 0; i < components.size(); ++i) {
            const auto pointer = "/sd/components/" + std::to_string(i);
            const auto id = id_field(components[i], pointer, ids);
            const auto kind = string_field(components[i], "kind", pointer);
            if (kind != "stock" && kind != "flow")
                throw Error("IR_KIND", pointer + "/kind", "agent-pool SD supports stock and flow");
            if (kind == "stock" && first_stock.empty()) first_stock = id;
        }
        if (first_stock.empty()) throw Error("IR_STOCK", "/sd/components", "an SD stock is required");
        Json nested = {{"ir_version", "0.1"}, {"name", model.name}, {"mode", "sd"},
            {"time", time}, {"components", components},
            {"outputs", Json::array({Json{{"id", "__delivery_stock"}, {"stock", first_stock}}})}};
        if (sd_section.contains("parameters")) nested["parameters"] = sd_section.at("parameters");
        try { model = load_document(nested); }
        catch (const Error& error) { throw Error(error.code, "/sd" + error.pointer, error.what()); }
        model.kind = Model::Kind::agent_pool_sd;
        model.outputs.clear();
        for (const auto& flow : model.flows)
            if (flow.expression.symbols().contains("dt"))
                throw Error("IR_HYBRID", "/sd/components/" + std::to_string(flow.component_index) + "/expr",
                            "hybrid flow cannot depend on dt because off-grid events split steps");
    }
    AgentPoolSpec spec;
    spec.population_id = id_field(population, "/agent_pool/population", ids);
    spec.pool_id = id_field(pool, "/agent_pool/pool", ids);
    spec.unit = string_field(pool, "unit", "/agent_pool/pool");
    const auto pool_unit = unit_field(pool, "unit", "/agent_pool/pool");
    if (!(unit_field(population, "unit", "/agent_pool/population") == pool_unit) ||
        !(unit_field(bridge, "unit", "/agent_pool/bridge") == pool_unit))
        throw Error("IR_UNIT", "/agent_pool/bridge/unit", "population, pool, and bridge units must match");
    if (string_field(bridge, "kind", "/agent_pool/bridge") != "agent_pool" ||
        string_field(bridge, "from", "/agent_pool/bridge") != spec.population_id ||
        string_field(bridge, "to", "/agent_pool/bridge") != spec.pool_id)
        throw Error("IR_REF", "/agent_pool/bridge", "agent-pool bridge endpoints or kind are invalid");
    spec.max_request_units = static_cast<std::size_t>(unsigned_field(
        pool, "max_request_units", "/agent_pool/pool", 1000000));
    if (spec.max_request_units == 0)
        throw Error("IR_AGENT_POOL", "/agent_pool/pool/max_request_units", "request limit must be positive");
    const auto& agents = field(population, "agents", "/agent_pool/population");
    if (!agents.is_array() || agents.size() > 1000000)
        throw Error("IR_TYPE", "/agent_pool/population/agents", "agents must be an array of at most one million capacities");
    for (std::size_t i = 0; i < agents.size(); ++i) {
        if (!agents[i].is_number_unsigned() || agents[i].get<std::uint64_t>() > 1000000)
            throw Error("IR_TYPE", "/agent_pool/population/agents/" + std::to_string(i),
                        "agent capacity must be an unsigned integer at most one million");
        spec.initial_capacities.push_back(agents[i].get<std::size_t>());
    }

    if (section.contains("delivery")) {
        const auto& delivery = section.at("delivery");
        reject_unknown(delivery, {"id", "time_unit"}, "/agent_pool/delivery");
        spec.delivery_id = id_field(delivery, "/agent_pool/delivery", ids);
        if (!(unit_field(delivery, "time_unit", "/agent_pool/delivery") ==
              unit_field(time, "unit", "/time")))
            throw Error("IR_UNIT", "/agent_pool/delivery/time_unit", "delivery duration must use model time units");
    }

    if (coupled) {
        if (!spec.delivery_id)
            throw Error("IR_DELIVERY", "/agent_pool/delivery", "agent-pool SD requires delivery");
        const auto& bridges = field(document, "bridges", "");
        if (!bridges.is_array() || bridges.size() != 1)
            throw Error("IR_HYBRID", "/bridges", "agent-pool SD requires one completion bridge");
        const auto& item = bridges[0];
        const std::string pointer = "/bridges/0";
        reject_unknown(item, {"kind", "from", "to", "amount", "unit"}, pointer);
        if (string_field(item, "kind", pointer) != "completion_to_stock")
            throw Error("IR_HYBRID", pointer + "/kind", "expected completion_to_stock");
        const auto from = string_field(item, "from", pointer);
        const auto to = string_field(item, "to", pointer);
        if (from != *spec.delivery_id)
            throw Error("IR_REF", pointer + "/from", "completion source must be delivery");
        const auto stock = std::find_if(model.stocks.begin(), model.stocks.end(),
            [&](const Stock& value) { return value.id == to; });
        if (stock == model.stocks.end()) throw Error("IR_REF", pointer + "/to", "unknown target stock");
        const double amount = number_field(item, "amount", pointer);
        if (amount <= 0) throw Error("IR_HYBRID", pointer + "/amount", "amount must be positive");
        if (!(unit_field(item, "unit", pointer) == Dimension::parse(stock->unit)))
            throw Error("IR_UNIT", pointer + "/unit", "pulse amount must have target stock units");
        model.completion_bridge = CompletionBridge{from, to, amount};
    }

    std::map<std::string, std::size_t> phase_indices;
    if (section.contains("phases")) {
        const auto& phases = section.at("phases");
        if (!phases.is_array() || phases.size() > 1000000)
            throw Error("IR_TYPE", "/agent_pool/phases", "expected at most one million phases");
        const std::map<std::string, Dimension> symbols{
            {"capacity", pool_unit}, {"allocated", pool_unit},
            {"total_capacity", pool_unit}, {"total_allocated", pool_unit},
            {"active", Dimension{}}, {"agent_id", Dimension{}},
            {"t", unit_field(time, "unit", "/time")}};
        for (std::size_t i = 0; i < phases.size(); ++i) {
            const auto pointer = "/agent_pool/phases/" + std::to_string(i);
            const auto& phase = phases[i];
            reject_unknown(phase, {"id", "capacity_expr", "unit"}, pointer);
            const auto id = id_field(phase, pointer, ids);
            if (!(unit_field(phase, "unit", pointer) == pool_unit))
                throw Error("IR_UNIT", pointer + "/unit", "phase must produce pool capacity units");
            const auto source = string_field(phase, "capacity_expr", pointer);
            try { spec.phases.push_back({id, Expression(source)}); }
            catch (const std::invalid_argument& error) {
                throw Error("IR_EXPR", pointer + "/capacity_expr", error.what());
            }
            if (!(expression_unit(spec.phases.back().capacity_expression, symbols, {},
                                  pointer + "/capacity_expr") == pool_unit))
                throw Error("IR_UNIT", pointer + "/capacity_expr", "expression must produce capacity units");
            phase_indices.emplace(id, i);
        }
    }

    const auto& schedule = field(section, "schedule", "/agent_pool");
    if (!schedule.is_array() || schedule.size() > 1000000)
        throw Error("IR_TYPE", "/agent_pool/schedule", "schedule must be an array of at most one million changes");
    std::set<std::uint64_t> active_agents;
    for (std::uint64_t id = 0; id < spec.initial_capacities.size(); ++id) active_agents.insert(id);
    std::uint64_t next_agent_id = spec.initial_capacities.size();
    std::set<std::uint64_t> request_ids;
    double previous_time = 0;
    for (std::size_t i = 0; i < schedule.size(); ++i) {
        const auto pointer = "/agent_pool/schedule/" + std::to_string(i);
        const auto& item = schedule[i];
        reject_unknown(item, {"time", "releases", "updates", "departures", "hires", "requests", "phases", "engagements"}, pointer);
        if (spec.delivery_id && (item.contains("requests") || item.contains("releases")))
            throw Error("IR_DELIVERY", pointer, "delivery owns staffing requests and releases");
        if (!spec.delivery_id && item.contains("engagements"))
            throw Error("IR_DELIVERY", pointer + "/engagements", "engagements require delivery configuration");
        AgentPoolChange change;
        change.time = number_field(item, "time", pointer);
        if (change.time < previous_time || change.time < 0 || change.time > model.horizon)
            throw Error("IR_AGENT_POOL", pointer + "/time", "change time must be ordered and within horizon");
        if (spec.delivery_id && i > 0 && change.time == previous_time)
            throw Error("IR_DELIVERY", pointer + "/time", "combine same-time delivery actions into one entry");
        previous_time = change.time;
        const auto read_array = [&](const char* key) -> Json {
            if (!item.contains(key)) return Json::array();
            if (!item.at(key).is_array())
                throw Error("IR_TYPE", pointer + "/" + key, "expected an array");
            return item.at(key);
        };
        const auto releases = read_array("releases");
        for (std::size_t j = 0; j < releases.size(); ++j) {
            const auto path = pointer + "/releases/" + std::to_string(j);
            const auto id = unsigned_field(releases[j], "request_id", path,
                                           std::numeric_limits<std::uint64_t>::max());
            reject_unknown(releases[j], {"request_id"}, path);
            if (!request_ids.contains(id))
                throw Error("IR_REF", path + "/request_id", "release must refer to an earlier request");
            change.releases.push_back({id});
        }
        const auto updates = read_array("updates");
        std::set<std::uint64_t> updated;
        for (std::size_t j = 0; j < updates.size(); ++j) {
            const auto path = pointer + "/updates/" + std::to_string(j);
            reject_unknown(updates[j], {"agent_id", "capacity"}, path);
            const auto id = unsigned_field(updates[j], "agent_id", path, next_agent_id);
            const auto capacity = unsigned_field(updates[j], "capacity", path, 1000000);
            if (!active_agents.contains(id) || !updated.insert(id).second)
                throw Error("IR_REF", path + "/agent_id", "update needs a distinct active agent");
            change.updates.emplace_back(id, static_cast<std::size_t>(capacity));
        }
        const auto departures = read_array("departures");
        for (std::size_t j = 0; j < departures.size(); ++j) {
            const auto path = pointer + "/departures/" + std::to_string(j);
            if (!departures[j].is_number_unsigned())
                throw Error("IR_TYPE", path, "departure agent ID must be unsigned");
            const auto id = departures[j].get<std::uint64_t>();
            if (!active_agents.erase(id))
                throw Error("IR_REF", path, "departure needs an active agent");
            change.departures.push_back(id);
        }
        const auto hires = read_array("hires");
        for (std::size_t j = 0; j < hires.size(); ++j) {
            const auto path = pointer + "/hires/" + std::to_string(j);
            if (!hires[j].is_number_unsigned() || hires[j].get<std::uint64_t>() > 1000000 ||
                next_agent_id >= 1000000)
                throw Error("IR_TYPE", path, "hire capacity or population limit is invalid");
            change.hires.push_back(hires[j].get<std::size_t>());
            active_agents.insert(next_agent_id++);
        }
        const auto requests = read_array("requests");
        for (std::size_t j = 0; j < requests.size(); ++j) {
            const auto path = pointer + "/requests/" + std::to_string(j);
            reject_unknown(requests[j], {"request_id", "units"}, path);
            const auto id = unsigned_field(requests[j], "request_id", path,
                                           std::numeric_limits<std::uint64_t>::max());
            const auto units = unsigned_field(requests[j], "units", path, spec.max_request_units);
            if (units == 0 || !request_ids.insert(id).second)
                throw Error("IR_AGENT_POOL", path, "request must have positive units and a unique ID");
            change.requests.push_back({id, static_cast<std::size_t>(units)});
        }
        const auto engagements = read_array("engagements");
        for (std::size_t j = 0; j < engagements.size(); ++j) {
            const auto path = pointer + "/engagements/" + std::to_string(j);
            const auto& engagement = engagements[j];
            reject_unknown(engagement, {"request_id", "units", "duration"}, path);
            const auto id = unsigned_field(engagement, "request_id", path,
                                           std::numeric_limits<std::uint64_t>::max());
            const auto units = unsigned_field(engagement, "units", path, spec.max_request_units);
            const double duration = number_field(engagement, "duration", path);
            if (duration <= 0) throw Error("IR_DELIVERY", path + "/duration", "duration must be positive");
            if (units == 0 || !request_ids.insert(id).second)
                throw Error("IR_DELIVERY", path, "engagement needs positive units and a unique ID");
            change.engagements.push_back({id, static_cast<std::size_t>(units), duration});
        }
        const auto phases = read_array("phases");
        for (std::size_t j = 0; j < phases.size(); ++j) {
            const auto path = pointer + "/phases/" + std::to_string(j);
            if (!phases[j].is_string()) throw Error("IR_TYPE", path, "expected a phase ID");
            const auto found = phase_indices.find(phases[j].get<std::string>());
            if (found == phase_indices.end()) throw Error("IR_REF", path, "unknown phase ID");
            change.phases.push_back(found->second);
        }
        if (change.releases.empty() && change.updates.empty() && change.departures.empty() &&
            change.hires.empty() && change.requests.empty() && change.phases.empty() && change.engagements.empty())
            throw Error("IR_AGENT_POOL", pointer, "scheduled change has no actions");
        spec.schedule.push_back(std::move(change));
    }

    const auto& outputs = field(document, "outputs", "");
    if (!outputs.is_array() || outputs.empty())
        throw Error("IR_TYPE", "/outputs", "outputs must be a nonempty array");
    std::set<std::string> output_ids;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto pointer = "/outputs/" + std::to_string(i);
        const auto& output = outputs[i];
        if (coupled && output.contains("stock")) {
            reject_unknown(output, {"id", "stock"}, pointer);
            const auto id = id_field(output, pointer, output_ids);
            const auto source = string_field(output, "stock", pointer);
            const auto stock = std::find_if(model.stocks.begin(), model.stocks.end(),
                [&](const Stock& value) { return value.id == source; });
            if (stock == model.stocks.end()) throw Error("IR_OUTPUT", pointer + "/stock", "unknown stock");
            spec.outputs.push_back({AgentPoolOutput::Source::stock, id, "",
                static_cast<std::uint64_t>(std::distance(model.stocks.begin(), stock))});
            continue;
        }
        reject_unknown(output, {"id", "pool", "population", "agent_id", "request_id", "process", "metric"}, pointer);
        AgentPoolOutput item;
        item.id = id_field(output, pointer, output_ids);
        item.metric = string_field(output, "metric", pointer);
        const int sources = static_cast<int>(output.contains("pool")) +
                            static_cast<int>(output.contains("population")) +
                            static_cast<int>(output.contains("agent_id")) +
                            static_cast<int>(output.contains("request_id")) +
                            static_cast<int>(output.contains("process"));
        if (sources != 1)
            throw Error("IR_OUTPUT", pointer, "output needs exactly one source");
        if (output.contains("pool")) {
            item.source = AgentPoolOutput::Source::pool;
            if (string_field(output, "pool", pointer) != spec.pool_id ||
                (item.metric != "capacity" && item.metric != "available" &&
                 item.metric != "allocated_units" && item.metric != "allocations" &&
                 item.metric != "waiting"))
                throw Error("IR_OUTPUT", pointer, "unsupported pool output");
        } else if (output.contains("population")) {
            item.source = AgentPoolOutput::Source::population;
            if (string_field(output, "population", pointer) != spec.population_id ||
                item.metric != "active")
                throw Error("IR_OUTPUT", pointer, "unsupported population output");
        } else if (output.contains("agent_id")) {
            item.source = AgentPoolOutput::Source::agent;
            item.reference = unsigned_field(output, "agent_id", pointer, next_agent_id);
            if (item.reference >= next_agent_id ||
                (item.metric != "allocated" && item.metric != "alive" && item.metric != "capacity"))
                throw Error("IR_OUTPUT", pointer, "unsupported agent output");
        } else if (output.contains("process")) {
            item.source = AgentPoolOutput::Source::process;
            if (!spec.delivery_id || string_field(output, "process", pointer) != *spec.delivery_id ||
                (item.metric != "accepted" && item.metric != "started" && item.metric != "completed" &&
                 item.metric != "in_service" && item.metric != "waiting" &&
                 item.metric != "wait_total" && item.metric != "cycle_total" &&
                 item.metric != "capacity_time" && item.metric != "allocated_time" &&
                 item.metric != "available_time" && item.metric != "queue_time" &&
                 item.metric != "service_time" && item.metric != "population_time" &&
                 item.metric != "utilization" && item.metric != "mean_queue" &&
                 item.metric != "mean_in_service" && item.metric != "mean_headcount"))
                throw Error("IR_OUTPUT", pointer, "unsupported delivery output");
        } else {
            item.source = AgentPoolOutput::Source::request;
            item.reference = unsigned_field(output, "request_id", pointer,
                                            std::numeric_limits<std::uint64_t>::max());
            if (!request_ids.contains(item.reference) ||
                (item.metric != "granted" && !(spec.delivery_id &&
                    (item.metric == "in_service" || item.metric == "completed"))))
                throw Error("IR_OUTPUT", pointer, "unsupported request output");
        }
        spec.outputs.push_back(std::move(item));
    }
    model.agent_pool = std::move(spec);
    return model;
}

Model load_document(const Json& document, const std::filesystem::path* base) {
    if (!document.is_object()) throw Error("IR_TYPE", "", "top-level IR must be an object");
    if (document.contains("checks") && document.contains("mode") && document.at("mode") != "sd")
        throw Error("IR_CHECK_SCOPE", "/checks", "declared checks currently require standalone SD");
    if (document.contains("data") && ((!base) ||
        (document.contains("mode") && document.at("mode") != "sd" && document.at("mode") != "abm")))
        throw Error("IR_DATA", "/data", "data bindings require a standalone SD or typed ABM model file");
    if (document.contains("mode") && document.at("mode") != "sd" && document.contains("time") &&
        document.at("time").is_object() && document.at("time").contains("start"))
        throw Error("IR_FIELD", "/time/start", "start time is standalone SD only");
    if (document.contains("mode") && document.at("mode") != "sd" && document.contains("sd") &&
        document.at("sd").is_object() && document.at("sd").contains("components") &&
        document.at("sd").at("components").is_array()) {
        const auto& items=document.at("sd").at("components");
        for (std::size_t i=0; i<items.size(); ++i)
            for (const auto* key : {"clip_outflows", "outflow_order", "clip_negative"})
                if (items[i].contains(key))
                    throw Error("IR_FIELD", "/sd/components/"+std::to_string(i)+"/"+key,
                                "clipping is standalone SD only");
    }
    if (document.contains("mode") && document.at("mode") == "hybrid")
        return load_hybrid(document);
    if (document.contains("mode") && document.at("mode") == "abm_sd")
        return load_abm_sd(document);
    if (document.contains("mode") && (document.at("mode") == "agent_pool" || document.at("mode") == "agent_pool_sd"))
        return load_agent_pool(document);
    reject_unknown(document, {"ir_version", "name", "mode", "time", "parameters", "components", "links", "outputs", "integrator", "data", "checks"}, "");
    if (string_field(document, "ir_version", "") != "0.1")
        throw Error("IR_VERSION", "/ir_version", "only IR version 0.1 is supported");
    Model model;
    model.name = string_field(document, "name", "");
    if (document.contains("mode")) {
        const auto mode = string_field(document, "mode", "");
        if (mode == "des") model.kind = Model::Kind::des;
        else if (mode == "abm") model.kind = Model::Kind::abm;
        else if (mode == "agent_stock_sd") model.kind = Model::Kind::agent_stock_sd;
        else if (mode != "sd") throw Error("IR_MODE", "/mode", "unknown simulation mode");
    }
    if (document.contains("integrator")) {
        if (model.kind != Model::Kind::sd)
            throw Error("IR_FIELD", "/integrator", "integrator selection is standalone SD only");
        const auto method = string_field(document, "integrator", "");
        if (method == "rk4") model.integrator = sd::Integrator::rk4;
        else if (method != "euler")
            throw Error("IR_INTEGRATOR", "/integrator", "expected euler or rk4");
    }
    const auto& time = field(document, "time", "");
    if (!time.is_object()) throw Error("IR_TYPE", "/time", "time must be an object");
    reject_unknown(time, {"unit", "dt", "horizon", "start"}, "/time");
    model.time_unit = string_field(time, "unit", "/time");
    const Dimension time_dimension = unit_field(time, "unit", "/time");
    model.dt = number_field(time, "dt", "/time");
    model.horizon = number_field(time, "horizon", "/time");
    model.start = time.contains("start") ? number_field(time, "start", "/time") : 0.;
    if (model.dt <= 0 || model.horizon <= 0)
        throw Error("IR_TIME", "/time", "dt and horizon must be positive");
    const double ratio = model.horizon / model.dt;
    if (!std::isfinite(ratio) || ratio < 1 || ratio > 1000000 ||
        std::abs(ratio - std::round(ratio)) > 1e-9)
        throw Error("IR_TIME", "/time", "horizon must be an integer number of dt steps, at most one million");

    if (model.start != 0) {
        double previous = model.start;
        for (std::size_t i = 1; i <= static_cast<std::size_t>(std::llround(ratio)); ++i) {
            const double current = model.start + i * model.dt;
            if (!std::isfinite(current) || current <= previous ||
                !std::isfinite(previous + model.dt) ||
                (model.integrator == sd::Integrator::rk4 &&
                 (previous + model.dt/2 <= previous || previous + model.dt/2 >= current)))
                throw Error("IR_TIME", "/time/start", "time grid must be finite and distinguishable at every step and RK4 stage");
            previous = current;
        }
    }

    std::set<std::string> ids;
    if (document.contains("parameters")) {
        const auto& parameters = document.at("parameters");
        if (!parameters.is_array()) throw Error("IR_TYPE", "/parameters", "parameters must be an array");
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            const auto pointer = "/parameters/" + std::to_string(i);
            reject_unknown(parameters[i], {"id", "unit", "value"}, pointer);
            const auto id = id_field(parameters[i], pointer, ids);
            model.parameter_units.emplace(id, unit_field(parameters[i], "unit", pointer));
            model.parameters.emplace(id, number_field(parameters[i], "value", pointer));
        }
    }

    const auto population_binding = document.contains("data")
        ? resolve_model_data(document.at("data"), *base, model) : std::nullopt;
    for (const auto& series : model.series_data)
        (void)id_field(document.at("data").at(series.binding_index),
                       "/data/"+std::to_string(series.binding_index),ids);
    if (model.kind == Model::Kind::abm) { parse_typed_abm(document,model,ids,base,population_binding); return model; }
    if (model.kind == Model::Kind::agent_stock_sd) { parse_agent_stock_sd(document,model,ids); return model; }
    if (model.kind == Model::Kind::des) {
        const auto& components=field(document,"components","");
        const bool typed=components.is_array() && std::any_of(components.begin(),components.end(),[](const Json& item) {
            return item.is_object() && item.contains("kind") && item.at("kind")=="entity_type";
        });
        if(typed) parse_typed_des(document,model,ids);
        else parse_des(document, model, ids);
        return model;
    }
    if (document.contains("links"))
        throw Error("IR_FIELD", "/links", "SD subset has no process links");

    const auto& components = field(document, "components", "");
    if (!components.is_array())
        throw Error("IR_TYPE", "/components", "components must be an array");
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto pointer = "/components/" + std::to_string(i);
        const auto& item = components[i];
        const auto id = id_field(item, pointer, ids);
        const auto kind = string_field(item, "kind", pointer);
        if (kind == "stock")
            reject_unknown(item, {"id", "kind", "unit", "init", "non_negative", "clip_outflows", "outflow_order"}, pointer);
        else if (kind == "flow")
            reject_unknown(item, {"id", "kind", "unit", "source", "destination", "expr", "non_negative", "clip_negative"}, pointer);
        else if (kind == "aux")
            reject_unknown(item, {"id", "kind", "unit", "expr"}, pointer);
        else if (kind == "table")
            reject_unknown(item, {"id", "kind", "unit", "x_unit", "x", "y", "extrapolate"}, pointer);
        else if (kind == "delay")
            reject_unknown(item, {"id", "kind", "unit", "type", "order", "duration", "initial", "input"}, pointer);
        const auto unit = string_field(item, "unit", pointer);
        (void)unit_field(item, "unit", pointer);
        if (kind == "stock") {
            bool nonnegative = true;
            if (item.contains("non_negative")) {
                if (!item.at("non_negative").is_boolean())
                    throw Error("IR_TYPE", pointer + "/non_negative", "expected boolean");
                nonnegative = item.at("non_negative").get<bool>();
            }
            const double initial = number_field(item, "init", pointer);
            if (nonnegative && initial < 0)
                throw Error("IR_STOCK", pointer + "/init", "nonnegative stock cannot start below zero");
            if (item.contains("clip_outflows") && !item.at("clip_outflows").is_boolean())
                throw Error("IR_TYPE", pointer + "/clip_outflows", "expected boolean");
            const bool clip=item.value("clip_outflows",false);
            if (clip && (!nonnegative || model.integrator!=sd::Integrator::euler))
                throw Error("IR_CLIPPING", pointer + "/clip_outflows", "clipped stocks require Euler and non_negative true");
            std::vector<std::string> order;
            if (clip) {
                const auto& list=field(item,"outflow_order",pointer);
                if (!list.is_array()) throw Error("IR_TYPE",pointer+"/outflow_order","expected array");
                for (const auto& entry : list) {
                    if (!entry.is_string()) throw Error("IR_TYPE",pointer+"/outflow_order","expected flow IDs");
                    order.push_back(entry.get<std::string>());
                }
            } else if (item.contains("outflow_order")) {
                throw Error("IR_CLIPPING",pointer+"/outflow_order","outflow_order requires clip_outflows true");
            }
            model.stocks.push_back(Stock{id, initial, nonnegative, unit, clip, std::move(order)});
        } else if (kind == "flow") {
            const auto source = endpoint(item, "source", pointer);
            const auto destination = endpoint(item, "destination", pointer);
            if (!source && !destination)
                throw Error("IR_FLOW", pointer, "flow needs a source or destination stock");
            const auto source_text = string_field(item, "expr", pointer);
            if (item.contains("non_negative") && !item.at("non_negative").is_boolean())
                throw Error("IR_TYPE", pointer + "/non_negative", "expected boolean");
            if (item.contains("clip_negative") && !item.at("clip_negative").is_boolean())
                throw Error("IR_TYPE", pointer + "/clip_negative", "expected boolean");
            const bool clip=item.value("clip_negative",false);
            if (clip && (!item.value("non_negative",true) || model.integrator!=sd::Integrator::euler))
                throw Error("IR_CLIPPING",pointer+"/clip_negative","clipped flows require Euler and non_negative true");
            try { model.flows.push_back(Flow{id, i, source, destination, Expression(source_text), unit,
                                             item.value("non_negative", true), clip}); }
            catch (const std::invalid_argument& error) {
                throw Error("IR_EXPR", pointer + "/expr", error.what());
            }
        } else if (kind == "aux") {
            try { model.auxiliaries.push_back({id,i,Expression(string_field(item,"expr",pointer)),unit}); }
            catch (const std::invalid_argument& error) { throw Error("IR_EXPR",pointer+"/expr",error.what()); }
        } else if (kind == "table") {
            const Dimension input_unit = unit_field(item, "x_unit", pointer);
            const Dimension output_unit = unit_field(item, "unit", pointer);
            const auto policy_text = string_field(item, "extrapolate", pointer);
            if (policy_text != "clamp" && policy_text != "linear")
                throw Error("IR_TABLE", pointer + "/extrapolate", "expected clamp or linear");
            const auto x = number_array_field(item, "x", pointer);
            const auto y = number_array_field(item, "y", pointer);
            try {
                model.tables.push_back(Table{id, input_unit, output_unit,
                    sd::LookupTable(x, y, policy_text == "clamp" ? sd::Extrapolation::clamp
                                                                 : sd::Extrapolation::linear)});
            } catch (const std::invalid_argument& error) {
                throw Error("IR_TABLE", pointer, error.what());
            }
        } else if (kind == "delay") {
            if (model.integrator != sd::Integrator::euler)
                throw Error("IR_INTEGRATOR", pointer + "/kind", "delay components require Euler integration");
            const auto type = string_field(item, "type", pointer);
            if (type != "material" && type != "information" && type != "fixed" && type != "history2")
                throw Error("IR_DELAY", pointer + "/type", "expected material, information, fixed, or history2");
            const auto& order_value = field(item, "order", pointer);
            if (!order_value.is_number_unsigned() ||
                order_value.get<std::uint64_t>() < 1 || order_value.get<std::uint64_t>() > sd::max_delay_order ||
                (type == "fixed" && order_value.get<std::uint64_t>() != 1) ||
                (type == "history2" && order_value.get<std::uint64_t>() != 2))
                throw Error("IR_DELAY", pointer + "/order", "delay order must be 1..255; fixed requires 1, history2 requires 2");
            const auto order = order_value.get<std::size_t>();
            std::variant<double, Expression> duration;
            if (field(item, "duration", pointer).is_string()) {
                try { duration = Expression(string_field(item, "duration", pointer)); }
                catch (const std::invalid_argument& error) {
                    throw Error("IR_EXPR", pointer + "/duration", error.what());
                }
            } else {
                const double value = number_field(item, "duration", pointer);
                if (value <= 0 || model.dt > value / static_cast<double>(order))
                    throw Error("IR_DELAY", pointer + "/duration", "duration must cover at least one step per stage");
                duration = value;
            }
            const double initial = number_field(item, "initial", pointer);
            const auto delay_type = type == "material" ? sd::DelayKind::material
                : type == "history2" ? sd::DelayKind::history2
                : type == "fixed" ? sd::DelayKind::fixed : sd::DelayKind::information;
            const auto input = string_field(item, "input", pointer);
            try { model.delays.push_back(Delay{id, i, delay_type, order, duration,
                                               initial, Expression(input), unit}); }
            catch (const std::invalid_argument& error) {
                throw Error("IR_EXPR", pointer + "/input", error.what());
            }
        } else {
            throw Error("IR_KIND", pointer + "/kind", "this IR subset supports stock, flow, aux, table, and delay components");
        }
    }

    std::set<std::string> stock_ids;
    for (const auto& stock : model.stocks) stock_ids.insert(stock.id);
    std::map<std::string, Dimension> symbol_units = model.parameter_units;
    symbol_units.emplace("t", time_dimension);
    symbol_units.emplace("dt", time_dimension);
    for (const auto& stock : model.stocks)
        symbol_units.emplace(stock.id, Dimension::parse(stock.unit));
    std::map<std::string,std::size_t> auxiliary_index;
    for (std::size_t i=0;i<model.auxiliaries.size();++i) {
        const auto& a=model.auxiliaries[i];
        auxiliary_index.emplace(a.id,i);
        symbol_units.emplace(a.id,Dimension::parse(a.unit));
    }
    std::set<std::string> delay_ids;
    for (const auto& delay : model.delays) {
        symbol_units.emplace(delay.id, Dimension::parse(delay.unit));
        delay_ids.insert(delay.id);
    }
    std::map<std::string, std::pair<Dimension, Dimension>> function_units;
    for (const auto& table : model.tables)
        function_units.emplace(table.id, std::make_pair(table.input_unit, table.output_unit));
    for (const auto& series : model.series_data)
        function_units.emplace(series.id, std::make_pair(series.input_unit,series.output_unit));
    std::map<std::string,std::set<std::string>> dependents;
    std::map<std::string,std::size_t> pending;
    std::set<std::string> ready;
    for (const auto& a:model.auxiliaries) {
        const auto pointer="/components/"+std::to_string(a.component_index);
        if (!(expression_unit(a.expression,symbol_units,function_units,pointer+"/expr")==Dimension::parse(a.unit)))
            throw Error("IR_UNIT",pointer+"/unit","auxiliary unit does not match its expression");
        auto& count=pending[a.id];
        for (const auto& symbol:a.expression.symbols()) {
            if (delay_ids.contains(symbol)) throw Error("IR_AUX",pointer+"/expr","auxiliary delay-output dependencies are not supported");
            if (auxiliary_index.contains(symbol)) { ++count;dependents[symbol].insert(a.id); }
        }
        for (const auto& function:a.expression.functions())
            if (model.integrator==sd::Integrator::rk4 && Expression::tick_builtin(function))
                throw Error("IR_INTEGRATOR",pointer+"/expr","tick-input auxiliaries require Euler integration");
        if (!count) ready.insert(a.id);
    }
    std::vector<Auxiliary> ordered;
    while (!ready.empty()) {
        const auto name=*ready.begin();ready.erase(ready.begin());
        ordered.push_back(model.auxiliaries.at(auxiliary_index.at(name)));
        for (const auto& next:dependents[name]) if (--pending.at(next)==0) ready.insert(next);
    }
    if (ordered.size()!=model.auxiliaries.size()) {
        for (const auto& [name,count]:pending) if (count)
            throw Error("IR_AUX_CYCLE","/components/"+std::to_string(model.auxiliaries.at(auxiliary_index.at(name)).component_index)+"/expr","auxiliary dependency cycle");
    }
    model.auxiliaries=std::move(ordered);
    for (std::size_t i = 0; i < model.flows.size(); ++i) {
        const auto& flow = model.flows[i];
        const auto pointer = "/components/" + std::to_string(flow.component_index);
        if (model.integrator == sd::Integrator::rk4)
            for (const auto& function : flow.expression.functions())
                if (Expression::tick_builtin(function))
                    throw Error("IR_INTEGRATOR", pointer + "/expr",
                                "tick input functions driving flows require Euler integration");
        if ((flow.source && !stock_ids.contains(*flow.source)) ||
            (flow.destination && !stock_ids.contains(*flow.destination)))
            throw Error("IR_REF", pointer, "flow endpoint references an unknown stock");
        const Dimension expression_dimension = expression_unit(flow.expression, symbol_units,
            function_units, pointer + "/expr");
        const Dimension declared_unit = Dimension::parse(flow.unit);
        if (!(expression_dimension == declared_unit))
            throw Error("IR_UNIT", pointer + "/unit", "flow unit does not match its expression");
        if ((flow.source && !(declared_unit == symbol_units.at(*flow.source).divided(time_dimension))) ||
            (flow.destination && !(declared_unit == symbol_units.at(*flow.destination).divided(time_dimension))))
            throw Error("IR_UNIT", pointer + "/unit", "flow must have stock unit divided by time unit");
    }
    for (const auto& delay : model.delays) {
        const auto pointer = "/components/" + std::to_string(delay.component_index);
        if (const auto* expression = std::get_if<Expression>(&delay.duration)) {
            for (const auto& symbol : expression->symbols())
                if (delay_ids.contains(symbol))
                    throw Error("IR_DELAY_DURATION", pointer + "/duration",
                                "duration may read parameters, stocks, time, and tables, but not delay outputs");
            if (!(expression_unit(*expression, symbol_units, function_units, pointer + "/duration") == time_dimension))
                throw Error("IR_UNIT", pointer + "/duration", "delay duration expression must have time units");
        }
        const Dimension input_dimension = expression_unit(delay.input, symbol_units,
            function_units, pointer + "/input");
        if (!(input_dimension == symbol_units.at(delay.id)))
            throw Error("IR_UNIT", pointer + "/input", "delay input unit must equal its output unit");
    }
    // Use the kernel's graph validator at load time too, with inert rate functions.
    try {
        sd::Model validator;
        std::map<std::string,std::size_t> stocks, flows;
        for (const auto& stock : model.stocks)
            stocks.emplace(stock.id,validator.add_stock(stock.id,stock.initial,stock.non_negative,stock.clip_outflows));
        for (const auto& flow : model.flows)
            flows.emplace(flow.id,validator.add_flow(flow.source ? stocks.at(*flow.source) : sd::Model::boundary,
                flow.destination ? stocks.at(*flow.destination) : sd::Model::boundary,
                [](const auto&,double){return 0.;},flow.non_negative,flow.clip_negative));
        for (const auto& stock : model.stocks) if (stock.clip_outflows) {
            std::vector<std::size_t> order;
            for (const auto& id : stock.outflow_order) {
                if (!flows.contains(id)) throw std::invalid_argument("unknown flow in outflow_order");
                order.push_back(flows.at(id));
            }
            validator.set_outflow_order(stocks.at(stock.id),std::move(order));
        }
        validator.validate_clipping();
    } catch (const std::exception& error) { throw Error("IR_CLIPPING","/components",error.what()); }
    // Validate initial durations after all references and dimensions are known.
    // Runtime re-evaluates them with scenario overrides before initializing pipelines.
    auto initial_values = model.parameters;
    initial_values.emplace("t", model.start);
    initial_values.emplace("dt", model.dt);
    for (const auto& stock : model.stocks) initial_values.emplace(stock.id, stock.initial);
    std::map<std::string, std::function<double(double)>> initial_functions;
    for (const auto& table : model.tables) {
        const auto* lookup = &table.lookup;
        initial_functions.emplace(table.id, [lookup](double value) { return lookup->evaluate(value); });
    }
    for (const auto& binding : model.series_data) {
        const auto* series=&binding.series;
        initial_functions.emplace(binding.id,[series](double time) { return series->value_at(time); });
    }
    if (!model.delays.empty()) evaluate_auxiliaries(model,initial_values,initial_functions);
    for (const auto& delay : model.delays) {
        const auto pointer = "/components/" + std::to_string(delay.component_index);
        double duration = 0;
        try {
            duration = std::holds_alternative<double>(delay.duration) ? std::get<double>(delay.duration)
                : std::get<Expression>(delay.duration).evaluate(initial_values, initial_functions);
            validate_delay_duration(delay, duration, model.dt);
        } catch (const std::exception& error) { throw Error("IR_DELAY", pointer + "/duration", error.what()); }
        try { (void)make_delay(delay, duration, model.dt); }
        catch (const std::exception& error) { throw Error("IR_DELAY", pointer + "/initial", error.what()); }
    }

    const auto& outputs = field(document, "outputs", "");
    if (!outputs.is_array() || outputs.empty())
        throw Error("IR_TYPE", "/outputs", "outputs must be a nonempty array");
    std::set<std::string> output_ids;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto pointer = "/outputs/" + std::to_string(i);
        reject_unknown(outputs[i], {"id", "stock", "component", "expr", "unit"}, pointer);
        const auto id = id_field(outputs[i], pointer, output_ids);
        if (outputs[i].contains("stock") + outputs[i].contains("component") + outputs[i].contains("expr") != 1)
            throw Error("IR_OUTPUT", pointer, "output needs exactly one stock, component, or expr field");
        if (outputs[i].contains("expr")) {
            const auto text = string_field(outputs[i], "expr", pointer);
            std::optional<Expression> expression;
            try { expression.emplace(text); }
            catch (const std::invalid_argument& error) { throw Error("IR_EXPR", pointer + "/expr", error.what()); }
            const auto declared_unit = unit_field(outputs[i], "unit", pointer);
            if (!(expression_unit(*expression, symbol_units, function_units, pointer + "/expr") == declared_unit))
                throw Error("IR_UNIT", pointer + "/unit", "output unit does not match its expression");
            model.outputs.push_back(Output{id, "", false, "", std::move(expression)});
            continue;
        }
        if (outputs[i].contains("unit"))
            throw Error("IR_FIELD", pointer + "/unit", "unit is declared only for expression outputs");
        const bool component = outputs[i].contains("component");
        const auto source = string_field(outputs[i], component ? "component" : "stock", pointer);
        if (!stock_ids.contains(source) && (!component || !delay_ids.contains(source)))
            throw Error("IR_REF", pointer + (component ? "/component" : "/stock"),
                        "unknown output stock or delay");
        model.outputs.push_back(Output{id, source, delay_ids.contains(source), ""});
    }
    return model;
}

Model load_file(const std::string& path) {
    InputIdentity input;
    const auto document=provenance::read_input(path,input,"");
    const auto base = std::filesystem::absolute(path).parent_path();
    auto model=load_document(document, &base);
    validation::parse_checks(document,model);
    model.input=std::move(input);
    return model;
}
Model load_json(std::string_view json,const std::filesystem::path* base) {
    if(base && !base->is_absolute()) throw Error("IR_DATA","/data","JSON model base directory must be absolute");
    InputIdentity input;
    const auto document=provenance::parse_input(json,input,"");
    auto model=load_document(document,base);
    validation::parse_checks(document,model);
    model.input=std::move(input);
    return model;
}

std::vector<Row> run_des(const Model& spec, std::uint64_t seed,
                         std::uint32_t scenario, std::uint32_t replication) {
    if (!spec.process) throw Error("IR_DES_RUNTIME", "/components", "DES process is missing");
    const auto& process = *spec.process;
    std::vector<des::Entity> schedule = process.schedule;
    if (process.exponential) {
        const auto& generator = *process.exponential;
        try {
            schedule = des::exponential_schedule(generator.count, generator.arrival_rate,
                generator.service_rate, generator.start, seed,
                rng::DrawAddress{scenario, replication, generator.first_id, 0, generator.stream, 0});
        } catch (const std::exception& error) {
            throw Error("IR_DES_RUNTIME", "/components/" +
                std::to_string(process.source_component_index) + "/exponential", error.what());
        }
    }
    devs::Simulator<des::Entity> simulator;
    auto source = std::make_unique<des::ScheduledSource<>>(std::move(schedule));
    auto* source_ptr = source.get();
    const auto source_id = simulator.add(std::move(source));
    std::vector<des::MultiServer<>*> server_ptrs;
    std::map<std::string, std::size_t> nodes{{process.source_id, source_id}};
    for (const auto& stage : process.servers) {
        const bool route_rejections = std::any_of(process.links.begin(), process.links.end(),
            [&](const auto& link) { return link.from == stage.id && link.port == "rejected"; });
        auto server = std::make_unique<des::MultiServer<>>(stage.capacity, stage.service_scale,
            stage.queue, route_rejections, stage.service ? std::optional<des::ExponentialService>{
                {stage.service->rate, seed, scenario, replication, stage.service->stream}} : std::nullopt);
        server_ptrs.push_back(server.get());
        nodes.emplace(stage.id, simulator.add(std::move(server)));
    }
    auto sink = std::make_unique<des::CompletionSink<>>();
    auto* sink_ptr = sink.get();
    const auto sink_id = simulator.add(std::move(sink));
    nodes.emplace(process.sink_id, sink_id);
    std::map<std::string, des::PriorityRouter<>*> routers;
    std::map<std::string, des::DiscardSink<>*> discards;
    for (const auto& router : process.routers) {
        auto node = router.probability ? std::make_unique<des::PriorityRouter<>>(des::BernoulliRouting{
            router.probability->match, seed, scenario, replication, router.probability->stream}) :
            std::make_unique<des::PriorityRouter<>>(router.priority_at_most);
        routers.emplace(router.id, node.get());
        nodes.emplace(router.id, simulator.add(std::move(node)));
    }
    for (const auto& id : process.discards) {
        auto node = std::make_unique<des::DiscardSink<>>();
        discards.emplace(id, node.get());
        nodes.emplace(id, simulator.add(std::move(node)));
    }
    for (const auto& link : process.links) {
        const std::uint32_t port = link.from == process.source_id ? 0 :
            (link.port == "out" || link.port == "match" ? 1 : 2);
        simulator.connect(nodes.at(link.from), port, nodes.at(link.to), 0);
    }

    const auto steps = static_cast<std::size_t>(std::llround(spec.horizon / spec.dt));
    std::vector<Row> rows;
    rows.reserve((steps + 1) * spec.outputs.size());
    for (std::size_t step = 0; step <= steps; ++step) {
        const double time = step * spec.dt;
        (void)simulator.run_until(time);
        for (const auto& output : spec.outputs) {
            double value = 0;
            if (output.source == process.source_id) {
                value = static_cast<double>(source_ptr->emitted_count());
            } else if (output.source == process.sink_id) {
                if (output.metric == "completed") value = static_cast<double>(sink_ptr->completed_count());
                else if (output.metric == "cycle_total") value = sink_ptr->total_cycle_time();
                else if (output.metric == "cycle_mean")
                    value = sink_ptr->completed_count() == 0 ? 0 : sink_ptr->mean_cycle_time();
            } else if (routers.contains(output.source)) {
                const auto* router = routers.at(output.source);
                if (output.metric == "received") value = static_cast<double>(router->received_count());
                else if (output.metric == "matched") value = static_cast<double>(router->matched_count());
                else value = static_cast<double>(router->otherwise_count());
            } else if (discards.contains(output.source)) {
                value = static_cast<double>(discards.at(output.source)->received_count());
            } else {
                const auto stage = std::find_if(process.servers.begin(), process.servers.end(),
                    [&output](const DesServer& server) { return server.id == output.source; });
                const auto* server_ptr = server_ptrs.at(static_cast<std::size_t>(
                    std::distance(process.servers.begin(), stage)));
                if (output.metric == "accepted") value = static_cast<double>(server_ptr->accepted_count());
                else if (output.metric == "rejected") value = static_cast<double>(server_ptr->rejected_count());
                else if (output.metric == "completed") value = static_cast<double>(server_ptr->completed_count());
                else if (output.metric == "waiting") value = static_cast<double>(server_ptr->waiting());
                else if (output.metric == "busy") value = static_cast<double>(server_ptr->busy_servers());
                else if (output.metric == "queue_mean")
                    value = time == 0 ? 0 : server_ptr->mean_queue_length(time);
                else if (output.metric == "utilization")
                    value = time == 0 ? 0 : server_ptr->utilization(time);
            }
            rows.push_back(Row{time, output.id, value});
        }
    }
    return rows;
}

std::vector<Row> run_hybrid(const Model& spec,
                            const std::map<std::string, double>& parameters,
                            std::uint64_t seed, std::uint32_t scenario,
                            std::uint32_t replication) {
    if (!spec.process || !spec.completion_bridge)
        throw Error("IR_HYBRID_RUNTIME", "", "hybrid model is incomplete");
    const auto& process = *spec.process;
    if (!process.routers.empty() || !process.discards.empty() ||
        std::any_of(process.links.begin(), process.links.end(),
                    [](const auto& link) { return link.port != "out"; }))
        throw Error("IR_HYBRID_RUNTIME", "/des/links", "hybrid subset currently requires a linear process");
    std::vector<des::Entity> schedule = process.schedule;
    if (process.exponential) {
        const auto& generator = *process.exponential;
        try {
            schedule = des::exponential_schedule(generator.count, generator.arrival_rate,
                generator.service_rate, generator.start, seed,
                rng::DrawAddress{scenario, replication, generator.first_id, 0, generator.stream, 0});
        } catch (const std::exception& error) {
            throw Error("IR_HYBRID_RUNTIME", "/des/components/" +
                std::to_string(process.source_component_index) + "/exponential", error.what());
        }
    }

    sd::Model stocks;
    std::map<std::string, std::size_t> stock_index;
    for (const auto& stock : spec.stocks)
        stock_index.emplace(stock.id, stocks.add_stock(stock.id, stock.initial, stock.non_negative));
    for (const auto& flow : spec.flows) {
        const auto source = flow.source ? stock_index.at(*flow.source) : sd::Model::boundary;
        const auto destination = flow.destination ? stock_index.at(*flow.destination) : sd::Model::boundary;
        const Expression* expression = &flow.expression;
        stocks.add_flow(source, destination,
            [stock_index, parameters, expression](const auto& state, double time) {
                auto values = parameters;
                values.emplace("t", time);
                for (const auto& [id, index] : stock_index) values.emplace(id, state[index]);
                return expression->evaluate(values);
            }, flow.non_negative);
    }

    using Message = std::variant<des::Entity, double>;
    devs::Simulator<Message> simulator;
    auto source = std::make_unique<des::ScheduledSource<Message>>(std::move(schedule));
    auto* source_ptr = source.get();
    const auto source_id = simulator.add(std::move(source));
    std::vector<des::MultiServer<Message>*> server_ptrs;
    std::vector<std::size_t> server_ids;
    for (const auto& stage : process.servers) {
        auto server = std::make_unique<des::MultiServer<Message>>(stage.capacity, stage.service_scale, stage.queue,
            false, stage.service ? std::optional<des::ExponentialService>{
                {stage.service->rate, seed, scenario, replication, stage.service->stream}} : std::nullopt);
        server_ptrs.push_back(server.get());
        server_ids.push_back(simulator.add(std::move(server)));
    }
    auto sink = std::make_unique<des::CompletionSink<Message>>();
    auto* sink_ptr = sink.get();
    const auto sink_id = simulator.add(std::move(sink));
    auto bridge = std::make_unique<hybrid::EntityToPulse<Message>>(
        [amount = spec.completion_bridge->amount](const des::Entity&) { return amount; });
    const auto bridge_id = simulator.add(std::move(bridge));
    auto clocked = std::make_unique<hybrid::ClockedSD<Message>>(std::move(stocks), spec.dt);
    auto* clocked_ptr = clocked.get();
    const auto sd_id = simulator.add(std::move(clocked));
    simulator.connect(source_id, des::ScheduledSource<Message>::output_port,
                      server_ids.front(), des::MultiServer<Message>::input_port);
    for (std::size_t i = 1; i < server_ids.size(); ++i)
        simulator.connect(server_ids[i - 1], des::MultiServer<Message>::output_port,
                          server_ids[i], des::MultiServer<Message>::input_port);
    simulator.connect(server_ids.back(), des::MultiServer<Message>::output_port,
                      sink_id, des::CompletionSink<Message>::input_port);
    simulator.connect(server_ids.back(), des::MultiServer<Message>::output_port,
                      bridge_id, hybrid::EntityToPulse<Message>::input_port);
    simulator.connect(bridge_id, hybrid::EntityToPulse<Message>::output_port,
                      sd_id, static_cast<std::uint32_t>(stock_index.at(spec.completion_bridge->stock)));
    std::optional<std::size_t> feedback_id;
    if (spec.feedback_bridge) {
        const auto& feedback = *spec.feedback_bridge;
        feedback_id = simulator.add(std::make_unique<hybrid::StockToEntity<Message>>(
            feedback.threshold, feedback.service_duration, feedback.first_id, feedback.max_count));
        simulator.connect(*feedback_id, hybrid::StockToEntity<Message>::output_port,
                          server_ids.front(), des::MultiServer<Message>::input_port);
    }
    std::optional<std::size_t> rate_source_id;
    if (spec.rate_bridge) {
        const auto& rate_bridge = *spec.rate_bridge;
        rate_source_id = simulator.add(std::make_unique<hybrid::RateDrivenSource<Message>>(
            seed, scenario, replication, rate_bridge.first_id, rate_bridge.max_count,
            rate_bridge.stream, rate_bridge.service_rate));
        simulator.connect(*rate_source_id, hybrid::RateDrivenSource<Message>::output_port,
                          server_ids.front(), des::MultiServer<Message>::input_port);
    }

    const auto steps = static_cast<std::size_t>(std::llround(spec.horizon / spec.dt));
    std::vector<Row> rows;
    rows.reserve((steps + 1) * spec.outputs.size());
    for (std::size_t step = 0; step <= steps; ++step) {
        const double time = step * spec.dt;
        try {
            (void)simulator.run_until(time);
            if (feedback_id) {
                const double level = clocked_ptr->model().state().at(
                    stock_index.at(spec.feedback_bridge->stock));
                simulator.inject(time, *feedback_id,
                    hybrid::StockToEntity<Message>::input_port, Message{level});
                (void)simulator.run_until(time);
            }
            if (rate_source_id) {
                const auto& rate_bridge = *spec.rate_bridge;
                const double level = clocked_ptr->model().state().at(
                    stock_index.at(rate_bridge.stock));
                const double rate = rate_bridge.base_rate + rate_bridge.gain * level;
                if (!std::isfinite(rate) || rate < 0)
                    throw std::domain_error("SD-driven arrival rate must be finite and nonnegative");
                simulator.inject(time, *rate_source_id,
                    hybrid::RateDrivenSource<Message>::rate_port, Message{rate});
                (void)simulator.run_until(time);
            }
        }
        catch (const std::exception& error) {
            throw Error("IR_HYBRID_RUNTIME", "", error.what());
        }
        for (const auto& output : spec.outputs) {
            double value = 0;
            if (output.metric.empty()) {
                value = clocked_ptr->model().state().at(stock_index.at(output.source));
            } else if (output.source == process.source_id) {
                value = static_cast<double>(source_ptr->emitted_count());
            } else if (output.source == process.sink_id) {
                if (output.metric == "completed") value = static_cast<double>(sink_ptr->completed_count());
                else if (output.metric == "cycle_total") value = sink_ptr->total_cycle_time();
                else if (output.metric == "cycle_mean")
                    value = sink_ptr->completed_count() == 0 ? 0 : sink_ptr->mean_cycle_time();
            } else {
                const auto stage = std::find_if(process.servers.begin(), process.servers.end(),
                    [&output](const DesServer& item) { return item.id == output.source; });
                const auto* server_ptr = server_ptrs.at(static_cast<std::size_t>(
                    std::distance(process.servers.begin(), stage)));
                if (output.metric == "accepted") value = static_cast<double>(server_ptr->accepted_count());
                else if (output.metric == "rejected") value = static_cast<double>(server_ptr->rejected_count());
                else if (output.metric == "completed") value = static_cast<double>(server_ptr->completed_count());
                else if (output.metric == "waiting") value = static_cast<double>(server_ptr->waiting());
                else if (output.metric == "busy") value = static_cast<double>(server_ptr->busy_servers());
                else if (output.metric == "queue_mean")
                    value = time == 0 ? 0 : server_ptr->mean_queue_length(time);
                else if (output.metric == "utilization")
                    value = time == 0 ? 0 : server_ptr->utilization(time);
            }
            rows.push_back(Row{time, output.id, value});
        }
    }
    return rows;
}

std::vector<Row> run_abm_sd(const Model& spec,
                            const std::map<std::string, double>& parameters,
                            std::uint64_t seed, std::uint32_t scenario,
                            std::uint32_t replication) {
    if (!spec.abm_population || !spec.adoption_bridge ||
        scenario > 65535 || replication > 65535)
        throw Error("IR_ABM_RUNTIME", "", "invalid ABM model or draw address");
    const auto& population_spec = *spec.abm_population;
    const double innovation = parameters.at(population_spec.innovation_parameter);
    const double imitation = parameters.at(population_spec.imitation_parameter);
    if (innovation < 0 || imitation < 0 ||
        !std::isfinite((innovation + imitation) * spec.dt) ||
        (innovation + imitation) * spec.dt > 1)
        throw Error("IR_ABM_RUNTIME", "/abm/population", "scenario adoption probability bound exceeds one");

    sd::Model stocks;
    std::map<std::string, std::size_t> stock_index;
    for (const auto& stock : spec.stocks)
        stock_index.emplace(stock.id, stocks.add_stock(stock.id, stock.initial, stock.non_negative));
    for (const auto& flow : spec.flows) {
        const auto source = flow.source ? stock_index.at(*flow.source) : sd::Model::boundary;
        const auto destination = flow.destination ? stock_index.at(*flow.destination) : sd::Model::boundary;
        const Expression* expression = &flow.expression;
        stocks.add_flow(source, destination,
            [stock_index, parameters, expression](const auto& state, double time) {
                auto values = parameters;
                values.emplace("t", time);
                for (const auto& [id, index] : stock_index) values.emplace(id, state[index]);
                return expression->evaluate(values);
            }, flow.non_negative);
    }

    abm::SyncPopulation<bool> population;
    for (std::size_t i = 0; i < population_spec.count; ++i)
        (void)population.spawn(i < population_spec.initial_adopters);
    double probability = 0;
    std::uint32_t step_index = 0;
    population.add_phase([&](std::size_t index, const auto& snapshot) {
        if (snapshot[index].value) return true;
        const rng::DrawAddress address{scenario, replication, snapshot[index].id,
            step_index, population_spec.stream, 0};
        return rng::bernoulli(probability, rng::draw(seed, address)[0]);
    });

    devs::Simulator<double> simulator;
    auto clocked = std::make_unique<hybrid::ClockedSD<double>>(std::move(stocks), spec.dt);
    auto* clocked_ptr = clocked.get();
    const auto sd_id = simulator.add(std::move(clocked));
    const auto aggregate_id = simulator.add(
        std::make_unique<hybrid::PopulationToStock<bool>>(population,
            [](bool adopted) { return adopted ? 1.0 : 0.0; }));
    simulator.connect(aggregate_id, hybrid::PopulationToStock<bool>::output_port,
        sd_id, static_cast<std::uint32_t>(stock_index.at(spec.adoption_bridge->stock)));

    const auto steps = static_cast<std::size_t>(std::llround(spec.horizon / spec.dt));
    std::vector<Row> rows;
    rows.reserve((steps + 1) * spec.outputs.size());
    for (std::size_t step = 0; step <= steps; ++step) {
        const double time = step * spec.dt;
        try {
            (void)simulator.run_until(time);
            if (step > 0) {
                const auto adopted = static_cast<std::size_t>(std::count_if(
                    population.records().begin(), population.records().end(),
                    [](const auto& agent) { return agent.alive && agent.value; }));
                const double prior_fraction = static_cast<double>(adopted) /
                    static_cast<double>(population_spec.count);
                probability = (innovation + imitation * prior_fraction) * spec.dt;
                if (!std::isfinite(probability) || probability < 0 || probability > 1)
                    throw std::domain_error("adoption probability left [0,1]");
                step_index = static_cast<std::uint32_t>(step);
                population.step();
            }
            simulator.inject(time, aggregate_id,
                hybrid::PopulationToStock<bool>::trigger_port, 0.0);
            (void)simulator.run_until(time);
        } catch (const std::exception& error) {
            throw Error("IR_ABM_RUNTIME", "/abm/population", error.what());
        }
        const auto adopted = static_cast<double>(std::count_if(
            population.records().begin(), population.records().end(),
            [](const auto& agent) { return agent.alive && agent.value; }));
        for (const auto& output : spec.outputs) {
            double value = 0;
            if (output.metric.empty())
                value = clocked_ptr->model().state().at(stock_index.at(output.source));
            else if (output.metric == "active")
                value = static_cast<double>(population.active_count());
            else if (output.metric == "adopted") value = adopted;
            rows.push_back(Row{time, output.id, value});
        }
    }
    return rows;
}

hybrid::TransactionalAgentPool<std::size_t>::Change make_agent_pool_change(
    const AgentPoolSpec& spec, const AgentPoolChange& scheduled, std::size_t schedule_index) {
    hybrid::TransactionalAgentPool<std::size_t>::Change change;
    change.time = scheduled.time;
    change.releases = scheduled.releases;
    change.updates = scheduled.updates;
    change.departures = scheduled.departures;
    change.hires = scheduled.hires;
    change.requests = scheduled.requests;
    for (std::size_t invocation = 0; invocation < scheduled.phases.size(); ++invocation) {
        const auto* phase = &spec.phases.at(scheduled.phases[invocation]);
        const auto pointer = "/agent_pool/schedule/" + std::to_string(schedule_index) +
            "/phases/" + std::to_string(invocation);
        change.phases.push_back([phase, pointer, event_time = scheduled.time](
            std::size_t index, const auto& snapshot, const auto& broker) {
            try {
                double active = 0;
                double total_capacity = 0;
                for (const auto& record : snapshot) {
                    if (!record.alive) continue;
                    ++active;
                    total_capacity += static_cast<double>(record.value);
                }
                const auto& agent = snapshot[index];
                const double value = phase->capacity_expression.evaluate({
                    {"capacity", static_cast<double>(agent.value)},
                    {"allocated", static_cast<double>(broker.allocated_to(agent.id))},
                    {"total_capacity", total_capacity},
                    {"total_allocated", static_cast<double>(broker.allocated_units())},
                    {"active", active}, {"agent_id", static_cast<double>(agent.id)},
                    {"t", event_time}});
                if (value < 0 || value > 1000000 || value != std::floor(value))
                    throw std::domain_error("capacity result must be an integer in [0,1000000]");
                return static_cast<std::size_t>(value);
            } catch (const std::exception& error) {
                throw Error("IR_AGENT_POOL_PHASE", pointer, "phase " + phase->id +
                    ", agent " + std::to_string(snapshot[index].id) + ": " + error.what());
            }
        });
    }
    return change;
}

void append_agent_pool_rows(const Model& model, double time,
    const hybrid::TransactionalAgentPool<std::size_t>& observed_system,
    const hybrid::AgentPoolProcess<std::size_t>* delivery,
    const std::set<std::uint64_t>& granted, std::vector<Row>& rows,
    const sd::Model::State* stock_state = nullptr) {
    std::optional<hybrid::AgentPoolProcess<std::size_t>::TimeStatistics> time_statistics;
    if (delivery) {
        try {
            // Grid ties may have drained an event a few ulps after the printed tick.
            time_statistics = delivery->time_statistics(std::max(time, observed_system.now()));
        } catch (const std::exception& error) {
            throw Error("IR_AGENT_POOL_RUNTIME", "/agent_pool/delivery", error.what());
        }
    }
    for (const auto& output : model.agent_pool->outputs) {
        double value = 0;
        if (output.source == AgentPoolOutput::Source::stock) {
            if (!stock_state) throw std::logic_error("stock observation requires coupled SD state");
            value = stock_state->at(static_cast<std::size_t>(output.reference));
        } else if (output.source == AgentPoolOutput::Source::pool) {
            const auto& pool = observed_system.pool();
            if (output.metric == "capacity") value = static_cast<double>(pool.capacity());
            else if (output.metric == "available") value = static_cast<double>(pool.available());
            else if (output.metric == "allocated_units")
                value = static_cast<double>(pool.allocated_units());
            else if (output.metric == "allocations") value = static_cast<double>(pool.allocated());
            else value = static_cast<double>(pool.waiting());
        } else if (output.source == AgentPoolOutput::Source::population) {
            value = static_cast<double>(observed_system.population().active_count());
        } else if (output.source == AgentPoolOutput::Source::agent) {
            const auto& records = observed_system.population().records();
            if (output.reference < records.size() && records[output.reference].alive) {
                if (output.metric == "alive") value = 1;
                else if (output.metric == "capacity")
                    value = static_cast<double>(records[output.reference].value);
                else value = static_cast<double>(observed_system.broker().allocated_to(output.reference));
            }
        } else if (output.source == AgentPoolOutput::Source::process) {
            const auto& statistics = (*delivery).statistics();
            if (output.metric == "accepted") value = static_cast<double>(statistics.accepted);
            else if (output.metric == "started") value = static_cast<double>(statistics.started);
            else if (output.metric == "completed") value = static_cast<double>(statistics.completed);
            else if (output.metric == "in_service")
                value = static_cast<double>(statistics.started - statistics.completed);
            else if (output.metric == "waiting")
                value = static_cast<double>(statistics.accepted - statistics.started);
            else if (output.metric == "wait_total") value = statistics.wait_total;
            else if (output.metric == "cycle_total") value = statistics.cycle_total;
            else {
                const auto& weighted = time_statistics.value();
                if (output.metric == "capacity_time") value = weighted.capacity_time;
                else if (output.metric == "allocated_time") value = weighted.allocated_time;
                else if (output.metric == "available_time") value = weighted.available_time;
                else if (output.metric == "queue_time") value = weighted.queue_time;
                else if (output.metric == "service_time") value = weighted.service_time;
                else if (output.metric == "population_time") value = weighted.population_time;
                else if (output.metric == "utilization") value = weighted.utilization();
                else if (output.metric == "mean_queue") value = weighted.mean_queue();
                else if (output.metric == "mean_in_service") value = weighted.mean_in_service();
                else value = weighted.mean_headcount();
            }
        } else if (output.metric != "granted") {
            const auto found = (*delivery).records().find(output.reference);
            if (found != (*delivery).records().end()) {
                const auto& record = found->second;
                value = output.metric == "completed" ? record.completed.has_value()
                    : record.started.has_value() && !record.completed.has_value();
            }
        } else {
            value = granted.contains(output.reference) ? 1.0 : 0.0;
        }
        rows.push_back(Row{time, output.id, value});
    }
}

std::vector<Row> run_agent_pool(const Model& model) {
    if (!model.agent_pool)
        throw Error("IR_AGENT_POOL_RUNTIME", "/agent_pool", "missing agent pool specification");
    const auto& spec = *model.agent_pool;
    abm::SyncPopulation<std::size_t> population;
    for (const auto capacity : spec.initial_capacities) (void)population.spawn(capacity);
    hybrid::TransactionalAgentPool<std::size_t> system(
        std::move(population), [](const std::size_t& capacity) { return capacity; },
        spec.max_request_units);
    std::optional<hybrid::AgentPoolProcess<std::size_t>> delivery;
    if (spec.delivery_id) delivery.emplace(system);
    const auto steps = static_cast<std::size_t>(std::llround(model.horizon / model.dt));
    std::vector<Row> rows;
    rows.reserve((steps + 1) * spec.outputs.size());
    std::set<std::uint64_t> granted;
    std::size_t next_change = 0;
    for (std::size_t step = 0; step <= steps; ++step) {
        const double time = step * model.dt;
        // Zero has no grid-rounding error: never pull positive-time delivery into t=0.
        const double tie_tolerance = 8 * std::numeric_limits<double>::epsilon() *
            std::abs(time);
        while (true) {
            const double scheduled_time = next_change < spec.schedule.size()
                ? spec.schedule[next_change].time : std::numeric_limits<double>::infinity();
            const double completion_time = delivery ? delivery->next_completion()
                : std::numeric_limits<double>::infinity();
            const double event_time = std::min(scheduled_time, completion_time);
            if (!std::isfinite(event_time) || event_time > time + tie_tolerance) break;
            const bool has_scheduled_change = scheduled_time == event_time;
            AgentPoolChange completion_only{};
            completion_only.time = event_time;
            const auto& scheduled = has_scheduled_change ? spec.schedule[next_change] : completion_only;
            auto change = make_agent_pool_change(spec, scheduled, next_change);
            try {
                if (delivery) {
                    hybrid::AgentPoolProcess<std::size_t>::Change process_change;
                    process_change.workforce = std::move(change);
                    for (const auto& engagement : scheduled.engagements)
                        process_change.arrivals.push_back({engagement.request_id,
                            engagement.units, engagement.duration});
                    const auto result = delivery->apply(process_change);
                    for (const auto& grant : result.staffing.grants) granted.insert(grant.request_id);
                } else {
                    const auto result = system.apply(change);
                    for (const auto& grant : result.grants) granted.insert(grant.request_id);
                }
            } catch (const Error&) {
                throw;
            } catch (const std::exception& error) {
                throw Error("IR_AGENT_POOL_RUNTIME", has_scheduled_change
                    ? "/agent_pool/schedule/" + std::to_string(next_change)
                    : "/agent_pool/delivery", error.what());
            }
            if (has_scheduled_change) ++next_change;
        }
        append_agent_pool_rows(model, time, delivery ? delivery->system() : system,
                               delivery ? &*delivery : nullptr, granted, rows);
    }
    return rows;
}

std::vector<Row> run_agent_pool_sd(const Model& model, const std::map<std::string, double>& parameters) {
    if (!model.agent_pool || !model.agent_pool->delivery_id || !model.completion_bridge)
        throw Error("IR_AGENT_POOL_SD_RUNTIME", "", "incomplete agent-pool SD model");
    const auto& spec = *model.agent_pool;
    using Process = hybrid::AgentPoolProcess<std::size_t>;
    using Message = std::variant<Process::Change, Process::Result, double>;
    using Delivery = hybrid::AgentPoolProcessAtomic<std::size_t, Message>;
    using Bridge = hybrid::AgentPoolCompletionToPulse<std::size_t, Message>;
    using SD = hybrid::ClockedSD<Message>;
    sd::Model stocks;
    std::map<std::string, std::size_t> stock_index;
    for (const auto& stock : model.stocks)
        stock_index.emplace(stock.id, stocks.add_stock(stock.id, stock.initial, stock.non_negative));
    for (const auto& flow : model.flows) {
        const auto source = flow.source ? stock_index.at(*flow.source) : sd::Model::boundary;
        const auto destination = flow.destination ? stock_index.at(*flow.destination) : sd::Model::boundary;
        const Expression* expression = &flow.expression;
        stocks.add_flow(source, destination,
            [stock_index, parameters, expression](const auto& state, double time) {
                auto values = parameters;
                values.emplace("t", time);
                for (const auto& [id, index] : stock_index) values.emplace(id, state[index]);
                return expression->evaluate(values);
            }, flow.non_negative);
    }
    abm::SyncPopulation<std::size_t> population;
    for (const auto capacity : spec.initial_capacities) (void)population.spawn(capacity);
    devs::Simulator<Message> simulator;
    const auto delivery_id = simulator.add(std::make_unique<Delivery>(
        std::move(population), [](const auto& capacity) { return capacity; }, spec.max_request_units));
    const auto bridge_id = simulator.add(std::make_unique<Bridge>(model.completion_bridge->amount));
    const auto sd_id = simulator.add(std::make_unique<SD>(std::move(stocks), model.dt));
    simulator.connect(delivery_id, Delivery::output_port, bridge_id, Bridge::input_port);
    simulator.connect(bridge_id, Bridge::output_port, sd_id,
        static_cast<std::uint32_t>(stock_index.at(model.completion_bridge->stock)));
    for (std::size_t i = 0; i < spec.schedule.size(); ++i) {
        const auto& scheduled = spec.schedule[i];
        Process::Change change;
        change.workforce = make_agent_pool_change(spec, scheduled, i);
        for (const auto& engagement : scheduled.engagements)
            change.arrivals.push_back({engagement.request_id, engagement.units, engagement.duration});
        simulator.inject(scheduled.time, delivery_id, Delivery::input_port, Message{std::move(change)});
    }
    const auto steps = static_cast<std::size_t>(std::llround(model.horizon / model.dt));
    std::vector<Row> rows;
    rows.reserve((steps + 1) * spec.outputs.size());
    std::set<std::uint64_t> granted;
    for (std::size_t step = 0; step <= steps; ++step) {
        const double time = step * model.dt;
        try {
            std::size_t transitions = 0;
            while (simulator.next_time() <= time) {
                if (++transitions > 1000000) throw std::runtime_error("hybrid transition limit exceeded");
                const auto result = simulator.step_transactional();
                for (const auto& event : result->emissions)
                    if (event.source == delivery_id)
                        for (const auto& grant : std::get<Process::Result>(event.value).staffing.grants)
                            granted.insert(grant.request_id);
            }
            // Checked rollback may replace component objects; always resolve by ID.
            const auto& delivery = dynamic_cast<const Delivery&>(simulator.model(delivery_id)).core();
            const auto& state = dynamic_cast<const SD&>(simulator.model(sd_id)).model().state();
            append_agent_pool_rows(model, time, delivery.system(), &delivery, granted, rows, &state);
        } catch (const Error&) {
            throw;
        } catch (const std::exception& error) {
            throw Error("IR_AGENT_POOL_SD_RUNTIME", "", error.what());
        }
    }
    return rows;
}

std::vector<Row> run(const Model& spec,
                     const std::map<std::string, double>& parameter_overrides,
                     std::uint64_t seed, std::uint32_t scenario,
                     std::uint32_t replication) {
    auto parameters = spec.parameters;
    for (const auto& [id, value] : parameter_overrides) {
        if (!parameters.contains(id) || !std::isfinite(value))
            throw Error("IR_OVERRIDE", "/experiment", "override must name a declared parameter and be finite: " + id);
        parameters[id] = value;
    }
    if (spec.kind == Model::Kind::des) return spec.typed_process ?
        run_typed_des(spec,parameters,seed,scenario,replication) : run_des(spec, seed, scenario, replication);
    if (spec.kind == Model::Kind::abm) return run_typed_abm(spec,parameters,seed,scenario,replication);
    if (spec.kind == Model::Kind::agent_stock_sd) return run_agent_stock_sd(spec,parameters);
    if (spec.kind == Model::Kind::hybrid)
        return run_hybrid(spec, parameters, seed, scenario, replication);
    if (spec.kind == Model::Kind::abm_sd)
        return run_abm_sd(spec, parameters, seed, scenario, replication);
    if (spec.kind == Model::Kind::agent_pool) return run_agent_pool(spec);
    if (spec.kind == Model::Kind::agent_pool_sd) return run_agent_pool_sd(spec, parameters);
    sd::Model dynamics;
    std::map<std::string, std::size_t> stock_index;
    for (const auto& stock : spec.stocks)
        stock_index.emplace(stock.id, dynamics.add_stock(stock.id, stock.initial, stock.non_negative, stock.clip_outflows));
    std::map<std::string, std::function<double(double)>> functions;
    for (const auto& table : spec.tables) {
        const sd::LookupTable* lookup = &table.lookup;
        functions.emplace(table.id, [lookup](double argument) { return lookup->evaluate(argument); });
    }
    for (const auto& binding : spec.series_data) {
        const auto* series=&binding.series;
        functions.emplace(binding.id,[series](double time) { return series->value_at(time); });
    }
    std::vector<DelayState> delay_states;
    std::vector<double> initial_durations;
    delay_states.reserve(spec.delays.size());
    std::map<std::string, double> delay_values;
    std::map<std::string, std::size_t> flow_index;
    for (const auto& flow : spec.flows) {
        const auto source = flow.source ? stock_index.at(*flow.source) : sd::Model::boundary;
        const auto destination = flow.destination ? stock_index.at(*flow.destination) : sd::Model::boundary;
        const Expression* expression = &flow.expression;
        flow_index.emplace(flow.id, dynamics.add_flow(source, destination,
            [&spec, &delay_values, stock_index, expression, parameters, functions](const auto& state, double time) {
                auto values = parameters;
                values.emplace("t", time);
                values.emplace("dt", spec.dt);
                for (const auto& [id, index] : stock_index) values.emplace(id, state[index]);
                for (const auto& [id, value] : delay_values) values.emplace(id, value);
                evaluate_auxiliaries(spec,values,functions);
                return expression->evaluate(values, functions);
            }, flow.non_negative, flow.clip_negative));
    }
    for (const auto& stock : spec.stocks) if (stock.clip_outflows) {
        std::vector<std::size_t> order;
        for (const auto& id : stock.outflow_order) order.push_back(flow_index.at(id));
        dynamics.set_outflow_order(stock_index.at(stock.id),std::move(order));
    }
    std::vector<Row> rows;
    const auto steps = static_cast<std::size_t>(std::llround(spec.horizon / spec.dt));
    rows.reserve((steps + 1) * spec.outputs.size());
    for (std::size_t step = 0; step <= steps; ++step) {
        const double time = spec.start + step * spec.dt;
        auto values = parameters;
        values.emplace("t", time);
        values.emplace("dt", spec.dt);
        for (const auto& [id, index] : stock_index) values.emplace(id, dynamics.state()[index]);
        evaluate_auxiliaries(spec,values,functions);
        std::vector<double> durations;
        durations.reserve(spec.delays.size());
        for (const auto& delay : spec.delays) {
            try {
                if (step > 0 && delay.type == sd::DelayKind::fixed) {
                    durations.push_back(initial_durations[durations.size()]);
                    continue;
                }
                const double duration = std::holds_alternative<double>(delay.duration) ? std::get<double>(delay.duration)
                    : std::get<Expression>(delay.duration).evaluate(values, functions);
                validate_delay_duration(delay, duration, spec.dt);
                durations.push_back(duration);
            } catch (const std::exception& error) {
                throw Error("IR_DELAY_RUNTIME", "/components/" +
                    std::to_string(delay.component_index) + "/duration", error.what());
            }
        }
        if (step == 0) {
            initial_durations = durations;
            for (std::size_t i = 0; i < spec.delays.size(); ++i) {
                const auto& delay = spec.delays[i];
                try { delay_states.push_back(make_delay(delay, durations[i], spec.dt)); }
                catch (const std::exception& error) {
                    throw Error("IR_DELAY_RUNTIME", "/components/" +
                        std::to_string(delay.component_index) + "/initial", error.what());
                }
            }
        }
        delay_values.clear();
        for (std::size_t i = 0; i < spec.delays.size(); ++i) {
            try { delay_values.emplace(spec.delays[i].id,
                std::visit([&](const auto& state) { return state.output(durations[i]); }, delay_states[i])); }
            catch (const std::exception& error) {
                throw Error("IR_DELAY_RUNTIME", "/components/" +
                    std::to_string(spec.delays[i].component_index) + "/duration", error.what());
            }
        }
        for (const auto& [id, value] : delay_values) values.emplace(id, value);
        for (std::size_t i = 0; i < spec.outputs.size(); ++i) {
            const auto& output = spec.outputs[i];
            try {
                rows.push_back(Row{time, output.id, output.expression
                    ? output.expression->evaluate(values, functions)
                    : output.delay ? delay_values.at(output.source)
                                   : dynamics.state()[stock_index.at(output.source)]});
            } catch (const std::exception& error) {
                throw Error("IR_OUTPUT_RUNTIME", "/outputs/" + std::to_string(i) + "/expr", error.what());
            }
        }
        if (step >= steps) continue;

        auto next_delays = delay_states;
        for (std::size_t i = 0; i < spec.delays.size(); ++i) {
            const auto& delay = spec.delays[i];
            try {
                const double input = delay.input.evaluate(values, functions);
                std::visit([&](auto& state) { state.step(input, durations[i], spec.dt); }, next_delays[i]);
            } catch (const std::exception& error) {
                throw Error("IR_DELAY_RUNTIME", "/components/" +
                    std::to_string(delay.component_index) + "/input", error.what());
            }
        }
        auto next_dynamics = dynamics;
        next_dynamics.step(time, spec.dt, spec.integrator);
        dynamics = std::move(next_dynamics);
        delay_states = std::move(next_delays);
    }
    return rows;
}

runtime::Experiment load_experiment_document(const Json& document,const Model& model,
                                             std::optional<std::uint64_t> seed_override) {
    if (!document.is_object()) throw Error("IR_TYPE", "/experiment", "experiment must be an object");
    reject_unknown(document, {"seed", "replications", "scenarios", "design"}, "/experiment");
    if(document.contains("scenarios")==document.contains("design"))
        throw Error("IR_EXPERIMENT","/experiment","exactly one scenarios list or design is required");
    const auto& seed = field(document, "seed", "/experiment");
    const auto& replications = field(document, "replications", "/experiment");
    if (!seed.is_number_unsigned() || !replications.is_number_unsigned())
        throw Error("IR_TYPE", "/experiment", "seed and replications must be unsigned integers");
    if (replications.get<std::uint64_t>() > 65536)
        throw Error("IR_EXPERIMENT", "/experiment/replications", "replication count exceeds 65536");
    runtime::Experiment experiment{seed_override.value_or(seed.get<std::uint64_t>()), {}, replications.get<std::uint32_t>()};
    if(document.contains("design")) {
        const auto& design=document.at("design");const std::string pointer="/experiment/design";
        const auto kind=string_field(design,"kind",pointer);
        if(kind=="grid") reject_unknown(design,{"kind","axes","first_id"},pointer);
        else if(kind=="lhs") reject_unknown(design,{"kind","bounds","count","design_seed","first_id"},pointer);
        else if(kind=="sobol") reject_unknown(design,{"kind","bounds","count","first_id"},pointer);
        else throw Error("IR_EXPERIMENT",pointer+"/kind","unknown scenario design");
        std::uint32_t first=0;
        if(design.contains("first_id")) {
            const auto& value=design.at("first_id");
            if(!value.is_number_unsigned() || value.get<std::uint64_t>()>65535)
                throw Error("IR_TYPE",pointer+"/first_id","first scenario ID must be an unsigned 16-bit integer");
            first=value.get<std::uint32_t>();
        }
        try {
            if(kind=="grid") {
                const auto& values=field(design,"axes",pointer);
                if(!values.is_object()) throw Error("IR_TYPE",pointer+"/axes","grid axes must be an object");
                std::vector<runtime::GridAxis> axes;
                for(auto it=values.begin();it!=values.end();++it) {
                    const auto location=pointer+"/axes/"+pointer_token(it.key());
                    if(!model.parameters.contains(it.key())) throw Error("IR_OVERRIDE",location,"design axis must name a model parameter");
                    if(!it.value().is_array()) throw Error("IR_TYPE",location,"grid axis must be an array");
                    runtime::GridAxis axis{it.key(),{}};
                    for(const auto& value:it.value()) {
                        if(!value.is_number() || !std::isfinite(value.get<double>())) throw Error("IR_TYPE",location,"grid values must be finite numbers");
                        axis.values.push_back(value.get<double>());
                    }
                    axes.push_back(std::move(axis));
                }
                experiment.scenarios=runtime::expand_grid(std::move(axes),first);
            } else {
                const auto& count=field(design,"count",pointer);const auto& values=field(design,"bounds",pointer);
                if(!count.is_number_unsigned() || count.get<std::uint64_t>()==0 || count.get<std::uint64_t>()>65536)
                    throw Error("IR_TYPE",pointer+"/count","design count must be in [1, 65536]");
                if(!values.is_object()) throw Error("IR_TYPE",pointer+"/bounds","design bounds must be an object");
                std::vector<runtime::ParameterBounds> bounds;
                for(auto it=values.begin();it!=values.end();++it) {
                    const auto location=pointer+"/bounds/"+pointer_token(it.key());
                    if(!model.parameters.contains(it.key())) throw Error("IR_OVERRIDE",location,"design bound must name a model parameter");
                    if(!it.value().is_array() || it.value().size()!=2 || !it.value()[0].is_number() || !it.value()[1].is_number())
                        throw Error("IR_TYPE",location,"bound must contain two numbers");
                    bounds.push_back({it.key(),it.value()[0].get<double>(),it.value()[1].get<double>()});
                }
                if(kind=="lhs") {
                    const auto& design_seed=field(design,"design_seed",pointer);
                    if(!design_seed.is_number_unsigned()) throw Error("IR_TYPE",pointer+"/design_seed","LHS design seed must be uint64");
                    if(design_seed.get<std::uint64_t>()==experiment.seed)
                        throw Error("IR_EXPERIMENT",pointer+"/design_seed","design and effective execution seeds must differ");
                    experiment.scenarios=runtime::expand_lhs(std::move(bounds),count.get<std::size_t>(),design_seed.get<std::uint64_t>(),first);
                } else experiment.scenarios=runtime::expand_sobol(std::move(bounds),count.get<std::size_t>(),first);
            }
        } catch(const std::invalid_argument& error) { throw Error("IR_EXPERIMENT",pointer,error.what()); }
    } else {
    const auto& scenarios = field(document, "scenarios", "/experiment");
    if (!scenarios.is_array() || scenarios.empty())
        throw Error("IR_TYPE", "/experiment/scenarios", "scenarios must be a nonempty array");
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        const std::string pointer = "/experiment/scenarios/" + std::to_string(i);
        const auto& item = scenarios[i];
        reject_unknown(item, {"id", "parameters"}, pointer);
        const auto& id = field(item, "id", pointer);
        if (!id.is_number_unsigned() || id.get<std::uint64_t>() > 65535)
            throw Error("IR_TYPE", pointer + "/id", "scenario ID must be an unsigned 16-bit integer");
        const auto& overrides = field(item, "parameters", pointer);
        if (!overrides.is_object())
            throw Error("IR_TYPE", pointer + "/parameters", "parameters must be an object");
        runtime::Scenario scenario{id.get<std::uint32_t>(), {}};
        for (auto it = overrides.begin(); it != overrides.end(); ++it) {
            if (!model.parameters.contains(it.key()) || !it.value().is_number() ||
                !std::isfinite(it.value().get<double>()))
                throw Error("IR_OVERRIDE", pointer + "/parameters/" + it.key(),
                            "override must name a declared parameter and be finite");
            scenario.parameters.emplace(it.key(), it.value().get<double>());
        }
        experiment.scenarios.push_back(std::move(scenario));
    }
    }
    try { runtime::validate(experiment); }
    catch (const std::invalid_argument& error) {
        throw Error("IR_EXPERIMENT", "/experiment", error.what());
    }
    return experiment;
}
runtime::Experiment load_experiment(const std::string& path,const Model& model,
                                   std::optional<std::uint64_t> seed_override={},InputIdentity* receipt=nullptr) {
    InputIdentity input;
    const auto document=provenance::read_input(path,input,"/experiment");
    if(receipt) *receipt=std::move(input);
    return load_experiment_document(document,model,seed_override);
}
runtime::Experiment load_experiment_json(std::string_view json,const Model& model,
                                        std::optional<std::uint64_t> seed_override,InputIdentity* receipt) {
    InputIdentity input;
    const auto document=provenance::parse_input(json,input,"/experiment");
    auto experiment=load_experiment_document(document,model,seed_override);
    if(receipt) *receipt=std::move(input);
    return experiment;
}

int cli(int argc, char** argv) {
    try {
        if (argc < 3) throw Error("IR_USAGE", "", "usage: fathom lint|run model.json [--out results.csv|parquet|arrow] [--format csv|parquet|arrow] [--experiment experiment.json] [--threads N] [--seed S] [--manifest run.json | --no-manifest]; fathom bundle run.json --out bundle-directory; fathom replay run.json|bundle-directory [--out results.csv|parquet|arrow] [--threads N]; fathom verify-results run.json [--results results.csv|parquet|arrow] [--format csv|parquet|arrow]; fathom verify-results results.parquet|arrow --embedded [--format parquet|arrow]");
        const std::string command = argv[1];
        if(command=="check") return validation::cli(argc,argv);
        if(command=="explain") return explanation::cli(argc,argv);
        if(command=="viz") return visualization::cli(argc,argv);
        if(command=="bundle") {
            if(argc!=5 || std::string_view(argv[3])!="--out" || argv[4][0]=='\0' || std::string_view(argv[4]).starts_with("--"))
                throw Error("IR_USAGE","","usage: fathom bundle run.json --out bundle-directory");
            std::cout<<provenance::create_bundle(argv[2],argv[4]).dump()<<'\n';
            if(!std::cout) throw Error("IR_IO","","cannot write bundle verdict");
            return 0;
        }
        if(command=="verify-results") {
            std::string path;
            bool embedded=false;
            std::optional<runtime::OutputFormat> requested_format;
            for(int i=3;i<argc;) {
                const std::string option=argv[i++];
                if(option=="--embedded" && !embedded) { embedded=true;continue; }
                if(i>=argc || argv[i][0]=='\0') throw Error("IR_USAGE","","verify-results option needs a value");
                const std::string value=argv[i++];
                if(option=="--results" && path.empty()) path=value;
                else if(option=="--format" && !requested_format) {
                    try { requested_format=runtime::parse_output_format(value); }
                    catch(const std::exception& error) { throw Error("IR_USAGE","",error.what()); }
                } else throw Error("IR_USAGE","","unknown or duplicate verify-results option: "+option);
            }
            Json manifest;
            if(embedded) {
                if(!path.empty()) throw Error("IR_USAGE","","--embedded cannot be combined with --results");
                path=argv[2];
                const auto format=requested_format.value_or(runtime::infer_output_format(path));
                if(format==runtime::OutputFormat::csv)
                    throw Error("IR_USAGE","","--embedded requires Arrow or Parquet; CSV requires a sidecar manifest");
                if(!runtime::arrow_output_available())
                    throw Error("IR_RESULT_UNAVAILABLE","","Arrow/Parquet verification is unavailable in this build");
                runtime::OutputProvenance embedded_provenance;
                try { embedded_provenance=runtime::verify_embedded_observations(path,format); }
                catch(const std::exception& error) { throw Error("IR_RESULT_VERIFY","/results",error.what()); }
                manifest=provenance::validate_manifest(Json::parse(embedded_provenance.manifest_json));
            } else {
                manifest=provenance::read_manifest(argv[2]);
                if(manifest.at("manifest_version")!="0.2" && manifest.at("manifest_version")!="0.3")
                    throw Error("IR_MANIFEST","/manifest/manifest_version","result verification requires a version 0.2 or 0.3 manifest");
                if(manifest.at("manifest_version")=="0.3" && path.empty())
                    throw Error("IR_USAGE","/results","in-memory manifests require an explicit --results path");
                const auto format=requested_format.value_or(path.empty()
                    ? runtime::parse_output_format(manifest.at("output").at("format").get<std::string>())
                    : runtime::infer_output_format(path));
                if(path.empty()) path=manifest.at("output").at("path").get<std::string>();
                if(format!=runtime::OutputFormat::csv && !runtime::arrow_output_available())
                    throw Error("IR_RESULT_UNAVAILABLE","","Arrow/Parquet verification is unavailable in this build");
                try { runtime::verify_observations(path,format,runtime::OutputProvenance{manifest.dump()}); }
                catch(const std::exception& error) { throw Error("IR_RESULT_VERIFY","/results",error.what()); }
            }
            std::cout<<Json{{"verdict","pass"},{"scope","artifact-integrity"},{"manifest_id",manifest.at("id")},
                {"manifest_source",embedded?"embedded":"sidecar"},
                {"result",manifest.at("result")},{"path",std::filesystem::absolute(path).string()}}.dump()<<'\n';
            if(!std::cout) throw Error("IR_IO","","cannot write verification verdict");
            return 0;
        }
        if (command != "lint" && command != "run" && command != "replay")
            throw Error("IR_USAGE", "", "expected lint, run, bundle, replay or verify-results");
        const bool replay=command=="replay";
        std::optional<Json> expected;
        std::optional<provenance::Bundle> bundle;
        if(replay) {
            if(std::filesystem::is_directory(argv[2])) {
                bundle=provenance::load_bundle(argv[2]);expected=bundle->manifest;
            } else expected=provenance::read_manifest(argv[2]);
            if(expected->at("manifest_version")=="0.3")
                throw Error("IR_REPLAY_UNSUPPORTED","/manifest/manifest_version","in-memory run manifests do not yet support replay");
        }
        const auto model_path=replay ? expected->at("inputs").at("model").at("path").get<std::string>() : std::string(argv[2]);
        const Model model = bundle?std::move(bundle->model):load_file(model_path);
        if (command == "lint") {
            if (argc != 3) throw Error("IR_USAGE", "", "lint accepts one model path");
            std::cout << Json{{"verdict", "pass"}, {"diagnostics", Json::array()}}.dump() << '\n';
            return 0;
        }
        std::string output_path,manifest_path;
        bool no_manifest=false,require_check=false;
        std::string experiment_path;
        std::optional<runtime::OutputFormat> format_override;
        std::optional<std::uint64_t> threads,seed_override;
        const auto integer_option=[](const std::string& value) {
            std::uint64_t number=0;
            const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),number);
            if(error!=std::errc{} || end!=value.data()+value.size() || value.empty())
                throw Error("IR_USAGE","","option requires an unsigned decimal integer");
            return number;
        };
        for (int i = 3; i < argc;) {
            const std::string option = argv[i];
            if(option=="--require-check") {
                if(replay || require_check) throw Error("IR_USAGE","","--require-check is run-only and cannot repeat");
                require_check=true;++i;continue;
            }
            if(option=="--no-manifest") {
                if(replay || no_manifest || !manifest_path.empty())
                    throw Error("IR_USAGE","","--no-manifest is run-only, cannot repeat or combine with --manifest");
                no_manifest=true;++i;continue;
            }
            if (i + 1 >= argc) throw Error("IR_USAGE", "", "option needs a value");
            if(argv[i+1][0]=='\0') throw Error("IR_USAGE","","option value cannot be empty");
            if(std::string_view(argv[i+1]).starts_with("--"))
                throw Error("IR_USAGE","","option needs a value before the next flag");
            if(replay && (option=="--experiment" || option=="--seed" || option=="--manifest"))
                throw Error("IR_USAGE","","replay accepts only output, format and thread overrides");
            if (option == "--manifest" && manifest_path.empty() && !no_manifest) manifest_path=argv[i+1];
            else if (option == "--out" && output_path.empty()) output_path = argv[i + 1];
            else if (option == "--experiment" && experiment_path.empty()) experiment_path = argv[i + 1];
            else if(option=="--threads" && !threads) threads=integer_option(argv[i+1]);
            else if(option=="--seed" && !seed_override) seed_override=integer_option(argv[i+1]);
            else if(option=="--format" && !format_override) {
                try { format_override=runtime::parse_output_format(argv[i+1]); }
                catch(const std::invalid_argument& error) { throw Error("IR_USAGE","",error.what()); }
            }
            else throw Error("IR_USAGE", "", "unknown or duplicate run option: " + option);
            i+=2;
        }
        if(threads && (*threads==0 || *threads>256)) throw Error("IR_USAGE","","threads must be in [1, 256]");
        if(replay) {
            const auto& input=expected->at("inputs");
            seed_override=input.at("execution").at("seed").get<std::uint64_t>();
            if(!threads) threads=input.at("execution").at("threads").get<std::uint64_t>();
            if(!input.at("experiment").is_null()) experiment_path=input.at("experiment").at("path").get<std::string>();
        }
        if(!manifest_path.empty() && output_path.empty()) throw Error("IR_USAGE","","--manifest requires --out");
        if(!replay && !no_manifest && manifest_path.empty() && !output_path.empty())
            manifest_path=output_path+".manifest.json";
        if(!manifest_path.empty() && std::filesystem::exists(std::filesystem::symlink_status(manifest_path)))
            throw Error("IR_USAGE","","manifest destination must be new");
        const auto format=format_override.value_or(runtime::infer_output_format(output_path));
        if(format!=runtime::OutputFormat::csv && output_path.empty())
            throw Error("IR_USAGE","","Arrow/Parquet output requires --out");
        if(format!=runtime::OutputFormat::csv && !runtime::arrow_output_available())
            throw Error("IR_OUTPUT_UNAVAILABLE","","Arrow/Parquet output is unavailable in this build");
        std::vector<std::string> protected_inputs{model_path};
        if(bundle) {
            protected_inputs.insert(protected_inputs.end(),bundle->files.begin(),bundle->files.end());
            for(const auto& destination:{output_path,manifest_path}) if(!destination.empty() && provenance::within_bundle(destination,bundle->root))
                throw Error("IR_USAGE","","replay output cannot modify its input bundle");
        }
        if(!experiment_path.empty()) protected_inputs.push_back(experiment_path);
        if(replay) protected_inputs.push_back(argv[2]);
        for(const auto& binding:model.parameter_data) protected_inputs.push_back(binding.source);
        for(const auto& binding:model.series_data) protected_inputs.push_back(binding.source);
        if(model.population_data) protected_inputs.push_back(model.population_data->source);
        for(const auto& destination:{output_path,manifest_path}) if(!destination.empty())
            for(const auto& input:protected_inputs) if(provenance::same_destination(destination,input))
                throw Error("IR_USAGE","","output or manifest cannot overwrite an input");
        if(!output_path.empty() && !manifest_path.empty() && provenance::same_destination(output_path,manifest_path))
            throw Error("IR_USAGE","","result and manifest destinations must differ");
        std::optional<runtime::Experiment> experiment;
        std::optional<InputIdentity> experiment_input;
        if(bundle) {
            experiment=std::move(bundle->experiment);experiment_input=std::move(bundle->experiment_input);
        } else if (!experiment_path.empty()) {
            experiment_input.emplace();
            experiment = load_experiment(experiment_path, model,seed_override,&*experiment_input);
        }
        const auto seed=experiment ? experiment->seed : seed_override.value_or(0);
        if(require_check) {
            const auto checked=validation::check(model,experiment.value_or(runtime::Experiment{seed,{{0,{}}},1}),
                static_cast<std::size_t>(threads.value_or(1)));
            if(checked.at("verdict")!="pass")
                throw Error("IR_CHECK_FAILED","/checks","run requires passing declared checks; use fathom check for the detailed report");
        }
        std::optional<Json> input_identity;
        if(replay || !manifest_path.empty()) {
            const auto recorded_threads=replay ? expected->at("inputs").at("execution").at("threads").get<std::size_t>() : static_cast<std::size_t>(threads.value_or(1));
            input_identity=provenance::inputs(model,experiment,experiment_input,seed,recorded_threads);
            if(replay) provenance::require_equal(*input_identity,expected->at("inputs"),"/inputs");
        }
        std::vector<runtime::Trajectory> trajectories;
        if (!experiment) {
            const auto rows = run(model,{},seed_override.value_or(0));
            runtime::Trajectory trajectory{0,0,{}};trajectory.observations.reserve(rows.size());
            for (const auto& row : rows)
                trajectory.observations.push_back({row.time,row.output_id,row.value});
            trajectories.push_back(std::move(trajectory));
        } else {
            trajectories = runtime::run_experiment(*experiment,
                [&model](const runtime::Scenario& scenario, std::uint32_t replication,
                         std::uint64_t seed) {
                    const auto rows = run(model, scenario.parameters, seed, scenario.id, replication);
                    std::vector<runtime::Observation> observations;
                    observations.reserve(rows.size());
                    for (const auto& row : rows)
                        observations.push_back({row.time, row.output_id, row.value});
                    return observations;
                },runtime::ExecutionOptions{static_cast<std::size_t>(threads.value_or(1))});
        }
        std::optional<Json> numeric_identity;
        if(replay || !manifest_path.empty()) numeric_identity=provenance::result(trajectories);
        if(replay) provenance::require_equal(*numeric_identity,expected->at("result"),"/result");
        if(replay && output_path.empty()) {
            std::cout<<Json{{"verdict","pass"},{"manifest_id",expected->at("id")},
                {"result",*numeric_identity},{"threads",threads.value_or(1)}}.dump()<<'\n';
        } else if(output_path.empty()) {
            std::cout<<runtime::observations_csv(trajectories,!experiment);
            if(!std::cout) throw Error("IR_IO","","cannot write standard output");
        } else {
            try {
                if(manifest_path.empty()) {
                    const auto lineage=replay && expected->at("manifest_version")=="0.2"
                        ? std::optional{runtime::OutputProvenance{expected->dump()}} : std::nullopt;
                    runtime::write_observations(output_path,format,trajectories,!experiment,lineage?&*lineage:nullptr);
                }
                else {
                    const auto manifest=provenance::manifest(*input_identity,*numeric_identity,output_path,format);
                    provenance::publish(manifest_path,manifest,output_path,format,trajectories,!experiment);
                }
            } catch(const std::exception& error) { throw Error("IR_IO","",error.what()); }
        }
        return 0;
    } catch (const Error& error) {
        std::cerr << Json{{"verdict", "fail"}, {"diagnostics", Json::array({
            {{"code", error.code}, {"pointer", error.pointer}, {"message", error.what()}}
        })}}.dump() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << Json{{"verdict", "fail"}, {"diagnostics", Json::array({
            {{"code", "IR_RUNTIME"}, {"pointer", ""}, {"message", error.what()}}
        })}}.dump() << '\n';
        return 1;
    }
}

} // namespace ankurafathom::ir
