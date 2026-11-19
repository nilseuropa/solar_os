from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Spd2010TouchTest(unittest.TestCase):
    def test_boot_reports_releases_errors_and_bounded_draining(self):
        source = r'''
#include <assert.h>
#include <string.h>
#include "spd2010_touch.h"
static struct { uint16_t reg; size_t len; uint8_t data[64]; } replies[32];
static unsigned queued, consumed, command_count, reads, fail_read;
static uint16_t selected;
static uint8_t commands[32][4];
static bool fail_write;
static void reset(void) {
    queued=consumed=command_count=reads=fail_read=0; fail_write=false;
    memset(replies,0,sizeof(replies)); memset(commands,0,sizeof(commands));
}
static uint8_t *queue(uint16_t reg, size_t len) {
    assert(queued<32 && len<=64); replies[queued].reg=reg; replies[queued].len=len;
    return replies[queued++].data;
}
static void status(uint8_t low,uint8_t high,uint16_t len) {
    uint8_t *p=queue(0x20,4); p[0]=low;p[1]=high;p[2]=len;p[3]=len>>8;
}
static void done(void) { uint8_t *p=queue(0x2fc,8); p[5]=0x82; }
static esp_err_t write_io(void *ctx,const uint8_t *data,size_t len) {
    (void)ctx; if(fail_write) return ESP_FAIL;
    if(len==2) selected=(uint16_t)data[0]|((uint16_t)data[1]<<8);
    else { assert(len==4 && command_count<32);memcpy(commands[command_count++],data,4); }
    return ESP_OK;
}
static esp_err_t read_io(void *ctx,uint8_t *data,size_t len) {
    (void)ctx; reads++; if(reads==fail_read) return ESP_FAIL;
    assert(consumed<queued && replies[consumed].reg==selected && replies[consumed].len==len);
    memcpy(data,replies[consumed++].data,len); return ESP_OK;
}
static void delay(void *ctx,uint32_t us) { (void)ctx;assert(us==200); }
int main(void) {
    solar_os_spd2010_touch_io_t io={.write=write_io,.read=read_io,.delay_us=delay};
    solar_os_spd2010_touch_sample_t sample;
    assert(solar_os_spd2010_touch_read(NULL,&sample)==ESP_ERR_INVALID_ARG);
    queue(0x26,18);
    assert(solar_os_spd2010_touch_probe(&io)==ESP_OK && consumed==queued);
    reset();status(0,0x40,0);
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_OK && !sample.valid);
    assert(command_count==2 && commands[0][0]==2 && commands[1][0]==4 && commands[1][2]==1);
    reset();status(0,0x20,0);
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_OK && !sample.valid);
    assert(command_count==3 && commands[0][0]==0x50 && commands[1][0]==0x46 && commands[2][0]==2);
    reset();status(0,0x80,0);
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_OK && !sample.valid && command_count==0);
    reset();status(0,8,0);
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_OK && sample.valid && !sample.touched);
    assert(command_count==1 && commands[0][0]==2);
    reset();status(1,8,10);
    uint8_t *p=queue(0x300,10);p[4]=3;p[5]=0x2c;p[6]=0x90;p[7]=0x11;p[8]=100;done();
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_OK && sample.valid && sample.touched);
    assert(sample.id==3 && sample.x==300 && sample.y==400 && consumed==queued);
    reset();status(1,8,10);queue(0x300,10);done();
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_OK && sample.valid && !sample.touched);
    reset();status(1,8,4);queue(0x300,4);done();
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_OK && sample.valid && !sample.touched);
    reset();status(1,8,65);
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_ERR_INVALID_RESPONSE && !sample.valid && reads==1);
    reset();status(1,8,9);queue(0x300,9);
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_ERR_INVALID_RESPONSE && !sample.valid);
    reset();status(1,8,10);queue(0x300,10)[4]=11;
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_ERR_INVALID_RESPONSE && !sample.valid);
    reset();status(1,8,10);queue(0x300,10);queue(0x2fc,8)[5]=0x89;
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_ERR_INVALID_RESPONSE && !sample.valid);
    reset();status(1,8,10);queue(0x300,10);queue(0x2fc,8)[2]=65;
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_ERR_INVALID_RESPONSE && !sample.valid);
    reset();status(1,8,10);queue(0x300,10);
    for(unsigned i=0;i<8;i++) { queue(0x2fc,8)[2]=4;queue(0x300,4); }
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_ERR_TIMEOUT && !sample.valid && reads==18);
    reset();status(1,8,10);queue(0x300,10);done();fail_read=3;
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_FAIL && !sample.valid);
    reset();fail_write=true;
    assert(solar_os_spd2010_touch_read(&io,&sample)==ESP_FAIL && !sample.valid && !reads);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(source)
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/drivers"),
                str(path / "test.c"), str(ROOT / "src/drivers/spd2010_touch.c"),
                "-o", str(path / "test"),
            ], check=True)
            subprocess.run([str(path / "test")], check=True)
