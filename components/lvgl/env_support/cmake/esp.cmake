include("${CMAKE_CURRENT_LIST_DIR}/version.cmake")

file(GLOB_RECURSE SOURCES ${LVGL_ROOT_DIR}/src/*.c ${LVGL_ROOT_DIR}/src/*.cpp)

idf_build_get_property(LV_MICROPYTHON LV_MICROPYTHON)
idf_build_get_property(target IDF_TARGET)

if(LV_MICROPYTHON)
  idf_component_register(
    SRCS
    ${SOURCES}
    INCLUDE_DIRS
    ${LVGL_ROOT_DIR}/include
    REQUIRES
    main)
else()
  # This project vendors the LVGL core only. Upstream examples and demos are
  # intentionally omitted to keep the repository small.

  set(IDF_COMPONENTS esp_timer log)

  if(${target} STREQUAL "esp32p4" OR ${target} STREQUAL "esp32s31")
    list(APPEND IDF_COMPONENTS esp_driver_ppa esp_mm)
  endif()

  if(CONFIG_LV_USE_FS_FATFS)
    list(APPEND IDF_COMPONENTS fatfs)
  endif()

  idf_component_register(SRCS ${SOURCES}
      INCLUDE_DIRS ${LVGL_ROOT_DIR} ${LVGL_ROOT_DIR}/src ${LVGL_ROOT_DIR}/../
      PRIV_REQUIRES ${IDF_COMPONENTS})
endif()

target_compile_definitions(${COMPONENT_LIB} PUBLIC "-DLV_CONF_INCLUDE_SIMPLE")

if(CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM)
  target_compile_definitions(${COMPONENT_LIB}
                             PUBLIC "-DLV_ATTRIBUTE_FAST_MEM=IRAM_ATTR")
endif()

if(CONFIG_FREERTOS_SMP)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${IDF_PATH}/components/freertos/FreeRTOS-Kernel-SMP/include/freertos/")
else()
    target_include_directories(${COMPONENT_LIB} PRIVATE "${IDF_PATH}/components/freertos/FreeRTOS-Kernel/include/freertos/")
endif()
