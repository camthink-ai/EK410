idf_component_register(
    SRCS "main.c" "config.c" "wifi.c" "mqtt.c" "http.c" "comm.c" "epd709.c" "app_epd.c"
         "canvas.c" "qrcodegen.c" "battery.c" "ota.c" "sleep.c" "led.c" "button.c"
         "aw9523b.c" "board_ioext.c" "splash.c" "font_hz16.c" "img_decode.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi nvs_flash esp_event esp_netif esp_http_server esp_http_client
             mqtt json esp_timer esp_adc esp_https_ota app_update mbedtls
)
