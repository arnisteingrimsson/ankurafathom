#include "ankurafathom/runtime/outputs.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include <bit>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#if FATHOM_HAS_ARROW
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <parquet/arrow/reader.h>
#endif

namespace rt=ankurafathom::runtime;
namespace fs=std::filesystem;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(F f) {
    bool caught=false;try { f(); } catch(const std::exception&) { caught=true; }
    require(caught,"invalid result publication accepted");
}
std::string read(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
void no_temporary(const fs::path& root) {
    for(const auto& entry:fs::directory_iterator(root))
        require(!entry.path().filename().string().starts_with(".fathom-"),"leaked temporary output");
}
const std::vector<std::uint64_t> bits={0,0x8000000000000000ULL,1,0x8000000000000001ULL,
    0x0010000000000000ULL,0x7fefffffffffffffULL,0xffefffffffffffffULL,
    0x3fb999999999999aULL,0x3ff0000000000001ULL};
std::vector<rt::Trajectory> fixture() {
    std::vector<rt::Trajectory> result;
    for(auto address:{std::pair{0U,0U},std::pair{65535U,65535U}}) {
        rt::Trajectory trajectory{address.first,address.second,{}};
        for(std::size_t i=0;i<bits.size();++i)
            trajectory.observations.push_back({i*.125,"value,\"quoted\"\n\r\t\xc3\xa9",std::bit_cast<double>(bits[i])});
        result.push_back(std::move(trajectory));
    }
    return result;
}
void atomic_failure(const fs::path& root) {
    const auto path=root/"sentinel.csv";rt::write_text_atomically(path,"previous result");
    for(int i=0;i<10;++i) rejects([&] { rt::write_file_atomically(path,[](int fd) {
        require(::write(fd,"partial",7)==7,"test write failed");throw std::runtime_error("injected writer failure");
    }); });
    require(read(path)=="previous result","writer failure changed destination");no_temporary(root);
    const auto directory=root/"directory";fs::create_directories(directory);
    rt::write_text_atomically(directory/"keep","intact");
    rejects([&] { rt::write_text_atomically(directory,"replacement"); });
    require(read(directory/"keep")=="intact","rename failure changed directory");no_temporary(root);
    rejects([&] { rt::write_text_atomically(root/"absent"/"file","x"); });
    rt::write_text_atomically(path,std::string_view("a\0b",3));
    require(read(path)==std::string("a\0b",3),"binary file publication truncated NUL");
}
struct DecimalComma:std::numpunct<char> { char do_decimal_point() const override { return ','; } };
void csv_and_validation(const fs::path& root) {
    const auto rows=fixture();
    const auto csv=rt::observations_csv(rows);
    const std::vector<rt::Trajectory> source_clock{{0,0,{{-2,"x",3},{-1,"x",4},{0,"x",5}}}};
    require(rt::observations_csv(source_clock)=="scenario,replication,time,output_id,value\n0,0,-2,x,3\n0,0,-1,x,4\n0,0,0,x,5\n",
        "negative source times changed in CSV");
#if FATHOM_HAS_ARROW
    if(rt::arrow_output_available()) {
        const auto table=rt::observation_table(source_clock);
        const auto times=std::static_pointer_cast<arrow::DoubleArray>(table->column(2)->chunk(0));
        require(times->Value(0)==-2 && times->Value(1)==-1 && times->Value(2)==0,"negative source times changed in Arrow");
    }
#endif
    require(csv.find("0,0,0,\"value,\"\"quoted\"\"\n\r\t\xc3\xa9\",0\n")!=std::string::npos,"CSV escaping changed identifier");
    require(csv.find(",-0\n")!=std::string::npos,"CSV lost negative zero");
    const auto old=std::locale::global(std::locale(std::locale::classic(),new DecimalComma));
    const auto local=rt::observations_csv(rows);std::locale::global(old);
    require(local==csv,"global locale changed numeric output");
    rt::write_observations(root/"values.csv",rt::OutputFormat::csv,rows);
    require(read(root/"values.csv")==csv,"CSV file mismatch");
    require(rt::observations_csv({})=="scenario,replication,time,output_id,value\n","empty result schema");
    require(rt::observations_csv(std::span(rows).first(1),true).starts_with("time,output_id,value\n"),"legacy single CSV schema");
    rejects([&] { rt::observations_csv(rows,true); });
    rejects([&] { rt::observations_csv(std::span(rows).last(1),true); });
    rejects([&] { rt::observations_csv({},true); });
    rejects([&] { rt::parse_output_format("feather"); });
    require(rt::infer_output_format("result.parquet")==rt::OutputFormat::parquet &&
        rt::infer_output_format("result.arrow")==rt::OutputFormat::arrow_ipc &&
        rt::infer_output_format("result.ipc")==rt::OutputFormat::arrow_ipc &&
        rt::infer_output_format("result.data")==rt::OutputFormat::csv,"format inference");
    const auto sentinel=root/"validation.csv";rt::write_text_atomically(sentinel,"intact");
    for(int kind=0;kind<9;++kind) {
        auto broken=rows;
        switch(kind) {
            case 0: broken[0].scenario=65536;break;
            case 1: broken[0].replication=65536;break;
            case 2: std::swap(broken[0],broken[1]);break;
            case 3: broken[1]=broken[0];break;
            case 4: broken[0].observations[0].time=-std::numeric_limits<double>::infinity();break;
            case 5: broken[0].observations[0].value=std::numeric_limits<double>::infinity();break;
            case 6: broken[0].observations[0].time=std::numeric_limits<double>::quiet_NaN();break;
            case 7: broken[0].observations[0].output_id="";break;
            case 8: broken[0].observations.push_back(broken[0].observations[0]);break;
        }
        rejects([&] { rt::write_observations(sentinel,rt::OutputFormat::csv,broken); });
        require(read(sentinel)=="intact","invalid observations changed destination");
        if(rt::arrow_output_available()) rejects([&] { rt::observation_table(broken); });
    }
    rejects([&] { rt::write_observations(sentinel,static_cast<rt::OutputFormat>(100),rows); });
    no_temporary(root);
}
#if FATHOM_HAS_ARROW
template<class T> T unwrap(arrow::Result<T> value) {
    if(!value.ok()) throw std::runtime_error(value.status().ToString());return std::move(value).ValueUnsafe();
}
void same(const arrow::Table& table,const std::vector<rt::Trajectory>& rows) {
    const auto expected=rt::observation_table(rows);
    require(table.schema()->Equals(expected->schema(),true) && table.Equals(*expected),"Arrow roundtrip schema/rows changed");
    const auto combined=unwrap(table.CombineChunks());
    auto values=std::static_pointer_cast<arrow::DoubleArray>(combined->column(4)->chunk(0));
    auto times=std::static_pointer_cast<arrow::DoubleArray>(combined->column(2)->chunk(0));
    std::int64_t index=0;
    for(const auto& t:rows) for(const auto& r:t.observations) {
        require(std::bit_cast<std::uint64_t>(values->Value(index))==std::bit_cast<std::uint64_t>(r.value),"roundtrip value bit mismatch");
        require(std::bit_cast<std::uint64_t>(times->Value(index))==std::bit_cast<std::uint64_t>(r.time),"roundtrip time bit mismatch");++index;
    }
}
void columnar(const fs::path& root) {
    const auto rows=fixture();same(*rt::observation_table(rows),rows);
    for(const auto format:{rt::OutputFormat::parquet,rt::OutputFormat::arrow_ipc}) {
        const auto suffix=format==rt::OutputFormat::parquet?"parquet":"arrow";
        const auto path=root/(std::string("values.")+suffix);
        rt::write_observations(path,format,rows);
        auto file=unwrap(arrow::io::ReadableFile::Open(path.string()));
        std::shared_ptr<arrow::Table> table;
        if(format==rt::OutputFormat::parquet) {
            auto reader=unwrap(parquet::arrow::OpenFile(file,arrow::default_memory_pool()));
            table=unwrap(reader->ReadTable());
        } else {
            auto reader=unwrap(arrow::ipc::RecordBatchFileReader::Open(file));table=unwrap(reader->ToTable());
        }
        same(*table,rows);
        rt::write_observations(root/(std::string("empty.")+suffix),format,{});
        rejects([&] { rt::write_observations(root/"directory",format,rows); });
        require(read(root/"directory"/"keep")=="intact","failed columnar rename changed directory");
        auto invalid=rows;invalid[0].observations[0].output_id=std::string("\xff",1);
        const auto original=read(path);
        rejects([&] { rt::write_observations(path,format,invalid); });
        require(read(path)==original,"invalid UTF-8 changed existing result");
        no_temporary(root);
    }
    // Force a row group / IPC batch boundary; zero and subnormal values must
    // survive encoding even where numeric equality alone cannot detect loss.
    std::vector<rt::Trajectory> large{{65535,65535,{}}};
    for(std::size_t i=0;i<65539;++i) large[0].observations.push_back({i*.125,"boundary",std::bit_cast<double>(bits[i%bits.size()])});
    rt::write_observations(root/"boundary.parquet",rt::OutputFormat::parquet,large);
    rt::write_observations(root/"boundary.arrow",rt::OutputFormat::arrow_ipc,large);
}
#endif
int main(int argc,char** argv) {
    try {
        require(argc==2,"usage: runtime_outputs_tests artifact-directory");const fs::path root=argv[1];fs::create_directories(root);
        atomic_failure(root);csv_and_validation(root);
#if FATHOM_HAS_ARROW
        require(rt::arrow_output_available(),"Arrow feature flag mismatch");columnar(root);
#else
        require(!rt::arrow_output_available(),"CSV-only feature flag mismatch");
        rejects([&] { rt::observation_table(fixture()); });
        const auto path=root/"unavailable.parquet";rt::write_text_atomically(path,"intact");
        rejects([&] { rt::write_observations(path,rt::OutputFormat::parquet,fixture()); });
        require(read(path)=="intact","unavailable format changed destination");
#endif
        std::cout<<"output schema, bit preservation, validation and atomic publication passed\n";return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
