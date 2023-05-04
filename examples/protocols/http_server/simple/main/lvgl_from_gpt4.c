#include "lvgl/lvgl.h"
#include "lvgl_esp32_drivers/lvgl_tft/esp32_spi_tft.h"
#include "lvgl_esp32_drivers/lvgl_touch/esp32_spi_touch.h"

#define LVGL_TICK_PERIOD_MS 1

void app_main(void)
{
    esp_err_t ret;

    lv_init();

    /*Initialize SPI or I2C bus used by the drivers*/
    esp_err_t disp_spi_init_ret = lvgl_driver_init();

    if(disp_spi_init_ret != ESP_OK) {
        ESP_LOGE(TAG, "Display driver init fail\n");
        return;
    }

    /*Initialize LVGL*/
    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.disp_flush = lvgl_driver_flush;
    disp_drv.disp_fill = lvgl_driver_fill;
    disp_drv.disp_map = lvgl_driver_map;
    lv_disp_drv_register(&disp_drv);

    /*Create a Label object*/
    lv_obj_t * label = lv_label_create(lv_scr_act(), NULL);
    lv_label_set_text(label, "Hello World!");

    while(1) {
        /* Periodically call the lv_task handler.
         * It could be done in a timer interrupt or an OS task too.*/
        lv_task_handler();
        vTaskDelay(LVGL_TICK_PERIOD_MS / portTICK_RATE_MS);
    }
}
```

I hope this helps! Let me know if you have any other questions.

Source: Conversation with Bing, 4/4/2023(1) TTGO-T-Display esp-idf example code - GitHub. https://github.com/suapapa/esp32_ttgo-t-dispaly Accessed 4/4/2023.
(2) GitHub - lvgl/lv_port_esp32: LVGL ported to ESP32 including various .... https://github.com/lvgl/lv_port_esp32 Accessed 4/4/2023.
(3) Espressif (ESP32) — LVGL documentation. https://docs.lvgl.io/latest/en/html/get-started/espressif.html Accessed 4/4/2023.