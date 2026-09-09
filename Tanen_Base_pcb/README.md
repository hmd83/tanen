# TanenBase carrier PCB

KiCad 7+ project for the TanenBase carrier board — the passive breakout that
holds the XIAO module, the Wio-SX1262 LoRa kit, the load-cell terminal block,
the DS18B20 terminal block, and the battery connections.

| File | Contents |
|------|----------|
| `XIAO_nRF54L15 V1.kicad_sch` | Schematic |
| `XIAO_nRF54L15 V1.kicad_pcb` | Board layout |
| `XIAO_nRF54L15_V1_Gerber/` | Gerbers + drill files (as fabricated) |
| `XIAO_nRF54L15_V1_Gerber.zip` | Same, zipped for the fab house |
| `simulation_image_top.png` / `_bottom.png` | 3D renders |

---

## Why is this named "nRF54L15" when the firmware targets the nRF54LM20A?

**Because the board does not care, and renaming it would invalidate a PCB that
has already been fabricated and verified.**

The Seeed Studio XIAO family is a fixed mechanical and electrical standard: all
XIAO modules share the same **21 × 17.5 mm** outline, the same two 7-pin 2.54 mm
headers, and the same pin *roles* in the same physical positions:

```
        ┌──────────────┐
   5V ──┤              ├── D0
  GND ──┤              ├── D1
  3V3 ──┤     XIAO     ├── D2
  D10 ──┤   21×17.5mm  ├── D3
   D9 ──┤              ├── D4
   D8 ──┤              ├── D5
   D7 ──┤              ├── D6
        └──────────────┘
```

The **XIAO nRF54L15** and the **XIAO nRF54LM20A** are drop-in mechanical
replacements for each other on this carrier. The footprint, the outline, the
castellated pads, the header pitch, and the power pin positions are identical.
Only the GPIO number sitting behind each header pin differs, and that is a
firmware concern, resolved entirely in the devicetree overlay
[`boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.overlay`](../boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.overlay).

So the KiCad project *filename* is historical — it records the module that was in
the socket when the board was laid out. The board itself is silkscreened
**"Tanen Base v0.1"**, not with any module name. The gerbers in this directory
are the ones that were actually fabricated; renaming the project would mean
regenerating them for zero electrical benefit.

### Header role mapping across both modules

Firmware keeps the header **roles** fixed, so the carrier's netlist is valid for
either module:

| XIAO Pin | Carrier net | nRF54L15 GPIO | nRF54LM20A GPIO |
|----------|-------------|---------------|-----------------|
| D0 | DS18B20 data (1-Wire) — mandatory K1 cut, see below | P1.04 | P1.00 |
| D1 | SX1262 DIO1 | P1.05 | P1.31 |
| D2 | SX1262 RST + external TanenButton | P1.06 | P1.30 |
| D3 | SX1262 BUSY | P1.07 | P1.29 |
| D4 | SX1262 NSS | P1.10 | P1.03 |
| D5 | LORA_RF_SW1 (Wio-SX1262 ant. switch) | P1.11 | P1.07 |
| D6 | NAU7802 SCL | P2.08 | P1.08 |
| D7 | NAU7802 SDA | P2.07 | P1.09 |
| D8 | SPI SCK | P2.01 | P1.04 |
| D9 | SPI MISO | P2.04 | P1.05 |
| D10 | SPI MOSI | P2.02 | P1.06 |

### Mandatory rework: cut the Wio-SX1262 K1 button off D0

The Wio-SX1262 for XIAO ships a user button **K1 (TS-1188E)** and a **10 kΩ
pull-up R2 to +3V3** on **D0** — the net this carrier uses for the DS18B20
1-Wire data line.

**This is not optional and not a corner case: with K1/R2 fitted, 1-Wire does not
read at all.** Observed on hardware. R2 lands in parallel with the carrier
1-Wire pull-up (~3.2 kΩ effective) on top of the switch stub loading; the exact
failure mechanism was not chased down because the cut fixes it outright.

**The cut:** the single trace from the K1/R2 node to the D0 castellated pad on
the Wio board. One cut lifts *both* K1 and R2 off D0 and leaves the D0
pass-through (XIAO → carrier → DS18B20 terminal) intact. Verify by continuity:
XIAO D0 still reaches the DS18B20 terminal, K1 node now isolated from D0.
**Done on the units built so far** — repeat it on every new Wio-SX1262 before
assembly, or temperature reads will fail on that unit.

**Optional afterwards:** the cut leaves K1 + R2 as a self-contained
button-with-pull-up, currently isolated and unused. Wiring that node to **P0.09**
on the XIAO — in parallel with the module own user button — makes K1 a second
TanenButton with **no firmware change**: P0.09 is already the armed System OFF
wake source (`gpio_disconnect_all()` in [`src/core/power.c`](../src/core/power.c))
and R2 reinforces the internal pull-up. P0.09 is not on any header, so that wire
has to land on the XIAO on-board switch pad.

### External button: D2 (P1.30) — shares the SX1262 NRESET net

> **Field-verified 2026-09-09** — bare switch to GND on D2, wake from System OFF
> into SETUP, no issues observed.

The second TanenButton lands on **D2 = P1.30**, the same net as the SX1262
**NRESET**. D2 is on a 7-pin header, so **carrier v0.1 already routes it** — no
flying wire, and v0.2 needs no new net. (It was on D19 = P0.00, a bottom pad,
until 2026-09-07; **D19 is now free**.)

Wiring: a plain momentary switch from D2 to GND. Nothing else — no pull-up, no
cap. The SoC's internal pull-up (R_PU = 12 / 14 / 16 kΩ min/typ/max) holds the
line high while the pin is sense-armed in System OFF, and that is also what
keeps the SX1262 out of reset.

**Why sharing a reset line is acceptable here.** SETUP is a once-a-year
calibration action and the station is asleep 99.8 % of the time (185 s awake per
day at T_meas = 900 s / T_tx = 14400 s, from `docs/POWER_BUDGET.md` §1):

- **Asleep (99.8 %)** — press pulls NRESET low: the SoC wakes *and* the SX1262
  resets. The radio reset is free, because every wake is a full SoC reset and
  the sx126x driver drives NRESET low at device init anyway.
- **Awake (0.2 %)** — the pin is a driver-owned output, so the press is not
  latched and is simply lost. Shorting a standard-drive pad to GND is harmless:
  I_OH,SD is 1 / 3 / 4 mA against a 15 mA recommended all-GPIO budget. The SETUP
  LED is the user's feedback — no LED, press again.
- **Switch fails closed** — NRESET held low: wake loop draining the cell *and* a
  radio stuck in reset. Missing uplinks make it visible server-side within one
  heartbeat interval.

Add **100 nF to GND** at the switch only if the button wire leaves the
enclosure. On D19 a coupled glitch cost a spurious boot; on D2 it also resets
the radio, possibly mid-TX. 100 nF against the ~14 kΩ pull-up is ~1.4 ms —
harmless next to the driver's 20 ms reset pulse.

Port note: any wake-capable port works. The nRF54LM20A datasheet *Port
capabilities* table lists P0, P1 and P3 as wakeup sources with pin sense/detect
and GPIOTE; **P2 has none of the three** and cannot wake the system at all.
Avoid D22/D23 (P0.03/P0.04 carry GRTC PWM / CLKOUT32K).

Firmware already supports it: `button_arm_sense()` in
[`src/core/power.c`](../src/core/power.c) arms P0.09 and P1.30 identically, and
either press wakes into SETUP.

### Differences that *are* worth knowing

These do not affect the carrier board, but they change firmware behaviour:

- **Battery sensing.** The nRF54L15 module has no PMIC — the L15 firmware read
  VBAT through a divider on an ADC pin. The **nRF54LM20A carries an on-board
  Nordic nPM1300**, and the firmware reads its factory-trimmed VBAT ADC instead.
  Nothing on the carrier changed; the divider was on the module side.
- **Antenna switch.** The L15 build had to sequence an RF switch on P2.03/P2.05.
  On the LM20A those pins belong to the on-board NOR flash. The Wio-SX1262 kit
  brings its own switch control out to header pin **D5 = P1.07 (LORA_RF_SW1)**;
  the firmware never drives it, because the radio is configured
  `dio2-tx-enable` and the SX1262 switches the path from DIO2. D5 is therefore
  **committed to an RF control net and is not a spare GPIO** — do not put a
  button, pull-up, or anything else on it.
- **On-board NOR flash.** The LM20A has a py25q64 on P2.00–P2.05. Firmware must
  put it into deep power-down before System OFF or it costs ~14 µA. Again,
  module-side only.

---

## Fabrication notes

- 2-layer board, standard 1.6 mm FR4, HASL or ENIG both fine.
- No components on the bottom layer other than the silkscreen.
- The 470 µF low-ESR bulk capacitor on the battery rail is **mandatory**, not
  optional — the LoRa TX burst peaks around 92 mA and the chosen ER14505 AA
  Li-SOCl₂ cell is rated 100 mA continuous. See
  [`docs/POWER_BUDGET.md`](../docs/POWER_BUDGET.md) §6.
- The battery input is wired straight to the XIAO **BAT** pin, i.e. the nPM1300
  VBAT input: **2.3 V min, 4.45 V max recommended, 5.5 V absolute**. Anything
  connected there must stay inside that window at fresh open-circuit *and* at
  end of life. See `docs/POWER_BUDGET.md` §8.

> **Safety:** the firmware deletes `charging-enable` from the nPM1300 charger
> node on purpose. This design runs a **primary, non-rechargeable** Li-SOCl₂
> cell. Do not fit a rechargeable cell and do not re-enable charging without
> changing both together.

---

## License

The hardware design files in this directory are released under the same
[Apache-2.0](../LICENSE) license as the rest of the project.
