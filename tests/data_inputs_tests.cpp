#include "ankurafathom/runtime/data_inputs.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include "ankurafathom/runtime/experiment.hpp"
#include "ankurafathom/sd/model.hpp"
#include <bit>
#include <iostream>

namespace data=ankurafathom::runtime::data;
namespace rt=ankurafathom::runtime;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(const std::string& code,F f) {
    bool caught=false;try { f(); } catch(const data::Error& error) { caught=error.code==code; }
    require(caught,"expected data input diagnostic");
}
bool same(double a,double b) { return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b); }
int main(int argc,char** argv) {
    try {
        if(!data::available()) { std::cout<<"input adapters compiled; loading requires Arrow\n";return 0; }
        require(argc==2,"usage: data_inputs_tests fixture-directory");
        const std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
        const auto file=root/"series.csv";rt::write_text_atomically(file,"t,value\n4,12\n0,-0\n-2,4\n2,8\n");
        const data::Schema schema{{{"t",data::Type::f64,"day",{}},{"value",data::Type::f64,"1",{}}},"t"};
        const auto table=data::load_file(file,schema);
        data::SeriesSpec spec{"t","value","day","1",data::Interpolation::hold};
        const data::ExogenousSeries held(table,spec);spec.interpolation=data::Interpolation::linear;
        const data::ExogenousSeries linear(data::load_file(file,schema),spec);
        require(held.value_at(-3)==4 && held.value_at(-1)==4 && same(held.value_at(0),-0.) &&
            same(held.value_at(1),-0.) && held.value_at(2)==8 && held.value_at(3)==8 && held.value_at(5)==12,"hold boundary convention");
        require(linear.value_at(-1)==2 && linear.value_at(1)==4 && linear.value_at(3)==10 &&
            same(linear.value_at(0),-0.) && linear.value_at(5)==12,"linear interpolation or exact knot bits");
        require(held.value_at(std::nextafter(2.,0.))==0 && held.value_at(std::nextafter(2.,3.))==8,"adjacent knot samples");
        const auto sampled=linear.sample({0,.25,21});
        require(sampled.file_hash==table.file_hash() && sampled.canonical_hash==table.canonical_hash(),"series receipt identity");
        for(std::size_t i=0;i<21;++i) require(sampled.times[i]==i*.25 && same(sampled.values[i],linear.value_at(i*.25)),"grid evaluation differs");
        const auto inf=std::numeric_limits<double>::infinity();
        for(const auto grid:std::vector<data::SampleGrid>{{-1,1,2},{0,0,2},{0,-1,2},{0,inf,2},{inf,1,2},{0,1,0},{0,1,1000001},
            {1e30,1,2},{std::numeric_limits<double>::max(),std::numeric_limits<double>::max(),2}})
            rejects("DATA_GRID",[&] { linear.sample(grid); });
        rejects("DATA_SERIES",[&] { linear.value_at(inf); });
        rejects("DATA_SERIES",[&] { linear.value_at(std::numeric_limits<double>::quiet_NaN()); });
        for(int kind=0;kind<4;++kind) {
            auto bad=spec;std::string code="DATA_SERIES";
            if(kind==0) bad.time_column="value";
            if(kind==1) { bad.value_column="missing";code="DATA_MAPPING"; }
            if(kind==2) { bad.time_unit="week";code="DATA_UNIT"; }
            if(kind==3) bad.interpolation=static_cast<data::Interpolation>(99);
            rejects(code,[&] { data::ExogenousSeries invalid(table,bad); });
        }
        const auto empty=root/"empty.csv";rt::write_text_atomically(empty,"t,value\n");
        rejects("DATA_SERIES",[&] { data::ExogenousSeries invalid(data::load_file(empty,schema),spec); });
        const auto singleton=root/"singleton.csv";rt::write_text_atomically(singleton,"t,value\n2,-0\n");
        const data::ExogenousSeries one(data::load_file(singleton,schema),spec);
        require(same(one.value_at(-10),-0.) && same(one.value_at(2),-0.) && same(one.value_at(100),-0.),"singleton endpoint hold");
        const auto wide=root/"wide.csv";rt::write_text_atomically(wide,"t,value\n-1.7976931348623157e308,-1.7976931348623157e308\n1.7976931348623157e308,1.7976931348623157e308\n");
        const data::ExogenousSeries extreme(data::load_file(wide,schema),spec);
        require(extreme.value_at(0)==0 && std::isfinite(extreme.value_at(1e308)),"overflowing interval interpolation");
        const auto parameters=root/"parameters.csv";rt::write_text_atomically(parameters,"group,growth,offset\nb,0.5,-0\na,0.25,2\n");
        const data::Schema params{{{"group",data::Type::string,"",{}},{"growth",data::Type::f64,"1/day",{}},{"offset",data::Type::f64,"kg",{}}},"group"};
        const std::vector<data::ParameterField> mappings{{"initial","offset","kg"},{"rate","growth","1/day"}};
        const data::ParameterTable bound(data::load_file(parameters,params),mappings);
        require(bound.values(std::string("a")).values==std::map<std::string,double>({{"initial",2},{"rate",.25}}),"keyed parameter mapping");
        require(same(bound.values(std::string("b")).values.at("initial"),-0.),"parameter zero bits");
        auto changed=bound.values(std::string("a"));changed.values["initial"]=999;
        require(bound.values(std::string("a")).values.at("initial")==2,"returned parameter mutation leaked into binding");
        rejects("DATA_KEY",[&] { bound.values(std::string("missing")); });
        rejects("DATA_KEY",[&] { bound.values(std::uint64_t{0}); });
        for(int kind=0;kind<4;++kind) {
            auto fields=mappings;std::string code="DATA_MAPPING";
            if(kind==0) fields.clear();
            if(kind==1) fields[1].parameter=fields[0].parameter;
            if(kind==2) fields[0].column="group";
            if(kind==3) { fields[0].unit="USD";code="DATA_UNIT"; }
            rejects(code,[&] { data::ParameterTable invalid(data::load_file(parameters,params),fields); });
        }
        // Read-only shared adapters feed private deterministic trajectories.
        rt::Experiment experiment{0,{},3};for(std::uint32_t i=0;i<64;++i) experiment.scenarios.push_back({i,{}});
        const auto run=[&](const rt::Scenario& scenario,auto,auto) {
            const auto inputs=bound.values(std::string(scenario.id%2?"b":"a"));
            ankurafathom::sd::Model model;const auto stock=model.add_stock("stock",inputs.values.at("initial"));
            double held_input=0.;const auto rate=inputs.values.at("rate");
            model.add_flow(ankurafathom::sd::Model::boundary,stock,[&](const auto&,double) { return rate*held_input; });
            std::vector<rt::Observation> rows;
            for(std::size_t i=0;i<sampled.times.size();++i) {
                rows.push_back({sampled.times[i],"stock",model.state()[stock]});
                if(i+1<sampled.times.size()) { held_input=sampled.values[i];model.step(sampled.times[i],.25); }
            }
            return rows;
        };
        const auto baseline=rt::run_experiment(experiment,run);
        // Exact left-grid sum for this piecewise-linear input is 38.5 day.
        for(const auto& trajectory:baseline) require(trajectory.observations.back().value==
            (trajectory.scenario%2?19.25:11.625),"bound SD trajectory differs from exact left-grid integral");
        for(const std::size_t threads:{8,32}) {
            const auto parallel=rt::run_experiment(experiment,run,{threads});
            for(std::size_t i=0;i<baseline.size();++i) for(std::size_t j=0;j<sampled.times.size();++j)
                require(same(parallel[i].observations[j].value,baseline[i].observations[j].value),"input binding changes with thread count");
        }
        std::cout<<"series, grids, parameter mappings, ownership and 192 trajectories at 1/8/32 threads passed\n";return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
