deps_config := \
	/home/xzwang/Work/ESP32/esp-idf/components/app_trace/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/aws_iot/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/bt/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/esp32/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/ethernet/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/fatfs/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/freertos/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/heap/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/libsodium/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/log/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/lwip/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/mbedtls/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/openssl/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/pthread/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/spi_flash/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/spiffs/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/tcpip_adapter/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/wear_levelling/Kconfig \
	/home/xzwang/Work/ESP32/esp-idf/components/bootloader/Kconfig.projbuild \
	/home/xzwang/Work/ESP32/esp-idf/components/esptool_py/Kconfig.projbuild \
	/home/xzwang/Work/ESP32/esp-idf/components/partition_table/Kconfig.projbuild \
	/home/xzwang/Work/ESP32/esp-idf/Kconfig

include/config/auto.conf: \
	$(deps_config)


$(deps_config): ;
