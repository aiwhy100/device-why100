# 预置提示语（voice/out/*.wav，16kHz 单声道）直接打包进固件
set(VOICE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../voice/out)
set(VOICES boot got found not_heard rejected busy no_credit bad_key net_error no_wifi ready failed setup saved connecting wifi_bad phone_in)
set(VOICE_FILES)
foreach(v ${VOICES})
  list(APPEND VOICE_FILES ${VOICE_DIR}/${v}.wav)
endforeach()

idf_component_register(
  SRCS "main.c" "settings.c" "portal.c" "api.c"
  INCLUDE_DIRS "."
  REQUIRES esp_timer esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_http_server mbedtls json
           esp_driver_i2s esp_driver_i2c esp_driver_gpio lwip
  EMBED_FILES ${VOICE_FILES})
