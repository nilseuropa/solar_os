/* Exercise the production command with controlled clock, transport and input. */
#define SOLAR_OS_SHELL_IO_KIND_PORT 1
struct solar_os_net_session { solar_os_net_session_cancel_fn cancel; void *user; };
static unsigned tx_count, sessions, destroyed, resolves, sends, failed_send, send_delay;
static bool bad_resolve, bad_create, bad_open, port_input;
static uint64_t cancel_at, disconnect_at;
static uint8_t cancel_key;
static struct {uint64_t time;size_t size;uint8_t data[512];} packets[256];
static void send_tests(void);
static const char *run(const char *line);
static int solar_os_shell_io_kind(solar_os_shell_io_t *io) { (void)io;return port_input?1:0; }
static size_t read_cancel(char *out) { if(cancel_at && now_ms>=cancel_at){*out=(char)cancel_key;cancel_at=0;return 1;}return 0; }
static size_t solar_os_input_read_chars(char *out,size_t n) { (void)n;return read_cancel(out); }
static esp_err_t solar_os_port_read(int *port,uint8_t *out,size_t n,uint32_t timeout,size_t *count)
{ (void)port;(void)n;(void)timeout;*count=0;if(disconnect_at && now_ms>=disconnect_at)return ESP_FAIL;*count=read_cancel((char*)out);return ESP_OK; }
void vTaskDelay(TickType_t ticks) { now_ms+=ticks;assert(now_ms<100000); }
esp_err_t solar_os_net_resolve_host(const char *host,char *out,size_t n)
{ assert(*host);resolves++;strlcpy(out,"127.0.0.1",n);return bad_resolve?ESP_ERR_NOT_FOUND:ESP_OK; }
esp_err_t solar_os_net_session_create(const char *owner,solar_os_net_session_cancel_fn cancel,void *user,solar_os_net_session_t **out)
{ assert(!strcmp(owner,"osc-send"));*out=NULL;if(bad_create)return ESP_ERR_NO_MEM;*out=malloc(sizeof(**out));assert(*out);**out=(solar_os_net_session_t){cancel,user};sessions++;return ESP_OK; }
esp_err_t solar_os_net_session_udp_open(solar_os_net_session_t *s,uint16_t port,uint32_t *handle)
{ assert(s && !port);*handle=1;return bad_open?ESP_FAIL:ESP_OK; }
esp_err_t solar_os_net_session_udp_send(solar_os_net_session_t *s,uint32_t handle,const char *host,uint16_t port,const void *data,size_t n,uint32_t timeout)
{
 assert(s && handle==1 && !strcmp(host,"127.0.0.1") && port==9000 && timeout==100 && !s->cancel(s->user));
 if(++sends==failed_send)return ESP_FAIL;
 assert(tx_count<256 && n<=512);packets[tx_count].time=now_ms;packets[tx_count].size=n;memcpy(packets[tx_count++].data,data,n);
 now_ms+=send_delay;return ESP_OK;
}
void solar_os_net_session_destroy(solar_os_net_session_t *s) { if(s){destroyed++;free(s);} }
static void reset_send(void)
{ assert(sessions==destroyed);tx_count=sends=failed_send=send_delay=resolves=0;bad_resolve=bad_create=bad_open=port_input=false;cancel_at=disconnect_at=0;now_ms=100; }
static void send_tests(void)
{
 reset_send();assert(strstr(run("osc send bot:9000 /x 1 -0.5"),"sent 1 message"));
 const uint8_t expected[]={'/','x',0,0,',','f','f',0,0x3f,0x80,0,0,0xbf,0,0,0};
 assert(tx_count==1 && packets[0].size==sizeof(expected) && !memcmp(packets[0].data,expected,sizeof(expected)));
 const char *bad[]={
  "osc send bot /x 0", "osc send bot:0 /x 0", "osc send :9000 /x 0", "osc send bot:65536 /x 0",
  "osc send bot:9000 x 0", "osc send bot:9000 /x nan", "osc send bot:9000 /x inf",
  "osc send bot:9000 /x 0 --duration 1", "osc send bot:9000 /x 0 --rate 0", "osc send bot:9000 /x 0 --rate 101",
  "osc send bot:9000 /x 0 --rate 1.5", "osc send bot:9000 /x 0 --rate 10 --rate 20",
  "osc send bot:9000 /x 0 --duration 1 --duration 2 --rate 20", "osc send bot:9000 /x 0 --duration 0 --rate 20",
  "osc send bot:9000 /x 0 --duration 86401 --rate 20", "osc send bot:9000 /x 0 --duration nan --rate 20",
  "osc send bot:9000 /x 0 --unknown", "osc send bot:9000 /x 1 2 3 4 5 6 7 8 9", "osc send bot:9000 /x --rate 20",
 };
 for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++){reset_send();assert(strstr(run(bad[i]),"osc send:"));assert(!tx_count && !resolves);}
 reset_send();run("osc send bot:9000 /x 1 2 3 4 5 6 7 8");assert(tx_count==1 && packets[0].size==48);
 reset_send();assert(strstr(run("osc send bot:9000 /x 0 0 --rate 20 --duration 1"),"sent 20 messages"));
 assert(tx_count==20 && packets[0].time==100 && packets[19].time==1050 && now_ms==1100);
 for(unsigned i=1;i<tx_count;i++)assert(packets[i].size==packets[0].size && !memcmp(packets[i].data,packets[0].data,packets[0].size));
 reset_send();send_delay=180;run("osc send bot:9000 /x --duration 1 0 --rate 20");
 assert(tx_count==5);for(unsigned i=1;i<tx_count;i++)assert(packets[i].time-packets[i-1].time==200);
 for(unsigned k=0;k<3;k++){
  reset_send();port_input=k==1;cancel_key=k==0?3:k==1?27:SOLAR_OS_KEY_APP_EXIT;cancel_at=225;
  assert(strstr(run("osc send bot:9000 /x 0 --rate 20"),"cancelled"));assert(tx_count==3 && now_ms<250);
 }
 reset_send();port_input=true;disconnect_at=225;run("osc send bot:9000 /x 0 --rate 20");assert(tx_count==3 && now_ms<250);
 reset_send();bad_resolve=true;assert(strstr(run("osc send missing:9000 /x 0"),"resolve host"));assert(!tx_count && sessions==destroyed);
 reset_send();bad_create=true;assert(strstr(run("osc send bot:9000 /x 0"),"create UDP session"));
 reset_send();bad_open=true;assert(strstr(run("osc send bot:9000 /x 0"),"open UDP socket"));assert(sessions==destroyed);
 reset_send();failed_send=3;assert(strstr(run("osc send bot:9000 /x 0 --rate 20 --duration 1"),"send UDP message"));assert(tx_count==2 && sessions==destroyed);
 puts("OSC send: multi-value bytes, validation, cadence, skipped periods, cancellation, disconnect and cleanup passed");
}
