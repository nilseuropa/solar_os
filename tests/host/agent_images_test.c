#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_memory.h"
#include "solar_os_agent_images.h"
#include "solar_os_agent_provider.h"
#include "solar_os_json.h"
#include "cJSON.h"

static int allocations, fail_after=-1, requests;
static bool cancelled;
static int http_status=200;
void *solar_os_memory_alloc(size_t n,solar_os_memory_class_t kind,const char *tag)
{
    (void)kind;(void)tag;
    if (fail_after==0) return NULL;
    if (fail_after>0) fail_after--;
    void *p=malloc(n);if(p) allocations++;return p;
}
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t kind,const char *tag)
{ void *p=solar_os_memory_alloc(n*size,kind,tag);if(p)memset(p,0,n*size);return p; }
void solar_os_memory_free(void *p) { if(p){assert(allocations>0);allocations--;free(p);} }
size_t strlcpy(char *out,const char *in,size_t n)
{ size_t size=strlen(in);if(n){size_t copy=size<n-1?size:n-1;memcpy(out,in,copy);out[copy]=0;}return size; }
const char *esp_err_to_name(esp_err_t e) { (void)e;return "test-error"; }
void solar_os_agent_provider_note_memory(void) {}
bool solar_os_agent_provider_cancel_requested(void) { return cancelled; }
void solar_os_agent_provider_bind_request(solar_os_http_request_t *r) { (void)r; }
void solar_os_agent_provider_unbind_request(solar_os_http_request_t *r) { (void)r; }
struct solar_os_http_request { solar_os_http_request_options_t options; };
esp_err_t solar_os_http_request_create(const solar_os_http_request_options_t *options,solar_os_http_request_t **out)
{ *out=malloc(sizeof(**out));assert(*out);(*out)->options=*options;requests++;return ESP_OK; }
esp_err_t solar_os_http_request_destroy(solar_os_http_request_t *r) { free(r);return ESP_OK; }
esp_err_t solar_os_http_request_perform(solar_os_http_request_t *r,solar_os_http_response_t *response)
{
    *response=(solar_os_http_response_t){.status_code=http_status};
    const char *data=http_status==200 ?
        "data: {\"choices\":[{\"delta\":{\"content\":\"image seen\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n" :
        "{\"error\":{\"message\":\"vision unsupported\"}}";
    if (cancelled) return ESP_ERR_INVALID_STATE;
    const solar_os_http_event_t event={.type=SOLAR_OS_HTTP_EVENT_DATA,.status_code=http_status,
        .data=(const uint8_t *)data,.data_len=strlen(data)};
    return r->options.event_handler(&event,r->options.user_data);
}
#include "../../src/services/solar_os_agent_openai.c"

static esp_err_t receive(const solar_os_agent_event_t *event,void *user)
{ if(event->type==SOLAR_OS_AGENT_EVENT_TEXT_DELTA)strcat(user,event->text);return ESP_OK; }
static void record(FILE *file,const solar_os_agent_provider_config_t *config,solar_os_agent_provider_turn_t *turn)
{
    char *body=NULL;size_t length=0;
    assert(!agent_openai_build_body(config,turn,&body,&length));
    cJSON *parsed=cJSON_Parse(body);assert(parsed && strlen(body)==length);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(parsed,"parallel_tool_calls")));
    cJSON_Delete(parsed);assert(fwrite(body,1,length,file)==length);fputc('\n',file);solar_os_memory_free(body);
}
int main(int argc,char **argv)
{
    assert(argc==2);assert(!solar_os_json_init());
    FILE *file=fopen(argv[1],"wb");assert(file);
    uint8_t jpeg[]={255,216,255,0,1};
    char source[]="camera0";
    solar_os_media_sample_metadata_t metadata={.source=source,.sequence=9,.timestamp_us=123,.width=640,.height=480};
    solar_os_media_sample_t *images[2]={0};
    assert(solar_os_media_sample_copy_image(jpeg,0,NULL,&images[0])==ESP_ERR_INVALID_SIZE && !images[0]);
    assert(solar_os_media_sample_copy_image(jpeg,SOLAR_OS_MEDIA_SAMPLE_MAX_BYTES+1,NULL,&images[0])==ESP_ERR_INVALID_SIZE && !images[0]);
    assert(!solar_os_media_sample_copy_image(jpeg,sizeof(jpeg),&metadata,&images[0]));
    jpeg[0]=0;source[0]='x';
    const solar_os_media_sample_image_t *view=solar_os_media_sample_image(images[0]);
    assert(view->data[0]==255 && !strcmp(view->metadata.source,"camera0") && view->metadata.sequence==9);
    solar_os_media_sample_retain(images[0]);solar_os_media_sample_release(images[0]);
    const uint8_t png[]={137,80,78,71,13,10,26,10};
    assert(!solar_os_media_sample_copy_image(png,sizeof(png),NULL,&images[1]));
    solar_os_agent_provider_config_t config={.endpoint="http://fixture/v1/chat/completions",.model="vision-test"};
    const solar_os_agent_history_message_t history={.role=SOLAR_OS_AGENT_MESSAGE_ASSISTANT,.text="past \"answer\""};
    solar_os_agent_provider_turn_t turn={.prompt="What is \"here\"?\n\t",.images=images,.image_count=2,
        .history=&history,.history_count=1};
    record(file,&config,&turn);
    turn.continuation=true;turn.tool_call_id="call_1";turn.tool_name="inspect";
    turn.tool_arguments="{}";turn.tool_result="{\"ok\":true}";turn.tool_context="prior tools";
    record(file,&config,&turn);
    strcpy(config.endpoint,"http://fixture/v1/responses");turn.continuation=false;
    record(file,&config,&turn);turn.previous_response_id="resp_prior";record(file,&config,&turn);
    turn.continuation=true;record(file,&config,&turn);
    turn.continuation=false;turn.image_count=0;record(file,&config,&turn);
    int baseline=allocations;
    for(int i=0;i<18;i++) {
        fail_after=i;char *body=NULL;size_t length=0;
        esp_err_t error=agent_openai_build_body(&config,&turn,&body,&length);
        fail_after=-1;assert(error==ESP_OK || error==ESP_ERR_NO_MEM);
        solar_os_memory_free(body);assert(allocations==baseline);
    }
    turn.image_count=2;
    for(int i=0;i<18;i++) {
        fail_after=i;char *body=NULL;size_t length=0;
        esp_err_t error=agent_openai_build_body(&config,&turn,&body,&length);
        fail_after=-1;assert(error==ESP_OK || error==ESP_ERR_NO_MEM);
        solar_os_memory_free(body);assert(allocations==baseline);
    }
    solar_os_media_sample_release(images[1]);solar_os_media_sample_release(images[0]);
    uint8_t *large=malloc(SOLAR_OS_AGENT_IMAGE_BYTES_MAX);assert(large);
    for(size_t i=0;i<SOLAR_OS_AGENT_IMAGE_BYTES_MAX;i++)large[i]=(uint8_t)i;
    memcpy(large,"\xff\xd8\xff",3);
    assert(!solar_os_media_sample_copy_image(large,SOLAR_OS_AGENT_IMAGE_BYTES_MAX,NULL,&images[0]));free(large);
    turn.image_count=1;turn.images=images;turn.previous_response_id=NULL;record(file,&config,&turn);
    images[1]=images[0];assert(solar_os_agent_images_validate(images,2)==ESP_ERR_INVALID_SIZE);
    assert(solar_os_agent_images_validate(images,3)==ESP_ERR_INVALID_ARG);
    char text[64]={0};solar_os_agent_provider_result_t result;
    strcpy(config.endpoint,"http://fixture/v1/chat/completions");
    assert(!solar_os_agent_openai_provider.run_turn(&config,&turn,receive,text,&result));assert(!strcmp(text,"image seen"));
    http_status=400;assert(solar_os_agent_openai_provider.run_turn(&config,&turn,receive,text,&result)!=ESP_OK && result.http_status==400);
    cancelled=true;assert(solar_os_agent_openai_provider.run_turn(&config,&turn,receive,text,&result)!=ESP_OK);cancelled=false;
    assert(requests==3);solar_os_media_sample_release(images[0]);
    assert(!solar_os_media_sample_copy_image("unknown",7,NULL,&images[0]));
    assert(solar_os_agent_images_validate(images,1)==ESP_ERR_NOT_SUPPORTED);solar_os_media_sample_release(images[0]);
    char sources[SOLAR_OS_AGENT_IMAGES_MAX][SOLAR_OS_MEDIA_SAMPLE_SOURCE_MAX];size_t count;
    char options[]="--image '/a b.jpg' --image stream:camera0 Explain  this \"image\"";
    assert(!solar_os_agent_image_options(options,sources,&count) && count==2);
    assert(!strcmp(sources[0],"/a b.jpg") && !strcmp(sources[1],"stream:camera0") && !strcmp(options,"Explain  this \"image\""));
    char chat[]="/image a\\ b.jpg -- --image literal";
    assert(!solar_os_agent_image_options(chat,sources,&count) && count==1 && !strcmp(sources[0],"a b.jpg") && !strcmp(chat,"--image literal"));
    const char *invalid[]={"--image","--image a.jpg","--image 'bad path question","--image a --image b --image c question"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){char p[128];strcpy(p,invalid[i]);assert(solar_os_agent_image_options(p,sources,&count)!=ESP_OK);}
    assert(!fclose(file));assert(!allocations);
    puts("Agent image ownership, protocols, continuations, limits, errors, cancellation and OOM passed");
}
