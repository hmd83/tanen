# Vendored: Seeed XIAO nRF54LM20A Zephyr board definition

Out-of-tree Zephyr board support for the **Seeed Studio XIAO nRF54LM20A**, taken
verbatim from Seeed's PlatformIO platform repository
(<https://github.com/Seeed-Studio/platform-seeedboards>).

Only the single board definition is vendored here — the upstream repo is ~33 MB
of PlatformIO tooling, Arduino cores, and board definitions this project does not
use.

| Here | Upstream |
|------|----------|
| `boards/arm/xiao_nrf54lm20a/` | `zephyr/boards/arm/xiao_nrf54lm20a/` |

The intermediate `zephyr/` level is dropped because Zephyr's `BOARD_ROOT` must
point at the directory *containing* `boards/`.

## Why vendored at all

The upstream repo ships no `zephyr/module.yml`, so `west` cannot pick it up as a
Zephyr module. The board must be passed explicitly:

```
-DBOARD_ROOT=<repo>/third_party/seeed-xiao-nrf54lm20a
```

`build.ps1` and `build_test.ps1` do this automatically.

## Local modifications

**None.** Files are unmodified from upstream. Board-level fixes that TanenBase
needs live in the application overlay
[`boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.overlay`](../../boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.overlay)
instead, so this directory can be re-synced from upstream without merge work.
The one that matters:

- The board `.dtsi` declares the nPM1300 GPIO-I2C on **P1.15/P1.16**. The actual
  wiring is **SDA P1.18 / SCL P1.17** (matching Seeed's own `zephyr-battery`
  example). The app overlay re-points `&pmic_i2c`.

## License

Apache-2.0, © Seeed Technology Co., Ltd. See [LICENSE](LICENSE).
