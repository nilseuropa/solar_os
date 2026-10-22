#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_can.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static bool number(const char *text,uint32_t max,uint32_t *value)
{
    if(!text || !text[0] || text[0]=='-') return false;
    char *end; errno=0;
    unsigned long parsed=strtoul(text,&end,0);
    if(errno || *end || parsed>max) return false;
    *value=parsed; return true;
}
static int hex(char c)
{
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
void solar_os_shell_cmd_can(solar_os_context_t *ctx,int argc,char **argv)
{
    solar_os_shell_io_t *io=solar_os_shell_command_io(ctx);
    if(argc==1 || (argc==2 && !strcmp(argv[1],"list"))) {
        solar_os_can_info_t info;
        if(solar_os_can_get_info(&info)) solar_os_shell_io_printf(io,"%s  %u kbit/s  %s\n",
            info.name,info.bitrate_kbit,info.listen_only ? "listen-only" : "normal");
        else solar_os_shell_io_writeln(io,"no CAN controller attached");
        return;
    }
    esp_err_t err=ESP_ERR_INVALID_ARG;
    if(argc==3 && !strcmp(argv[1],"status")) {
        solar_os_can_status_t s;
        err=solar_os_can_status(argv[2],&s);
        if(err==ESP_OK) {
            static const char *states[]={"stopped","running","bus-off","recovering"};
            solar_os_shell_io_printf(io,"%s state=%s queued-tx=%"PRIu32" queued-rx=%"PRIu32
                " tx-errors=%"PRIu32" rx-errors=%"PRIu32" failed-tx=%"PRIu32
                " missed-rx=%"PRIu32" bus-errors=%"PRIu32"\n",argv[2],
                s.state<=SOLAR_OS_CAN_RECOVERING ? states[s.state] : "unknown",
                s.pending_tx,s.pending_rx,s.tx_errors,s.rx_errors,s.failed_tx,s.missed_rx,s.bus_errors);
        }
    } else if(argc==3 && !strcmp(argv[1],"recover")) err=solar_os_can_recover(argv[2]);
    else if(argc==3 && !strcmp(argv[1],"start")) err=solar_os_can_start(argv[2]);
    else if((argc==3 || argc==4) && !strcmp(argv[1],"recv")) {
        uint32_t ms=0;
        if(argc==4 && !number(argv[3],1000,&ms)) goto usage;
        solar_os_can_frame_t f;
        err=solar_os_can_receive(argv[2],&f,ms);
        if(err==ESP_OK) {
            solar_os_shell_io_printf(io,"%s id=0x%"PRIx32" %s%s dlc=%u data=",argv[2],f.id,
                f.extended ? "extended" : "standard",f.remote ? " remote" : "",f.length);
            if(!f.remote) for(unsigned i=0;i<f.length;i++) solar_os_shell_io_printf(io,"%02x",f.data[i]);
            solar_os_shell_io_write(io,"\n");
        }
    } else if(argc>=5 && argc<=7 && !strcmp(argv[1],"send")) {
        solar_os_can_frame_t f={0};
        if(!number(argv[3],0x1fffffffU,&f.id)) goto usage;
        const char *bytes=argv[4];
        const size_t length=!strcmp(bytes,"-") ? 0 : strlen(bytes);
        if(length>16 || length%2) goto usage;
        f.length=length/2;
        for(size_t i=0;i<length;i+=2) {
            const int a=hex(bytes[i]),b=hex(bytes[i+1]);
            if(a<0 || b<0) goto usage;
            f.data[i/2]=(a<<4)|b;
        }
        for(int i=5;i<argc;i++) {
            if(!strcmp(argv[i],"ext") && !f.extended) f.extended=true;
            else if(!strcmp(argv[i],"rtr") && !f.remote) f.remote=true;
            else goto usage;
        }
        err=solar_os_can_send(argv[2],&f,100);
        if(err==ESP_OK) solar_os_shell_io_writeln(io,"CAN frame queued");
    } else goto usage;
    if(err!=ESP_OK) solar_os_shell_io_printf(io,"can: %s\n",esp_err_to_name(err));
    return;
usage:
    solar_os_shell_io_writeln(io,"usage: can [list] | status|recover|start <name> | recv <name> [timeout-ms] | send <name> <id> <hex-or-> [ext] [rtr]");
}
