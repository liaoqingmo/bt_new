deps_config := \
	/home/liaoqingmo/esp32/esp-idf/components/app_trace/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/aws_iot/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/bt/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/driver/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/esp32/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/esp_adc_cal/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/esp_http_client/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/ethernet/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/fatfs/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/freertos/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/heap/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/libsodium/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/log/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/lwip/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/mbedtls/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/openssl/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/pthread/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/spi_flash/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/spiffs/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/tcpip_adapter/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/vfs/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/wear_levelling/Kconfig \
	/home/liaoqingmo/esp32/esp-idf/components/bootloader/Kconfig.projbuild \
	/home/liaoqingmo/esp32/esp-idf/components/esptool_py/Kconfig.projbuild \
	/home/liaoqingmo/esp32/esp-idf/components/partition_table/Kconfig.projbuild \
	/home/liaoqingmo/esp32/esp-idf/Kconfig

include/config/auto.conf: \
	$(deps_config)


$(deps_config): ;
