/* Pure C11 consumer: no Arrow SDK headers or C++ linkage required. */
#include "ankurafathom/c_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"Arrow C assertion at %d: %s (%s)\n",__LINE__,#x,fathom_last_error()->message);exit(1); } } while(0)
static void unused_release(struct ArrowArrayStream* stream) { (void)stream; }
#if FATHOM_HAS_ARROW
struct expected { fathom_observation row; char* name; };
static void check_schema(const struct ArrowSchema* schema) {
    const char* names[]={"scenario","replication","time","output_id","value"};
    const char* formats[]={"I","I","g","u","g"};
    CHECK(schema->release && strcmp(schema->format,"+s")==0 && schema->n_children==5);
    CHECK(schema->dictionary==NULL && schema->metadata!=NULL);
    for(int i=0;i<5;++i) {
        const struct ArrowSchema* field=schema->children[i];
        CHECK(field->release && strcmp(field->name,names[i])==0 && strcmp(field->format,formats[i])==0);
        CHECK(!(field->flags & ARROW_FLAG_NULLABLE) && !field->dictionary && field->n_children==0);
    }
}
static void check_batch(const struct ArrowArray* batch,const struct expected* expected,uint64_t start,uint64_t count) {
    CHECK(batch->release && batch->n_children==5 && batch->null_count==0 && batch->length>0);
    CHECK((uint64_t)batch->length<=count-start);
    for(int c=0;c<5;++c) {
        CHECK(batch->children[c]->null_count==0);
        CHECK(batch->children[c]->n_buffers==(c==3?3:2));
    }
    for(int64_t r=0;r<batch->length;++r) {
        const fathom_observation* row=&expected[start+(uint64_t)r].row;
        const struct ArrowArray* a=batch->children[0];
        CHECK(((const uint32_t*)a->buffers[1])[a->offset+batch->offset+r]==row->scenario);
        a=batch->children[1];
        CHECK(((const uint32_t*)a->buffers[1])[a->offset+batch->offset+r]==row->replication);
        a=batch->children[2];
        CHECK(memcmp((const double*)a->buffers[1]+a->offset+batch->offset+r,&row->time,sizeof(double))==0);
        a=batch->children[4];
        CHECK(memcmp((const double*)a->buffers[1]+a->offset+batch->offset+r,&row->value,sizeof(double))==0);
        a=batch->children[3];
        const int32_t* offsets=a->buffers[1];const int64_t index=a->offset+batch->offset+r;
        CHECK((size_t)(offsets[index+1]-offsets[index])==row->output_id_size);
        CHECK(memcmp((const char*)a->buffers[2]+offsets[index],expected[start+(uint64_t)r].name,row->output_id_size)==0);
    }
}
static void exercise(fathom_results* results,int multiple_batches) {
    uint64_t trajectories=0,count=0;
    CHECK(fathom_results_size(results,&trajectories,&count)==FATHOM_OK && count>0);
    struct expected* rows=calloc((size_t)count,sizeof(*rows));CHECK(rows);
    for(uint64_t i=0;i<count;++i) {
        CHECK(fathom_results_observation(results,i,&rows[i].row)==FATHOM_OK);
        rows[i].name=malloc(rows[i].row.output_id_size);CHECK(rows[i].name);
        memcpy(rows[i].name,rows[i].row.output_id,rows[i].row.output_id_size);
    }
    struct ArrowArrayStream lineage={0};
    CHECK(fathom_results_arrow_with_manifest(results,&lineage)==FATHOM_OK);
    struct ArrowArrayStream lineage_saved=lineage;
    CHECK(fathom_results_arrow_with_manifest(results,&lineage)==FATHOM_INVALID_ARGUMENT);
    CHECK(memcmp(&lineage,&lineage_saved,sizeof(lineage))==0);
    struct ArrowArrayStream first={0},second={0},early={0};
    CHECK(fathom_results_arrow(results,&first)==FATHOM_OK);
    CHECK(fathom_results_arrow(results,&second)==FATHOM_OK);
    CHECK(fathom_results_arrow(results,&early)==FATHOM_OK);
    struct ArrowArrayStream saved=first;
    CHECK(fathom_results_arrow(results,&first)==FATHOM_INVALID_ARGUMENT);
    CHECK(memcmp(&saved,&first,sizeof(first))==0);
    early.release(&early);CHECK(!early.release);
    /* A released destination can be reused even if other fields are stale. */
    CHECK(fathom_results_arrow(results,&early)==FATHOM_OK);
    early.release(&early);CHECK(!early.release);
    fathom_results_free(results);
    struct ArrowSchema lineage_schema={0};struct ArrowArray lineage_batch={0};
    CHECK(lineage.get_schema(&lineage,&lineage_schema)==0 && lineage_schema.n_children==7);
    CHECK(lineage.get_next(&lineage,&lineage_batch)==0 && lineage_batch.n_children==7);
    lineage.release(&lineage);
    CHECK(strcmp(lineage_schema.children[5]->name,"manifest_id")==0);
    CHECK(strcmp(lineage_schema.children[6]->name,"scenario_parameters")==0);
    CHECK(lineage_batch.children[5]->null_count==0 && lineage_batch.length>0);
    lineage_schema.release(&lineage_schema);lineage_batch.release(&lineage_batch);
    struct ArrowSchema schema={0},again={0};struct ArrowArray retained={0};
    CHECK(first.get_schema(&first,&schema)==0 && first.get_schema(&first,&again)==0);
    CHECK(first.get_next(&first,&retained)==0 && retained.release);
    first.release(&first);CHECK(!first.release);
    check_schema(&schema);check_schema(&again);check_batch(&retained,rows,0,count);
    schema.release(&schema);again.release(&again);retained.release(&retained);
    CHECK(!schema.release && !again.release && !retained.release);
    CHECK(second.get_schema(&second,&schema)==0);check_schema(&schema);
    uint64_t read=0;int batches=0;
    while(1) {
        struct ArrowArray batch={0};CHECK(second.get_next(&second,&batch)==0);
        if(!batch.release) break;
        check_batch(&batch,rows,read,count);read+=(uint64_t)batch.length;++batches;
        batch.release(&batch);CHECK(!batch.release);
    }
    CHECK(read==count && (multiple_batches?batches>1:batches==1));
    struct ArrowArray end={0};CHECK(second.get_next(&second,&end)==0 && !end.release);
    second.release(&second);CHECK(!second.release);
    check_schema(&schema);schema.release(&schema);
    for(uint64_t i=0;i<count;++i) free(rows[i].name);
    free(rows);
}
#endif
int main(int argc,char** argv) {
    CHECK(argc==2);fathom_model* model=NULL;fathom_results* results=NULL;
    CHECK(fathom_load_file(argv[1],&model)==FATHOM_OK);
    CHECK(fathom_run(model,NULL,0,NULL,&results)==FATHOM_OK);
    fathom_model_free(model);model=NULL;
    struct ArrowArrayStream stream={0},saved=stream;
    CHECK(fathom_results_arrow(NULL,&stream)==FATHOM_INVALID_ARGUMENT);
    CHECK(memcmp(&saved,&stream,sizeof(stream))==0);
    CHECK(fathom_results_arrow(results,NULL)==FATHOM_INVALID_ARGUMENT);
    stream.release=unused_release;saved=stream;
    CHECK(fathom_results_arrow(results,&stream)==FATHOM_INVALID_ARGUMENT);
    CHECK(memcmp(&saved,&stream,sizeof(stream))==0);
#if FATHOM_HAS_ARROW
    exercise(results,0);results=NULL;
    /* Multiple batches and finite binary64 edge values, including -0 at t=0. */
    const char json[]="{\"ir_version\":\"0.1\",\"name\":\"arrow_bits\","
      "\"time\":{\"unit\":\"day\",\"dt\":1,\"horizon\":20000},\"parameters\":[],"
      "\"components\":[{\"id\":\"z\",\"kind\":\"stock\",\"init\":-0.0,\"unit\":\"kg\"},"
      "{\"id\":\"s\",\"kind\":\"stock\",\"init\":5e-324,\"unit\":\"kg\"},"
      "{\"id\":\"m\",\"kind\":\"stock\",\"init\":1.7976931348623157e308,\"unit\":\"kg\"},"
      "{\"id\":\"n\",\"kind\":\"stock\",\"init\":-123.25,\"non_negative\":false,\"unit\":\"kg\"}],"
      "\"outputs\":[{\"id\":\"zero\",\"stock\":\"z\"},{\"id\":\"subnormal\",\"stock\":\"s\"},"
      "{\"id\":\"maximum\",\"stock\":\"m\"},{\"id\":\"negative\",\"stock\":\"n\"}]}";
    CHECK(fathom_load_json(json,sizeof(json)-1,NULL,&model)==FATHOM_OK);
    CHECK(fathom_run(model,NULL,0,NULL,&results)==FATHOM_OK);fathom_model_free(model);
    fathom_observation row={0};uint64_t bits=0;
    CHECK(fathom_results_observation(results,0,&row)==FATHOM_OK);
    memcpy(&bits,&row.value,sizeof(bits));CHECK(bits==UINT64_C(0x8000000000000000));
    CHECK(fathom_results_observation(results,1,&row)==FATHOM_OK);
    memcpy(&bits,&row.value,sizeof(bits));CHECK(bits==1);
    exercise(results,1);
    puts("Arrow C11 stream: schema, 80015 exact rows, multiple batches, independent exports, release order and invalid destinations pass");
#else
    stream=(struct ArrowArrayStream){0};stream.private_data=&stream;saved=stream;
    CHECK(fathom_results_arrow_with_manifest(results,&stream)==FATHOM_UNAVAILABLE);
    CHECK(memcmp(&saved,&stream,sizeof(stream))==0);
    CHECK(fathom_results_arrow(results,&stream)==FATHOM_UNAVAILABLE);
    CHECK(memcmp(&saved,&stream,sizeof(stream))==0);
    CHECK(fathom_last_error()->status==FATHOM_UNAVAILABLE);
    CHECK(strcmp(fathom_last_error()->code,"FATHOM_UNAVAILABLE")==0);
    uint64_t trajectories=0,rows=0;
    CHECK(fathom_results_size(results,&trajectories,&rows)==FATHOM_OK && rows==11);
    CHECK(fathom_last_error()->status==FATHOM_OK);
    fathom_results_free(results);
    puts("Arrow C11 stream: unavailable status, preserved destinations and usable CSV-only results pass");
#endif
    return 0;
}
