#include "ankurafathom/c_api.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char* read_bytes(const char* path,size_t* size) {
    FILE* file=fopen(path,"rb");if(!file) return NULL;
    if(fseek(file,0,SEEK_END)!=0) { fclose(file);return NULL; }
    long length=ftell(file);if(length<0) { fclose(file);return NULL; }rewind(file);
    char* bytes=malloc((size_t)length+1);if(!bytes) { fclose(file);return NULL; }
    *size=fread(bytes,1,(size_t)length,file);fclose(file);
    if(*size!=(size_t)length) { free(bytes);return NULL; }return bytes;
}
int main(int argc,char** argv) {
    if(argc!=7 && !(argc==8 && strcmp(argv[7],"--manifest")==0)) return 2;
    fathom_model* model=NULL;fathom_results* results=NULL;
    fathom_status status;
    if(strcmp(argv[4],"json")==0) {
        size_t size=0;char* json=read_bytes(argv[1],&size);if(!json) return 2;
        status=fathom_load_json(json,size,argv[5],&model);memset(json,'?',size);free(json);
    } else status=fathom_load_file(argv[1],&model);
    if(status!=FATHOM_OK) goto fail;
    size_t experiment_size=0;char* experiment=NULL;
    if(strcmp(argv[2],"-")!=0) { experiment=read_bytes(argv[2],&experiment_size);if(!experiment) { fathom_model_free(model);return 2; } }
    fathom_run_options options=FATHOM_RUN_OPTIONS_INIT;options.threads=(uint32_t)strtoul(argv[3],NULL,10);
    if(strcmp(argv[6],"-")!=0) { options.override_seed=1;options.seed=strtoull(argv[6],NULL,10); }
    status=fathom_run(model,experiment,experiment_size,&options,&results);free(experiment);
    fathom_model_free(model);model=NULL;
    if(status!=FATHOM_OK) goto fail;
    if(argc==8) {
        const char* manifest=NULL;size_t size=0;
        if(fathom_results_manifest(results,&manifest,&size)!=FATHOM_OK) goto fail;
        if(fwrite(manifest,1,size,stdout)!=size) { fathom_results_free(results);return 1; }
        putchar('\n');fathom_results_free(results);return 0;
    }
    uint64_t trajectories=0,rows=0;const char* digest=NULL;
    if(fathom_results_size(results,&trajectories,&rows)!=FATHOM_OK || fathom_results_sha256(results,&digest)!=FATHOM_OK) goto fail;
    printf("%"PRIu64" %"PRIu64" %s\n",trajectories,rows,digest);
    for(uint64_t i=0;i<rows;++i) {
        fathom_observation row;uint64_t time_bits,value_bits;
        if(fathom_results_observation(results,i,&row)!=FATHOM_OK) goto fail;
        memcpy(&time_bits,&row.time,sizeof(time_bits));memcpy(&value_bits,&row.value,sizeof(value_bits));
        printf("%u %u %016"PRIx64" %016"PRIx64" ",row.scenario,row.replication,time_bits,value_bits);
        for(size_t j=0;j<row.output_id_size;++j) printf("%02x",(unsigned char)row.output_id[j]);
        putchar('\n');
    }
    fathom_results_free(results);return 0;
fail:
    fprintf(stderr,"%s %s %s\n",fathom_last_error()->code,fathom_last_error()->pointer,fathom_last_error()->message);
    fathom_model_free(model);fathom_results_free(results);return 1;
}
