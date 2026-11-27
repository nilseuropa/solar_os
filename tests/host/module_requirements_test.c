#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "solar_os_module_packages.h"
#include "solar_os_json.h"

size_t strlcpy(char *out, const char *source, size_t capacity)
{ size_t size = strlen(source); if (capacity) snprintf(out, capacity, "%s", source); return size; }
const solar_os_component_t *solar_os_component_find(solar_os_module_type_t type, const char *id)
{ (void)type; (void)id; return NULL; }

static bool module_name_valid(const char *s)
{ return s && *s && strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == strlen(s); }
const solar_os_json_value_t *solar_os_json_path_get(const solar_os_json_value_t *v,const char *s)
{ return cJSON_GetObjectItemCaseSensitive(v,s); }
bool solar_os_json_is_array(const solar_os_json_value_t *v) { return cJSON_IsArray(v); }
size_t solar_os_json_array_size(const solar_os_json_value_t *v) { return cJSON_GetArraySize(v); }
const solar_os_json_value_t *solar_os_json_array_get(const solar_os_json_value_t *v,size_t n) { return cJSON_GetArrayItem(v,n); }
esp_err_t solar_os_json_get_path_string(const solar_os_json_value_t *v,const char *key,char *out,size_t n)
{ const cJSON *s=cJSON_GetObjectItemCaseSensitive(v,key);if(!cJSON_IsString(s)||strlen(s->valuestring)>=n)return ESP_FAIL;strcpy(out,s->valuestring);return ESP_OK; }
esp_err_t solar_os_json_get_path_uint32(const solar_os_json_value_t *v,const char *key,uint32_t *out)
{ const cJSON *s=cJSON_GetObjectItemCaseSensitive(v,key);if(!cJSON_IsNumber(s)||s->valuedouble<0||s->valuedouble>UINT32_MAX||s->valuedouble!=(uint32_t)s->valuedouble)return ESP_FAIL;*out=s->valuedouble;return ESP_OK; }
static bool solar_os_native_service_available(const char *name,uint32_t abi,uint32_t size)
{ return abi==1 && ((!strcmp(name,"libc") && size<=52) || (!strcmp(name,"storage") && size<=32)); }
#include "solar_os_module_requirements.inc"

static solar_os_module_package_t entries[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX];
static solar_os_module_catalog_t catalog;
static void reset_catalog(void)
{ memset(&catalog, 0, sizeof(catalog)); memset(entries, 0, sizeof(entries)); catalog.packages = entries; }
static esp_err_t parse(const char *text,uint32_t schema)
{
    cJSON *json=cJSON_Parse(text);assert(json);
    reset_catalog();
    catalog.packages[0].type=SOLAR_OS_MODULE_TYPE_APP;
    catalog.packages[0].lifecycle_abi=1;
    catalog.packages[0].compatible=true;
    esp_err_t code=module_parse_requirements(json,schema,&catalog.packages[0]);
    cJSON_Delete(json);return code;
}
int main(void)
{
    const solar_os_component_t engine = {.type=SOLAR_OS_MODULE_TYPE_LIBRARY, .id="engine", .version="1.0"};
    const solar_os_component_dependency_t missing[] = {{"missing", "1.0"}};
    const solar_os_component_t client = {.type=SOLAR_OS_MODULE_TYPE_APP, .id="client", .version="1.0",
        .dependency_count=1, .dependencies=missing};
    solar_os_module_install_result_t result;
    module_component_result(&engine, &result);
    assert(result.compiled && !strcmp(result.id, "engine") && !strcmp(result.version, "1.0"));
    assert(module_component_self_contained(&engine, 0) && !module_component_self_contained(&client, 0));
    assert(parse("{}",2)==ESP_OK);
    assert(parse("{\"dependencies\":[]}",2)==ESP_ERR_INVALID_RESPONSE);
    assert(parse("{\"dependencies\":{}}",3)==ESP_ERR_INVALID_RESPONSE);
    assert(parse("{\"dependencies\":[{\"id\":\"tinycc\",\"version\":\"0.5.0\"}]}",3)==ESP_OK);
    assert(parse("{\"required_services\":[{\"name\":\"libc\",\"abi\":1,\"minimum_struct_size\":52}]}",3)==ESP_OK);
    assert(catalog.packages[0].compatible);
    assert(parse("{\"required_services\":[{\"name\":\"libc\",\"abi\":1,\"minimum_struct_size\":60}]}",3)==ESP_OK);
    assert(!catalog.packages[0].compatible && strstr(catalog.packages[0].incompatibility,"libc"));
    assert(parse("{\"required_services\":[{\"name\":\"libc\",\"abi\":0,\"minimum_struct_size\":56}]}",3)!=ESP_OK);
    assert(parse("{\"dependencies\":[{\"id\":\"tinycc\",\"version\":\"0.5.0\"},{\"id\":\"tinycc\",\"version\":\"0.4.0\"}]}",3)!=ESP_OK);
    reset_catalog();catalog.count=3;
    for(unsigned i=0;i<3;i++) {
        solar_os_module_package_t *p=&catalog.packages[i];
        snprintf(p->id,sizeof(p->id),"p%u",i);strcpy(p->version,"1.0");
        p->type=SOLAR_OS_MODULE_TYPE_LIBRARY;p->compatible=true;
        if(i<2) {p->dependency_count=1;snprintf(p->dependencies[0].id,32,"p%u",i+1);strcpy(p->dependencies[0].version,"1.0");}
    }
    assert(module_catalog_requirements(&catalog)==ESP_OK);
    uint8_t state[16]={0},order[16];size_t count=0;
    assert(module_plan_visit(&catalog,0,state,order,&count)==ESP_OK);
    assert(count==3 && order[0]==2 && order[1]==1 && order[2]==0);
    catalog.packages[2].compatible=false;strcpy(catalog.packages[2].incompatibility,"needs service libc");
    assert(module_catalog_requirements(&catalog)==ESP_OK);
    assert(!catalog.packages[0].compatible && strstr(catalog.packages[0].incompatibility,"dependency"));
    strcpy(catalog.packages[1].dependencies[0].version,"2.0");
    assert(module_catalog_requirements(&catalog)==ESP_ERR_INVALID_RESPONSE);
    strcpy(catalog.packages[1].dependencies[0].version,"1.0");
    catalog.packages[2].dependency_count=1;
    strcpy(catalog.packages[2].dependencies[0].id,"p0");strcpy(catalog.packages[2].dependencies[0].version,"1.0");
    assert(module_catalog_requirements(&catalog)==ESP_ERR_INVALID_RESPONSE);
    catalog.packages[2].dependency_count=0;catalog.count=2;
    assert(module_catalog_requirements(&catalog)==ESP_ERR_INVALID_RESPONSE);
    puts("Signed requirements: bounds, service gating, exact versions, ordering and cycles passed");
}
