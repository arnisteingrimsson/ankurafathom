#include "ankurafathom/runtime/population_init.hpp"
#include "ankurafathom/abm/typed_population.hpp"
#include <iomanip>
#include <iostream>

struct Practice {};
int main(int argc,char** argv) {
    namespace data=ankurafathom::runtime::data;
    namespace abm=ankurafathom::abm;
    using Kind=ankurafathom::des::FieldKind;
    using Store=abm::PopulationStore<Practice>;
    try {
        if(argc!=2) throw std::invalid_argument("usage: fathom_population_from_data parameters.csv|parquet|arrow");
        const auto table=data::load_file(argv[1],{{{"practice",data::Type::string,"",{}},
            {"capacity",data::Type::i32,"people",{}},{"rate",data::Type::f64,"USD/hour",{}}},"practice"});
        Store store(17,{{"name",Kind::string},{"people",Kind::integer},{"rate",Kind::real}},10);
        const std::vector<data::PopulationField> fields{{"rate","rate","USD/hour"},{"name","practice",""},{"people","capacity","people"}};
        const auto receipt=data::initialize_population(store,table,fields);
        abm::TypedPopulation<Practice> model(std::move(store));
        model.add_phase([](Store::Reference ref,const Store& state) {
            auto record=state.record(ref);const auto rate=state.field_index("rate");
            record[rate]=std::get<double>(record[rate])+1.;return record;
        });
        model.step();
        std::cout<<"agent_id,practice,people,rate_after_one_synthetic_step\n"<<std::setprecision(17);
        for(const auto& entry:receipt.entries) std::cout<<entry.agent.id<<','<<std::get<std::string>(entry.source_key)<<','
            <<std::get<std::int64_t>(model.store().field(entry.agent,"people"))<<','
            <<std::get<double>(model.store().field(entry.agent,"rate"))<<'\n';
        return 0;
    } catch(const data::Error& error) { std::cerr<<error.code<<' '<<error.pointer<<": "<<error.what()<<'\n';return 1; }
      catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
