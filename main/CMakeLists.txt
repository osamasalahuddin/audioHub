idf_component_register(SRCS "softap_example_main.c"
                            "bt_main.c"
                            "bt_app_av.c"
                            "bt_app_core.c"
                            "udp_audio.c"
                            "tcp_audio.c"
                            "main.c"
                    PRIV_REQUIRES esp_wifi esp_driver_i2s bt nvs_flash esp_ringbuf esp_driver_dac
                    INCLUDE_DIRS ".")
