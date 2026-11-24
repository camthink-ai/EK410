set(srcs
    "ct_prov.c" "ct_prov_frag.c" "ct_prov_ble.c" "last_frame.c" "evtlog.c" "main.c" "config.c"
    "wifi.c" "mqtt.c" "http.c" "comm.c" "json_guard.c"
    "epd709.c" "app_epd.c" "canvas.c" "qrcodegen.c" "battery.c" "ota.c" "sleep.c"
    "led.c" "button.c"
    "aw9523b.c" "board_ioext.c" "splash.c" "font_hz16.c" "img_decode.c"
)
set(reqs
    driver esp_wifi nvs_flash esp_event esp_netif esp_http_server esp_http_client
    mqtt json esp_timer esp_adc esp_https_ota app_update mbedtls
    bt esp_coex
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES ${reqs}
)
