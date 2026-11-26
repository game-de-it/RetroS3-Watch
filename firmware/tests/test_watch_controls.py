"""Exercise combo consumption, repeat timing, limits and deferred persistence."""
import pathlib, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class ControlsTest(unittest.TestCase):
    def test_control_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            d = pathlib.Path(directory)
            (d/'nvs.h').write_text('''#include <stdint.h>
typedef int nvs_handle_t; typedef int esp_err_t;
#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
int nvs_open(const char*,int,nvs_handle_t*); int nvs_get_u16(int,const char*,uint16_t*);
int nvs_set_u16(int,const char*,uint16_t); int nvs_commit(int); void nvs_close(int);
const char *esp_err_to_name(int);
''')
            (d/'esp_log.h').write_text('#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n')
            (d/'esp_timer.h').write_text('#include <stdint.h>\nint64_t esp_timer_get_time(void);\n')
            (d/'test.c').write_text('''#include <assert.h>
#include <stdio.h>
#include "watch_controls.h"
#include "nvs.h"
static uint32_t now; static uint16_t saved; static int commits;
int64_t esp_timer_get_time(void) { return (int64_t)now*1000; }
int nvs_open(const char*n,int m,int*h) {(void)n;(void)m;*h=1;return 0;}
int nvs_get_u16(int h,const char*k,uint16_t*v) {(void)h;(void)k; if(!saved)return 1; *v=saved;return 0;}
int nvs_set_u16(int h,const char*k,uint16_t v) {(void)h;(void)k;saved=v;return 0;}
int nvs_commit(int h){(void)h;++commits;return 0;} void nvs_close(int h){(void)h;}
const char *esp_err_to_name(int x){(void)x;return "test";}
static void tick(uint32_t t){now=t;watch_controls_tick(t);}
int main(void){
 watch_controls_init(); assert(watch_controls_volume()==100 && watch_controls_brightness()==70);
 assert(watch_controls_filter(1|16)==17);
 assert(watch_controls_filter(256|2|16)==16); tick(100);
 assert(watch_controls_volume()==95);tick(549);assert(watch_controls_volume()==95);
 tick(550);assert(watch_controls_volume()==90);tick(700);assert(watch_controls_volume()==85);
 assert(watch_controls_filter(2)==0);tick(725); /* release modifier first */
 assert(watch_controls_filter(0)==0);assert(watch_controls_filter(2)==2);
 watch_controls_filter(256|4);tick(800);assert(watch_controls_brightness()==60);
 watch_controls_filter(256|8);tick(825);assert(watch_controls_brightness()==70);
 watch_controls_filter(256|3|12);tick(850);assert(watch_controls_volume()==85 && watch_controls_brightness()==70);
 watch_controls_adjust(1,-500);assert(watch_controls_volume()==0);
 watch_controls_adjust(2,-500);assert(watch_controls_brightness()==10);
 watch_controls_adjust(1,500);assert(watch_controls_volume()==100);
 watch_controls_adjust(2,500);assert(watch_controls_brightness()==100);
 tick(2000);assert(commits==0); /* modifier still held */
 watch_controls_filter(0);tick(2025);assert(commits==1);
 watch_controls_adjust(1,-5);watch_controls_adjust(2,-10);tick(2500);assert(commits==1);
 tick(3025);assert(commits==2);watch_controls_init();assert(watch_controls_volume()==95 && watch_controls_brightness()==90);
 int value;assert(watch_controls_notice(3025,&value)==2 && value==90);
 assert(watch_controls_notice(3525,&value)==0);
 puts("combo consumption/repeat/limits/persistence passed");
}
''')
            exe=d/'test'
            subprocess.run(['cc','-std=c11','-I'+str(d),'-I'+str(ROOT/'main'),str(ROOT/'main/watch_controls.c'),str(d/'test.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
if __name__=='__main__': unittest.main()
