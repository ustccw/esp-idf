LCD
===

Introduction
------------

ESP chips can generate various kinds of timings that needed by common LCDs on the market, like SPI LCD, I80 LCD (a.k.a Intel 8080 parallel LCD), RGB/SRGB LCD, I2C LCD, etc. The ``esp_lcd`` component is officially to support those LCDs with a group of universal APIs across chips.

Functional Overview
-------------------

In ``esp_lcd``, an LCD panel is represented by :cpp:type:`esp_lcd_panel_handle_t`, which plays the role of an **abstract frame buffer**, regardless of the frame memory is allocated inside ESP chip or in external LCD controller. Based on the location of the frame buffer, the LCD panel allocation functions are mainly grouped into the following categories:

-  ``RGB LCD panel`` - is simply based on a group of specific synchronous signals indicating where to start and stop a frame.
-  ``Controller based LCD panel`` involves multiple steps to get a panel handle, like bus allocation, IO device registration and controller driver install.

After we get the LCD handle, the remaining LCD operations are the same for different LCD interfaces and vendors.

.. only:: SOC_LCD_RGB_SUPPORTED

    RGB Interfaced LCD
    ------------------

    RGB LCD panel is allocated in one step: :cpp:func:`esp_lcd_new_rgb_panel`, with various configurations specified by :cpp:type:`esp_lcd_rgb_panel_config_t`.

    - :cpp:member:`esp_lcd_rgb_panel_config_t::clk_src` selects the clock source for the RGB LCD controller. The available clock sources are listed in :cpp:type:`lcd_clock_source_t`.
    - :cpp:member:`esp_lcd_rgb_panel_config_t::data_width` set number of data lines used by the RGB interface. Currently, the supported value can be 8 or 16.
    - :cpp:member:`esp_lcd_rgb_panel_config_t::bits_per_pixel` set the number of bits per pixel. This is different from :cpp:member:`esp_lcd_rgb_panel_config_t::data_width`. By default, if you set this field to 0, the driver will automatically adjust the bpp to the :cpp:member:`esp_lcd_rgb_panel_config_t::data_width`. But in some cases, these two value must be different. For example, a Serial RGB interface LCD only needs `8` data lines, but the color width can reach to `RGB888`, i.e. the :cpp:member:`esp_lcd_rgb_panel_config_t::bits_per_pixel` should be set to `24`.
    - :cpp:member:`esp_lcd_rgb_panel_config_t::hsync_gpio_num`, :cpp:member:`esp_lcd_rgb_panel_config_t::vsync_gpio_num`, :cpp:member:`esp_lcd_rgb_panel_config_t::de_gpio_num`, :cpp:member:`esp_lcd_rgb_panel_config_t::pclk_gpio_num`, :cpp:member:`esp_lcd_rgb_panel_config_t::disp_gpio_num` and :cpp:member:`esp_lcd_rgb_panel_config_t::data_gpio_nums` are the GPIO pins used by the RGB LCD controller. If some of them are not used, please set it to `-1`.
    - :cpp:member:`esp_lcd_rgb_panel_config_t::sram_trans_align` and :cpp:member:`esp_lcd_rgb_panel_config_t::psram_trans_align` set the alignment of the allocated frame buffer. Internally, the DMA transfer ability will adjust against these alignment values. The alignment value must be a power of 2.
    - :cpp:member:`esp_lcd_rgb_panel_config_t::bounce_buffer_size_px` set the size of bounce buffer. This is only necessary for a so-called "bounce buffer" mode. Please refer to :ref:`_bounce_buffer_with_single_psram_frame_buffer` for more information.
    - :cpp:member:`esp_lcd_rgb_panel_config_t::timings` sets the LCD panel specific timing parameters. All required parameters are listed in the :cpp:type:`esp_lcd_rgb_timing_t`, including the LCD resolution and blanking porches. Please fill them according to the datasheet of your LCD.

    LCD RGB frame buffer operation modes
    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    .. _bounce_buffer_with_single_psram_frame_buffer:

    Bounce Buffer with Single PSRAM Frame Buffer
    ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    With regards to how the framebuffer is accessed and where it is located, the RGB LCD panel driver can operate in four modes:

    - Framebuffer in internal memory.
    - Framebuffer in PSRAM, accessed using EDMA
    - Framebuffer in PSRAM, smaller bounce buffers in internal memory.
    - No framebuffer in driver, bounce buffers in internal memory filled by callback.

    The first option (framebuffer in internal memory) is the default and simplest. There is a framebuffer in internal memory that is read out once a frame using DMA and the data is sent out to the LCD verbatim. It needs no CPU intervention to function, but it has the downside that it uses up a fair bit of the limited amount of internal memory. This is the default if you do not specify flags or bounce buffer options.

    The second option is useful if you have PSRAM and want to store the framebuffer there rather than in the limited internal memory. The LCD peripheral will use EDMA to fetch frame data directly from the PSRAM, bypassing the internal cache. If you use this, after writing to the framebuffer, make sure to use e.g. Cache_WriteBack_Addr to make sure the framebuffer is actually written back to the PSRAM. Not doing this will lead to image corruption. The downside of this is that when both the CPU as well as peripherals need access to the EDMA, the bandwidth will be shared between the two, that is, EDMA gets half and the CPUs the other half. If there's other peripherals using EDMA as well, with a high enough pixel clock this can lead to starvation of the LCD peripheral, leading to display corruption. However, if the pixel clock is low enough for this not to be an issue, this is a solution that uses almost no CPU intervention. This option can be enabled by setting the ``fb_in_psram`` flag.

    The third option makes use of two so-called 'bounce buffers' in internal memory, but a main framebuffer that is still in PSRAM. These bounce buffers are buffers large enough to hold e.g. a few lines of display data, but still significantly less than the main framebuffer. The LCD peripheral will use DMA to read data from one of the bounce buffers, and meanwhile an interrupt routine will use the CPU to copy data from the main PSRAM framebuffer into the other bounce buffer. Once the LCD peripheral has finished reading the bounce buffer, the two buffers change place and the CPU can fill the others. Note that as the CPU reads the framebuffer data through the cache, it's not needed to call Cache_WriteBack_Addr() anymore. The advantage here is that, as it's easier to control CPU memory bandwith use than EDMA memory bandwith use, doing this can lead to higher pixel clocks being supported. As the bounce buffers are larger than the FIFOs in the EDMA path, this method is also more robust against short bandwidth spikes. The downside is a major increase in CPU use. This mode is selected by setting the ``fb_in_psram`` flag and additionally specifying a (non-zero) bounce_buffer_size_px value. This value is dependent on your use case, but a suggested initial value would be e.g. 8 times the amount of pixels in one LCD line.

    Note that this third option also allows for a ``bb_do_cache_invalidate`` flag to be set. Enabling this frees up the cache lines after they're used to read out the framebuffer data from PSRAM, but it may lead to slight corruption if the other core writes data to the framebuffer at the exact time the cache lines are freed up. (Technically, a write to the framebuffer can be ignored if it falls between the cache writeback and the cache invalidate calls.)

    Finally, the fourth option is the same as the third option, but there is no PSRAM frame buffer initialized by the LCD driver. Instead, the user supplies a callback function that is responsible for filling the bounce buffers. As this driver does not care where the written pixels come from, this allows for the callback doing e.g. on-the-fly conversion from a smaller, 8-bit-per-pixel PSRAM framebuffer to an 16-bit LCD, or even procedurally-generated framebuffer-less graphics. This option is selected by not setting the ``fb_in_psram`` flag but supplying both a ``bounce_buffer_size_px`` value as well as a ``on_bounce_empty`` callback.

    .. note::

        It should never happen in a well-designed embedded application, but it can in theory be possible that GDMA cannot deliver data as fast as the LCD consumes it. In the ESP32-S3 hardware, this leads to the LCD simply outputting dummy bytes while GDMA waits for data. If we were to run DMA in a simple circular fashion, this would mean a de-sync between the LCD address the GDMA reads the data for and the LCD address the LCD peripheral thinks it outputs data for, leading to a permanently shifted image.
        In order to stop this from happening, we restart GDMA in the VBlank interrupt; this way we always know where it starts. However, the LCD peripheral also has a FIFO, and at the time of the VBlank, it already has read some data in there. We cannot reset this FIFO entirely, there's always one pixel that remains. So instead, when we restart DMA, we take into account it does not need to output the data that already is in the FIFO and we restart it using a descriptor that starts at the position after the last pixel in the LCD fifo.

Application Example
-------------------

LCD examples are located under: :example:`peripherals/lcd`:

* Jpeg decoding and LCD display - :example:`peripherals/lcd/tjpgd`
* i80 controller based LCD and LVGL animation UI - :example:`peripherals/lcd/i80_controller`
* GC9A01 user customized driver and dash board UI - :example:`peripherals/lcd/gc9a01`
* RGB panel example with lvgl demo music - :example:`peripherals/lcd/rgb_panel`
* I2C interfaced OLED display scrolling text - :example:`peripherals/lcd/i2c_oled`

API Reference
-------------

.. include-build-file:: inc/lcd_types.inc
.. include-build-file:: inc/esp_lcd_types.inc
.. include-build-file:: inc/esp_lcd_panel_io.inc
.. include-build-file:: inc/esp_lcd_panel_ops.inc
.. include-build-file:: inc/esp_lcd_panel_rgb.inc
.. include-build-file:: inc/esp_lcd_panel_vendor.inc
