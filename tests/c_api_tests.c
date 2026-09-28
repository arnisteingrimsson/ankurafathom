#include "ankurafathom/c_api.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"C ABI assertion at line %d: %s (%s)\n",__LINE__,#x,fathom_last_error()->message);exit(1); } } while(0)
static const char experiment[]="{\"seed\":42,\"replications\":2,\"design\":{\"kind\":\"grid\",\"axes\":{\"decay_rate\":[0.1,0.2,0.3]}}}";
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static int ready=0;
struct worker { const fathom_model* model;int id; };
static void* worker_run(void* argument) {
    const struct worker* worker=argument;
    fathom_results* result=NULL;fathom_model* unused=NULL;
    const fathom_status expected=worker->id?FATHOM_IR_ERROR:FATHOM_INVALID_ARGUMENT;
    if(worker->id) CHECK(fathom_load_json("{",1,NULL,&unused)==expected);
    else CHECK(fathom_run(NULL,NULL,0,NULL,&result)==expected);
    CHECK(pthread_mutex_lock(&mutex)==0);++ready;CHECK(pthread_cond_broadcast(&condition)==0);
    while(ready<2) CHECK(pthread_cond_wait(&condition,&mutex)==0);
    CHECK(pthread_mutex_unlock(&mutex)==0);
    CHECK(fathom_last_error()->status==expected);
    fathom_run_options options=FATHOM_RUN_OPTIONS_INIT;options.threads=8;
    CHECK(fathom_run(worker->model,experiment,sizeof(experiment)-1,&options,&result)==FATHOM_OK);
    CHECK(fathom_last_error()->status==FATHOM_OK);
    uint64_t trajectories=0,rows=0;
    CHECK(fathom_results_size(result,&trajectories,&rows)==FATHOM_OK && trajectories==6 && rows==66);
    fathom_results_free(result);return NULL;
}
struct progress_state {
    pthread_t caller;
    uint64_t calls,total,cancel_at;
    int invalid,reenter;
    const fathom_model* model;
};
static int32_t progress_callback(uint64_t completed,uint64_t total,void* context) {
    struct progress_state* state=context;
    CHECK(pthread_equal(pthread_self(),state->caller));
    CHECK(completed==state->calls++ && total==state->total);
    if(state->reenter) {
        fathom_results* nested=NULL;
        CHECK(fathom_run(state->model,NULL,0,NULL,&nested)==FATHOM_OK);
        fathom_results_free(nested);nested=NULL;
        /* Leave a truncated nested diagnostic. Outer completion must replace it. */
        const char prefix[]="{\"seed\":0,\"replications\":1,\"scenarios\":[{\"id\":0,\"parameters\":{\"";
        const char suffix[]="\":1}}]}";
        char input[2048];memcpy(input,prefix,sizeof(prefix)-1);
        memset(input+sizeof(prefix)-1,'x',1000);
        memcpy(input+sizeof(prefix)-1+1000,suffix,sizeof(suffix)-1);
        CHECK(fathom_run(state->model,input,sizeof(prefix)-1+1000+sizeof(suffix)-1,NULL,&nested)==FATHOM_IR_ERROR);
        CHECK(fathom_last_error()->truncated!=0);
    }
    return state->invalid?2:completed!=state->cancel_at;
}
static void callbacks_contract(const fathom_model* model) {
    const uint32_t threads[]={1,8,32};
    fathom_results* baseline=NULL;
    CHECK(fathom_run(model,experiment,sizeof(experiment)-1,NULL,&baseline)==FATHOM_OK);
    const char* digest=NULL;CHECK(fathom_results_sha256(baseline,&digest)==FATHOM_OK);
    for(size_t t=0;t<3;++t) {
        fathom_run_options options=FATHOM_RUN_OPTIONS_INIT;options.threads=threads[t];
        for(int single=0;single<2;++single) {
            const uint64_t total=single?1:6;
            for(uint64_t cancel=0;cancel<=total+1;++cancel) {
                struct progress_state state={pthread_self(),0,total,cancel,0,1,model};
                fathom_run_callbacks hooks=FATHOM_RUN_CALLBACKS_INIT;
                hooks.progress=progress_callback;hooks.context=&state;
                fathom_results* result=NULL;
                fathom_status status=fathom_run_with_callbacks(model,single?NULL:experiment,
                    single?0:sizeof(experiment)-1,&options,&hooks,&result);
                CHECK(status==(cancel<=total?FATHOM_CANCELLED:FATHOM_OK));
                CHECK(fathom_last_error()->status==status && fathom_last_error()->truncated==0);
                CHECK(state.calls==(cancel<=total?cancel+1:total+1));
                if(cancel<=total) {
                    CHECK(result==NULL && strcmp(fathom_last_error()->code,"FATHOM_CANCELLED")==0);
                } else {
                    CHECK(result!=NULL);
                    if(!single) {
                        const char* actual=NULL;CHECK(fathom_results_sha256(result,&actual)==FATHOM_OK);
                        CHECK(strcmp(digest,actual)==0);
                    }
                    fathom_results_free(result);
                }
            }
        }
    }
    struct progress_state state={pthread_self(),0,6,UINT64_MAX,0,0,model};
    fathom_run_callbacks hooks=FATHOM_RUN_CALLBACKS_INIT;
    hooks.progress=progress_callback;hooks.context=&state;
    fathom_results* result=NULL;
    hooks.abi_version=0;
    CHECK(fathom_run_with_callbacks(model,NULL,0,NULL,&hooks,&result)==FATHOM_ABI_MISMATCH);
    hooks.abi_version=FATHOM_ABI_VERSION;hooks.reserved=1;
    CHECK(fathom_run_with_callbacks(model,NULL,0,NULL,&hooks,&result)==FATHOM_INVALID_ARGUMENT);
    hooks.reserved=0;
    CHECK(fathom_run_with_callbacks(model,"{",1,NULL,&hooks,&result)==FATHOM_IR_ERROR);
    CHECK(state.calls==0 && result==NULL);
    state.invalid=1;
    CHECK(fathom_run_with_callbacks(model,experiment,sizeof(experiment)-1,NULL,&hooks,&result)==FATHOM_INVALID_ARGUMENT);
    CHECK(state.calls==1 && result==NULL);
    CHECK(fathom_run_with_callbacks(model,experiment,sizeof(experiment)-1,NULL,NULL,&result)==FATHOM_OK);
    fathom_results* saved=result;
    CHECK(fathom_run_with_callbacks(model,NULL,0,NULL,&hooks,&result)==FATHOM_INVALID_ARGUMENT && result==saved);
    CHECK(state.calls==1);
    fathom_results_free(result);fathom_results_free(baseline);
}
int main(int argc,char** argv) {
    CHECK(argc==2);CHECK(fathom_abi_version()==FATHOM_ABI_VERSION);
    CHECK(fathom_last_error()->status==FATHOM_OK);
    fathom_model* model=NULL;fathom_results* result=NULL;
    CHECK(fathom_load_json(NULL,0,NULL,&model)==FATHOM_INVALID_ARGUMENT && model==NULL);
    CHECK(fathom_load_file(NULL,&model)==FATHOM_INVALID_ARGUMENT);
    CHECK(fathom_load_file(argv[1],NULL)==FATHOM_INVALID_ARGUMENT);
    CHECK(fathom_load_json("{}",2,"relative",&model)==FATHOM_INVALID_ARGUMENT);
    CHECK(fathom_load_json("{}",(size_t)256*1024*1024+1,NULL,&model)==FATHOM_INVALID_ARGUMENT);
    CHECK(fathom_load_json("{}\0garbage",10,NULL,&model)==FATHOM_IR_ERROR && model==NULL);
    CHECK(strcmp(fathom_last_error()->code,"IR_JSON")==0);
    CHECK(fathom_load_json("{}{}",4,NULL,&model)==FATHOM_IR_ERROR);
    CHECK(fathom_load_file(argv[1],&model)==FATHOM_OK && model!=NULL);
    callbacks_contract(model);
    fathom_model* original=model;
    CHECK(fathom_load_file(argv[1],&model)==FATHOM_INVALID_ARGUMENT && model==original);
    fathom_run_options options=FATHOM_RUN_OPTIONS_INIT;
    options.abi_version=0;
    CHECK(fathom_run(model,NULL,0,&options,&result)==FATHOM_ABI_MISMATCH && result==NULL);
    options.abi_version=FATHOM_ABI_VERSION;options.threads=0;
    CHECK(fathom_run(model,NULL,0,&options,&result)==FATHOM_INVALID_ARGUMENT);
    options.threads=257;CHECK(fathom_run(model,NULL,0,&options,&result)==FATHOM_INVALID_ARGUMENT);
    options.threads=1;options.reserved=1;CHECK(fathom_run(model,NULL,0,&options,&result)==FATHOM_INVALID_ARGUMENT);
    options.reserved=0;options.override_seed=2;CHECK(fathom_run(model,NULL,0,&options,&result)==FATHOM_INVALID_ARGUMENT);
    options.override_seed=0;options.seed=1;CHECK(fathom_run(model,NULL,0,&options,&result)==FATHOM_INVALID_ARGUMENT);
    CHECK(fathom_run(model,NULL,1,NULL,&result)==FATHOM_INVALID_ARGUMENT);
    CHECK(fathom_run(model,"",0,NULL,&result)==FATHOM_IR_ERROR);
    const char invalid[]="{\"seed\":0,\"replications\":1,\"scenarios\":[{\"id\":0,\"parameters\":{\"missing\":1}}]}";
    CHECK(fathom_run(model,invalid,sizeof(invalid)-1,NULL,&result)==FATHOM_IR_ERROR && result==NULL);
    CHECK(strcmp(fathom_last_error()->code,"IR_OVERRIDE")==0);
    CHECK(strcmp(fathom_last_error()->pointer,"/experiment/scenarios/0/parameters/missing")==0);
    const char overflow[]="{\"seed\":0,\"replications\":2,\"scenarios\":[{\"id\":0,\"parameters\":{\"decay_rate\":0.1}},{\"id\":1,\"parameters\":{\"decay_rate\":1e308}}]}";
    options.seed=0;options.threads=8;
    CHECK(fathom_run(model,overflow,sizeof(overflow)-1,&options,&result)!=FATHOM_OK && result==NULL);
    CHECK(fathom_run(model,invalid,sizeof(invalid)-1,NULL,&result)==FATHOM_IR_ERROR);
    /* Concurrent read-only model runs and thread-local errors. */
    pthread_t threads[2];struct worker workers[2]={{model,0},{model,1}};
    for(int i=0;i<2;++i) CHECK(pthread_create(&threads[i],NULL,worker_run,&workers[i])==0);
    for(int i=0;i<2;++i) CHECK(pthread_join(threads[i],NULL)==0);
    CHECK(strcmp(fathom_last_error()->code,"IR_OVERRIDE")==0);
    CHECK(fathom_run(model,NULL,0,NULL,&result)==FATHOM_OK);
    fathom_results* saved=result;
    CHECK(fathom_run(model,NULL,0,NULL,&result)==FATHOM_INVALID_ARGUMENT && result==saved);
    fathom_model_free(model);model=NULL;
    uint64_t trajectories=999,rows=999;
    CHECK(fathom_results_size(result,&trajectories,&rows)==FATHOM_OK && trajectories==1 && rows==11);
    double expected=100;
    const char* borrowed=NULL;
    for(uint64_t i=0;i<rows;++i) {
        fathom_observation row;
        CHECK(fathom_results_observation(result,i,&row)==FATHOM_OK);
        CHECK(row.scenario==0 && row.replication==0 && fabs(row.time-i*.1)<1e-14);
        CHECK(fabs(row.value-expected)<1e-10);
        CHECK(row.output_id_size==11 && strcmp(row.output_id,"material_ts")==0);
        if(i==0) borrowed=row.output_id;
        expected*=.98;
    }
    CHECK(strcmp(borrowed,"material_ts")==0);
    fathom_observation untouched;memset(&untouched,0xa5,sizeof(untouched));
    unsigned char prior[sizeof(untouched)];memcpy(prior,&untouched,sizeof(untouched));
    CHECK(fathom_results_observation(result,UINT64_MAX,&untouched)==FATHOM_OUT_OF_RANGE);
    CHECK(memcmp(prior,&untouched,sizeof(untouched))==0);
    CHECK(fathom_results_observation(NULL,0,&untouched)==FATHOM_INVALID_ARGUMENT);
    CHECK(fathom_results_observation(result,0,NULL)==FATHOM_INVALID_ARGUMENT);
    CHECK(fathom_results_size(NULL,&trajectories,&rows)==FATHOM_INVALID_ARGUMENT && trajectories==1 && rows==11);
    CHECK(fathom_results_size(result,&rows,&rows)==FATHOM_INVALID_ARGUMENT && rows==11);
    const char* digest=NULL;CHECK(fathom_results_sha256(result,&digest)==FATHOM_OK && strlen(digest)==64);
    CHECK(fathom_results_sha256(NULL,&digest)==FATHOM_INVALID_ARGUMENT && strlen(digest)==64);
    fathom_model_free(NULL);fathom_results_free(NULL);
    CHECK(fathom_last_error()->status==FATHOM_INVALID_ARGUMENT);
    const char* manifest=NULL;size_t manifest_size=0;
    CHECK(fathom_results_manifest(result,&manifest,&manifest_size)==FATHOM_OK);
    CHECK(manifest && manifest_size==strlen(manifest) && strstr(manifest,"\"manifest_version\":\"0.3\""));
    const char* original_manifest=manifest;const size_t original_size=manifest_size;
    CHECK(fathom_results_manifest(NULL,&manifest,&manifest_size)==FATHOM_INVALID_ARGUMENT);
    CHECK(manifest==original_manifest && manifest_size==original_size);
    CHECK(fathom_results_manifest(result,NULL,&manifest_size)==FATHOM_INVALID_ARGUMENT && manifest_size==original_size);
    CHECK(fathom_results_manifest(result,&manifest,NULL)==FATHOM_INVALID_ARGUMENT && manifest==original_manifest);
    CHECK(fathom_results_size(result,&trajectories,&rows)==FATHOM_OK);
    CHECK(fathom_results_manifest(result,&manifest,&manifest_size)==FATHOM_OK && manifest==original_manifest);
    CHECK(manifest_size==original_size && strstr(manifest,digest));
    fathom_results_free(result);
    /* Input storage is caller-owned and may be discarded immediately after load. */
    FILE* file=fopen(argv[1],"rb");CHECK(file!=NULL);CHECK(fseek(file,0,SEEK_END)==0);
    const long size=ftell(file);CHECK(size>0);rewind(file);
    char* json=malloc((size_t)size);CHECK(json!=NULL);
    CHECK(fread(json,1,(size_t)size,file)==(size_t)size);CHECK(fclose(file)==0);
    CHECK(fathom_load_json(json,(size_t)size,NULL,&model)==FATHOM_OK);
    memset(json,'?',(size_t)size);free(json);result=NULL;
    CHECK(fathom_run(model,experiment,sizeof(experiment)-1,NULL,&result)==FATHOM_OK);
    CHECK(fathom_results_size(result,&trajectories,&rows)==FATHOM_OK && rows==66 && trajectories==6);
    fathom_model_free(model);fathom_results_free(result);
    model=NULL;result=NULL;
    CHECK(fathom_load_file(argv[1],&model)==FATHOM_OK);
    const char prefix[]="{\"seed\":0,\"replications\":1,\"scenarios\":[{\"id\":0,\"parameters\":{\"";
    const char suffix[]="\":1}}]}";
    char long_error[4096];memcpy(long_error,prefix,sizeof(prefix)-1);
    memset(long_error+sizeof(prefix)-1,'x',2000);
    memcpy(long_error+sizeof(prefix)-1+2000,suffix,sizeof(suffix)-1);
    CHECK(fathom_run(model,long_error,sizeof(prefix)-1+2000+sizeof(suffix)-1,NULL,&result)==FATHOM_IR_ERROR);
    CHECK((fathom_last_error()->truncated&2)!=0 && fathom_last_error()->pointer[511]=='\0');
    const char nul_name[]="{\"seed\":0,\"replications\":1,\"scenarios\":[{\"id\":0,\"parameters\":{\"bad\\u0000key\":1}}]}";
    CHECK(fathom_run(model,nul_name,sizeof(nul_name)-1,NULL,&result)==FATHOM_IR_ERROR);
    CHECK((fathom_last_error()->truncated&2)!=0);
    CHECK(strcmp(fathom_last_error()->pointer,"/experiment/scenarios/0/parameters/bad")==0);
    fathom_model_free(model);
    puts("C11 ABI: callback ordering/cancellation/reentrancy, analytic recurrence, ownership, failure outputs, option checks and concurrent TLS isolation pass");
    return 0;
}
