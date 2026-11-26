#include "display_sync.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include <limits.h>

/* LCD_TE on this board; input only. The BSP enables TE with command 0x35. */
#define TE_GPIO GPIO_NUM_13
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static display_sync_stats_t s_stats;
static TaskHandle_t s_waiter;
static bool s_ready;
static void IRAM_ATTR te_edge(void *argument)
{
    (void)argument;
    int64_t now=esp_timer_get_time();
    bool high=gpio_ll_get_level(&GPIO,TE_GPIO)!=0;
    BaseType_t wake=pdFALSE;
    portENTER_CRITICAL_ISR(&s_lock);
    if(high) {
        if(s_stats.last_rise_us) {
            uint32_t period=(uint32_t)(now-s_stats.last_rise_us);
            s_stats.period_us=period;
            if(period<s_stats.min_us) s_stats.min_us=period;
            if(period>s_stats.max_us) s_stats.max_us=period;
        }
        s_stats.last_rise_us=now;++s_stats.rises;
        if(s_waiter) vTaskNotifyGiveFromISR(s_waiter,&wake);
    } else if(s_stats.last_rise_us) s_stats.high_us=(uint32_t)(now-s_stats.last_rise_us);
    portEXIT_CRITICAL_ISR(&s_lock);
    if(wake) portYIELD_FROM_ISR();
}
esp_err_t display_sync_init(void)
{
    gpio_config_t config={.pin_bit_mask=1ULL<<TE_GPIO,.mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_DISABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_ANYEDGE};
    esp_err_t result=gpio_config(&config);
    if(result!=ESP_OK) return result;
    result=gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if(result!=ESP_OK && result!=ESP_ERR_INVALID_STATE) return result;
    s_stats.min_us=UINT32_MAX;
    result=gpio_isr_handler_add(TE_GPIO,te_edge,NULL);
    s_ready=result==ESP_OK;
    return result;
}
void display_sync_stats(display_sync_stats_t *stats)
{
    portENTER_CRITICAL(&s_lock);*stats=s_stats;portEXIT_CRITICAL(&s_lock);
}
bool display_sync_available(void)
{
    display_sync_stats_t stats;display_sync_stats(&stats);
    return s_ready&&stats.rises>=3&&stats.min_us>=10000&&stats.min_us<=25000&&
        esp_timer_get_time()-stats.last_rise_us<100000;
}
void display_sync_forget_task(TaskHandle_t task)
{
    portENTER_CRITICAL(&s_lock);
    if(s_waiter==task) s_waiter=NULL;
    portEXIT_CRITICAL(&s_lock);
}
bool display_sync_wait(uint32_t timeout_ms)
{
    TaskHandle_t task=xTaskGetCurrentTaskHandle();
    ulTaskNotifyTake(pdTRUE,0);
    portENTER_CRITICAL(&s_lock);s_waiter=task;portEXIT_CRITICAL(&s_lock);
    bool result=ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(timeout_ms))!=0;
    display_sync_forget_task(task);
    return result;
}
