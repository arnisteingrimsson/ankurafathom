#include "ankurafathom/runtime/population_init.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include "ankurafathom/runtime/experiment.hpp"
#include "ankurafathom/abm/typed_population.hpp"
#include <bit>
#include <iostream>
#include <numeric>

namespace data=ankurafathom::runtime::data;
namespace rt=ankurafathom::runtime;
namespace abm=ankurafathom::abm;
namespace des=ankurafathom::des;
struct Agent {};
using Store=abm::PopulationStore<Agent>;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(const std::string& code,F f) {
    bool caught=false;try { f(); } catch(const data::Error& error) { caught=error.code==code; }
    require(caught,"expected population binding diagnostic");
}
std::vector<des::EntityField> target() {
    return {{"name",des::FieldKind::string},{"pay",des::FieldKind::real},{"count",des::FieldKind::integer},
        {"enabled",des::FieldKind::boolean},{"rank",des::FieldKind::integer}};
}
std::vector<data::PopulationField> mapping() {
    return {{"enabled","flag",""},{"count","counter","1"},{"pay","amount","USD/hour"},{"rank","level","1"},{"name","label",""}};
}
data::Schema source() {
    return {{{"external",data::Type::u64,"",{}},{"flag",data::Type::boolean,"",{}},{"level",data::Type::i32,"1",{}},
        {"counter",data::Type::u64,"1",{}},{"amount",data::Type::f64,"USD/hour",{}},{"label",data::Type::string,"",{}}},"external"};
}
void empty(const Store& population) {
    require(population.size()==0 && population.active_count()==0 && population.next_id()==population.first_id(),"failed binding changed identity or membership");
    if(population.network()) require(population.network()->vertices().empty() && population.network()->edges().empty(),"failed binding changed topology");
}
void verify(const Store& population,const data::PopulationReceipt<Agent>& receipt,std::uint64_t first) {
    require(population.size()==3 && population.active_count()==3 && population.next_id()==first+3 && receipt.entries.size()==3,"initialized population shape");
    const std::uint64_t keys[]={0,7,std::numeric_limits<std::uint64_t>::max()};
    const std::int64_t ranks[]={0,std::numeric_limits<std::int32_t>::max(),std::numeric_limits<std::int32_t>::min()};
    const std::int64_t counts[]={std::numeric_limits<std::int64_t>::max(),1,0};
    const double amounts[]={-10.5,std::bit_cast<double>(std::uint64_t{1}),-0.0};
    const std::string labels[]={"zero","one,quoted","last"};
    for(std::size_t i=0;i<3;++i) {
        const auto ref=receipt.entries[i].agent;
        require(ref.store==population.store_id() && ref.id==first+i && std::get<std::uint64_t>(receipt.entries[i].source_key)==keys[i],"source identity truncated or used as engine ID");
        require(population.alive(ref) && std::get<std::string>(population.field(ref,"name"))==labels[i],"string field mapping");
        require(std::bit_cast<std::uint64_t>(std::get<double>(population.field(ref,"pay")))==std::bit_cast<std::uint64_t>(amounts[i]),"real field bits changed");
        require(std::get<std::int64_t>(population.field(ref,"count"))==counts[i] && std::get<std::int64_t>(population.field(ref,"rank"))==ranks[i],"integer conversion changed values");
        require(std::get<bool>(population.field(ref,"enabled"))==(i!=1),"boolean field mapping");
    }
    if(population.network()) require(population.network()->vertices()==std::vector<std::uint64_t>({first,first+1,first+2}) &&
        population.network()->edges().empty() && population.network()->directed(),"initial population topology");
}
int main(int argc,char** argv) {
    try {
        if(!data::available()) { std::cout<<"population binding compiled; file loading requires Arrow\n";return 0; }
        require(argc==2,"usage: population_init_tests artifact-directory");
        const std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
        const auto path=root/"population.csv";
        const std::string header="external,flag,level,counter,amount,label\n";
        rt::write_text_atomically(path,header+"18446744073709551615,true,-2147483648,0,-0,last\n7,false,2147483647,1,5e-324,\"one,quoted\"\n0,true,0,9223372036854775807,-10.5,zero\n");
        const auto table=data::load_file(path,source());
        auto fields=mapping();std::vector<std::size_t> order{0,1,2,3,4};std::size_t cases=0;
        do {
            std::vector<data::PopulationField> permuted;for(const auto i:order) permuted.push_back(fields[i]);
            auto schema=target();if(cases%2) std::reverse(schema.begin(),schema.end());
            Store population(17,schema,12);population.configure_network(abm::CsrNetwork({}, {},true));
            const auto receipt=data::initialize_population(population,table,permuted);verify(population,receipt,12);
            require(receipt.file_hash==table.file_hash() && receipt.canonical_hash==table.canonical_hash(),"receipt lost data identity");
            rejects("DATA_POPULATION",[&] { data::initialize_population(population,table,fields); });verify(population,receipt,12);
            ++cases;
        } while(std::next_permutation(order.begin(),order.end()));
        Store maximum(91,target(),Store::max_entity_id-2);
        verify(maximum,data::initialize_population(maximum,table,fields),Store::max_entity_id-2);
        Store overflow(91,target(),Store::max_entity_id-1);
        rejects("DATA_POPULATION",[&] { data::initialize_population(overflow,table,fields); });empty(overflow);
        for(int kind=0;kind<6;++kind) {
            auto bad=fields;std::string code="DATA_MAPPING";
            if(kind==0) bad.pop_back();
            if(kind==1) bad[1].field=bad[0].field;
            if(kind==2) bad[0].field="unknown";
            if(kind==3) bad[0].column="unknown";
            if(kind==4) { bad[2].unit="USD/day";code="DATA_UNIT"; }
            if(kind==5) { bad[2].column="counter";bad[2].unit="1"; }
            Store population(1,target());population.configure_network(abm::CsrNetwork({}, {},true));
            rejects(code,[&] { data::initialize_population(population,table,bad); });empty(population);
            verify(population,data::initialize_population(population,table,fields),0);
        }
        // Fail on the last row after valid predecessors. Nothing may be appended.
        const auto invalid=root/"range.csv";
        rt::write_text_atomically(invalid,header+"0,true,0,0,0,a\n1,true,0,1,1,b\n2,true,0,9223372036854775808,2,c\n");
        const auto too_large=data::load_file(invalid,source());Store population(8,target(),42);
        rejects("DATA_RANGE",[&] { data::initialize_population(population,too_large,fields); });empty(population);
        verify(population,data::initialize_population(population,table,fields),42);
        for(const auto id:{42U,43U,44U}) population.retire({8,id});
        rejects("DATA_POPULATION",[&] { data::initialize_population(population,table,fields); });
        require(population.size()==3 && population.active_count()==0 && population.next_id()==45,"retired identities reset by initialization");
        const auto blank=root/"empty.csv";rt::write_text_atomically(blank,header);const auto no_rows=data::load_file(blank,source());
        Store initial(9,target(),Store::max_entity_id);const auto receipt=data::initialize_population(initial,no_rows,fields);empty(initial);
        require(receipt.entries.empty() && receipt.canonical_hash==no_rows.canonical_hash(),"empty binding lost receipt");
        auto bad=fields;bad[0].column="amount";bad[0].unit="USD/hour";
        rejects("DATA_MAPPING",[&] { data::initialize_population(initial,no_rows,bad); });empty(initial);
        // Real ABM execution from one shared immutable file snapshot. Each
        // trajectory owns its initialized store and phase writes.
        rt::Experiment experiment{42,{},3};
        for(std::uint32_t i=0;i<64;++i) experiment.scenarios.push_back({i,{}});
        const auto run=[&](const rt::Scenario& scenario,std::uint32_t replication,std::uint64_t) {
            Store store(scenario.id,target(),100);data::initialize_population(store,table,fields);
            abm::TypedPopulation<Agent> model(std::move(store));
            model.add_phase([=](Store::Reference ref,const Store& state) {
                auto record=state.record(ref);const auto pay=state.field_index("pay");
                record[pay]=std::get<double>(record[pay])+scenario.id+replication+1.;return record;
            });
            model.step();std::vector<rt::Observation> observations;
            for(std::uint64_t id=100;id<103;++id) observations.push_back({1,"pay_"+std::to_string(id),std::get<double>(model.store().field({scenario.id,id},"pay"))});
            return observations;
        };
        const auto serial=rt::run_experiment(experiment,run);
        for(const std::size_t threads:{8,32}) {
            const auto parallel=rt::run_experiment(experiment,run,{threads});
            for(std::size_t i=0;i<serial.size();++i) for(std::size_t j=0;j<3;++j) {
                const auto expected=(j==0?-10.5:0.)+serial[i].scenario+serial[i].replication+1.;
                require(serial[i].observations[j].value==expected,"initialized ABM phase differs from hand result");
                require(std::bit_cast<std::uint64_t>(serial[i].observations[j].value)==
                    std::bit_cast<std::uint64_t>(parallel[i].observations[j].value),"bound population execution changed across threads");
            }
        }
        std::cout<<cases<<" mapping permutations, exact values, identity limits, topology and rollback checks passed\n";return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
