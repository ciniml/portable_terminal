# M5GFX / M5Unified — ESP-IDF 6.0 patch notes

`components/M5GFX` and `components/M5Unified` are vendored as git submodules.
Upstream (M5GFX 0.2.20 / M5Unified 0.2.14) targets ESP-IDF 5.x and does not
build cleanly under ESP-IDF 6.0 / ESP32-P4 because IDF 6.0:

- Removed the legacy `driver/i2s.h` shim (split into `esp_driver_i2s` with a
  redesigned API; `i2s_port_t` typedef is gone).
- Removed `SOC_I2S_NUM` and `I2S_NUM_MAX` from `soc_caps.h` (replaced by
  per-channel handle APIs).
- Removed `esp_lcd_dpi_panel_enable_dma2d()` (DMA2D is now enabled at panel
  creation time via `esp_lcd_dpi_panel_config_t::flags.use_dma2d`).
- Split per-peripheral driver components — `esp_driver_ledc`,
  `esp_driver_gpio`, `esp_driver_spi`, `esp_driver_i2c`, `esp_driver_i2s`,
  `esp_driver_rmt`, etc. — that previously lived under the umbrella `driver`
  component. The legacy `driver` component now ships only i2c / touch / twai.

The patches below are applied **in-place to the submodule worktrees** — the
submodules show as dirty in `git status`. Until upstream gains official
IDF 6.0 / Tab5 support, treat these as a working fork. To re-sync upstream:
`git submodule update` will overwrite our edits, so reapply them from this doc.

## components/M5GFX

### `CMakeLists.txt` — IDF 6 REQUIRES branch
Add an `IDF_VERSION_MAJOR GREATER_EQUAL 6` branch listing the new per-driver
components plus `esp_psram` and `esp_rom`:
```cmake
set(COMPONENT_REQUIRES nvs_flash efuse driver esp_timer esp_lcd esp_mm
    esp_driver_ledc esp_driver_i2s esp_driver_gpio esp_driver_spi
    esp_driver_i2c esp_psram esp_rom)
```

### `src/lgfx/v1/platforms/esp32/Bus_Parallel8.hpp` — provide `i2s_port_t`
Inject a `typedef int i2s_port_t;` shim under IDF 6.0 so the `config_t`
struct's `i2s_port` field still compiles. Bus_Parallel8 is not actually used
on ESP32-P4 at runtime — this is only to keep the header parseable.

### `src/lgfx/v1/platforms/esp32p4/Panel_DSI.cpp` — DMA2D init
Drop the IDF-6-only call to the removed `esp_lcd_dpi_panel_enable_dma2d()`
and set `dpi_config.flags.use_dma2d = true` unconditionally before
`esp_lcd_new_panel_dpi()` (works on both 5.x and 6.0).

## components/M5Unified

### `CMakeLists.txt` — IDF 6 REQUIRES branch
```cmake
set(COMPONENT_REQUIRES M5GFX esp_adc driver
    esp_driver_i2c esp_driver_gpio esp_driver_i2s esp_driver_ledc
    esp_driver_rmt)
```

### `src/utility/Speaker_Class.hpp` and `Mic_Class.hpp`
Inject under IDF 6:
```cpp
typedef int i2s_port_t;
#ifndef SOC_I2S_NUM
 // hard-code per chip; IDF 6 dropped this from soc_caps.h
#endif
#ifndef I2S_NUM_MAX
 #define I2S_NUM_MAX SOC_I2S_NUM
#endif
```
Change struct field initializer from `i2s_port_t::I2S_NUM_0` to
`(i2s_port_t)I2S_NUM_0` (works for both IDF 5 enum and IDF 6 typedef-int).

### `src/utility/Speaker_Class.cpp` and `Mic_Class.cpp`
Replace each `i2s_port_t::I2S_NUM_<n>` literal in the `dev = &I2S<n>`
selector with `(i2s_port_t)I2S_NUM_<n>`.

## Audio status

The shims above let Speaker/Mic **compile** under IDF 6.0; their actual
runtime behavior is **not validated**. The IDF 6 I2S API is sufficiently
different that the audio code paths will likely need a proper port before
audio works. Phase 1b only exercises display rendering, so this is deferred.

## espressif/esp_hosted 2.12.7 — Tab5 SDIO link

`managed_components/espressif__esp_hosted` is fetched by the IDF component
manager (not a submodule); `git status` does not surface its dirty state.
The single patch below is required for Wi-Fi via the on-board C6 to come
up on Tab5; without it `sdmmc_init_ocr` (CMD5 / `send_op_cond`) times out
with `0x107` forever.

If the managed component is re-downloaded (`rm -rf
managed_components/espressif__esp_hosted` + rebuild), the patch must be
re-applied.

### `host/drivers/transport/sdio/sdio_drv.c` — `transport_gpio_reset()`

Upstream's reset pulse leaves the slave's CHIP_EN line driven to the
`ACTIVE` level on exit:
```c
write_gpio(reset_pin, H_RESET_VAL_ACTIVE);
msleep(10);
write_gpio(reset_pin, H_RESET_VAL_INACTIVE);
msleep(10);
write_gpio(reset_pin, H_RESET_VAL_ACTIVE);    /* <-- chip ends in reset */
msleep(H_HOST_SDIO_RESET_DELAY_MS);
```
On Tab5 the reset GPIO (`GPIO 15`) is wired straight to the C6's CHIP_EN
(active high), so `H_RESET_VAL_ACTIVE` = LOW = chip held disabled. The
subsequent `sdmmc_card_init()` issues CMD5 against a chip that's still in
reset and times out.

In esp_hosted 1.4 (the version M5Tab5-UserDemo uses on IDF 5.4) this
worked because the SDMMC bus was initialised inside
`transport_init_internal()` — BEFORE the reset pulse, while the C6 was
still in its freshly-booted state. In 2.x the order is reversed.

The local patch comments out the trailing `ACTIVE` write so the C6 is
left running (`H_RESET_VAL_INACTIVE` = HIGH) when CMD5 finally arrives.

## espressif/esp_hosted 1.4.0 — c6_updater quirks

The IDF 5.4 updater under `c6_updater/` pulls in `espressif/esp_hosted
==1.4.0`, which still misses two REQUIRES in its CMakeLists.txt:

- `esp_wifi` (its public headers expose `esp_wifi.h`)
- `esp_driver_sdmmc` (its public headers expose `driver/sdmmc_host.h`)

These are declared in `c6_updater/main/CMakeLists.txt` PRIV_REQUIRES so
the linker pulls them in even though the esp_hosted manifest doesn't.

The updater also patches the slave's `idf_component.yml` at build time
(`slave_c6_fw/build.sh`) to drop the broken `cmd_system` example
dependency — IDF 6.0's `cmd_system_sleep.c` calls
`esp_sleep_get_wakeup_causes()` without listing `esp_hw_support` in its
REQUIRES, so the slave build dies before producing
`network_adapter.bin`. We don't use cmd_system, so the dep is just
deleted.
