set(LVGL_DEMOS_DIR ../managed_components/lvgl__lvgl/demos)

file(GLOB_RECURSE SOURCES rgb_lcd_example_main.c ${LVGL_DEMOS_DIR}/*.c)

idf_component_register(
    SRCS
        ${SOURCES}
    INCLUDE_DIRS
        "."
        ${LVGL_DEMOS_DIR}
)
