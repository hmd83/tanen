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
| D0 | DS18B20 data (1-Wire) | P1.04 | P1.00 |
| D1 | SX1262 DIO1 | P1.05 | P1.31 |
| D2 | SX1262 RST | P1.06 | P1.30 |
| D3 | SX1262 BUSY | P1.07 | P1.29 |
| D4 | SX1262 NSS | P1.10 | P1.03 |
| D5 | *(unused)* | P1.11 | P1.07 |
| D6 | NAU7802 SCL | P2.08 | P1.08 |
| D7 | NAU7802 SDA | P2.07 | P1.09 |
| D8 | SPI SCK | P2.01 | P1.04 |
| D9 | SPI MISO | P2.04 | P1.05 |
| D10 | SPI MOSI | P2.02 | P1.06 |

### Differences that *are* worth knowing

These do not affect the carrier board, but they change firmware behaviour:

- **Battery sensing.** The nRF54L15 module has no PMIC — the L15 firmware read
  VBAT through a divider on an ADC pin. The **nRF54LM20A carries an on-board
  Nordic nPM1300**, and the firmware reads its factory-trimmed VBAT ADC instead.
  Nothing on the carrier changed; the divider was on the module side.
- **Antenna switch.** The L15 build had to sequence an RF switch on P2.03/P2.05.
  On the LM20A those pins belong to the on-board NOR flash, and the module
  handles its own antenna path.
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
