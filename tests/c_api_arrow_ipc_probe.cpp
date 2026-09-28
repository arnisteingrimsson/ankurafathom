// Independent SDK consumer. Include the SDK first to check standard header guards.
#include <arrow/c/bridge.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/record_batch.h>
#include "ankurafathom/c_api.h"
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

static void check(const arrow::Status& status) {
    if(!status.ok()) throw std::runtime_error(status.ToString());
}
static void check(fathom_status status) {
    if(status!=FATHOM_OK) throw std::runtime_error(fathom_last_error()->message);
}
int main(int argc,char** argv) {
    try {
        if(argc!=5) throw std::runtime_error("usage: probe MODEL EXPERIMENT_OR_- THREADS OUTPUT");
        fathom_model* raw_model=nullptr;check(fathom_load_file(argv[1],&raw_model));
        std::unique_ptr<fathom_model,decltype(&fathom_model_free)> model(raw_model,fathom_model_free);
        std::string experiment;
        if(std::string(argv[2])!="-") {
            std::ifstream input(argv[2]);if(!input) throw std::runtime_error("cannot read experiment");
            experiment.assign(std::istreambuf_iterator<char>(input),{});
        }
        fathom_run_options options=FATHOM_RUN_OPTIONS_INIT;
        options.threads=static_cast<uint32_t>(std::stoul(argv[3]));
        fathom_results* raw_results=nullptr;
        check(fathom_run(model.get(),experiment.empty()?nullptr:experiment.data(),experiment.size(),&options,&raw_results));
        std::unique_ptr<fathom_results,decltype(&fathom_results_free)> results(raw_results,fathom_results_free);
        struct Export {
            ArrowArrayStream value{};
            ~Export() { if(value.release) value.release(&value); }
        } stream;
        check(fathom_results_arrow(results.get(),&stream.value));
        results.reset();model.reset();
        auto imported=arrow::ImportRecordBatchReader(&stream.value);check(imported.status());
        if(stream.value.release) throw std::runtime_error("import did not take stream ownership");
        auto reader=std::move(imported).ValueOrDie();
        auto opened=arrow::io::FileOutputStream::Open(argv[4]);check(opened.status());
        auto sink=std::move(opened).ValueOrDie();
        auto created=arrow::ipc::MakeFileWriter(sink,reader->schema());check(created.status());
        auto writer=std::move(created).ValueOrDie();
        while(true) {
            std::shared_ptr<arrow::RecordBatch> batch;check(reader->ReadNext(&batch));
            if(!batch) break;
            check(batch->ValidateFull());check(writer->WriteRecordBatch(*batch));
        }
        check(writer->Close());check(sink->Close());check(reader->Close());
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
    return 0;
}
