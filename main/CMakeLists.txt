idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_ebook.c"
         "ebook_reader.c"
         "boot_animation.c"
         "wifi_sta.c"
         "ble_prov.c"
         "ai_chat.c"
         "demo_ai.c"
         "demo_wifi.c"
         "font_cn_16.c"
         "demo_about.c"
         "bg_img.c"
    INCLUDE_DIRS "."
    REQUIRES bsp spiffs nvs_flash esp_wifi esp_netif esp_event bt json mbedtls esp_http_client
)
