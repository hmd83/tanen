# 🛠️ بيئة البرمجة — مشروع Tanen

**التاريخ:** 2026-08-03
**الهدف:** توثيق إعداد بيئة التطوير لمشروع TanenBase على سيرفر Hermes

## المشروع

**TanenBase** — مراقب خلايا نحل LoRaWAN منخفض الطاقة:
- **MCU:** Seeed Studio XIAO nRF54LM20A
- **Radio:** Wio-SX1262 LoRa (LoRaWAN Class A, OTAA, EU868)
- **Sensors:** NAU7802 (load-cell) · DS18B20 (temp) · nPM1300 (battery)
- **SDK:** Zephyr 4.4 عبر nRF Connect SDK v3.4.0
- **Sleep:** ~4 µA — **~9 سنوات** على بطارية AA

## البنية

| المسار | الوصف |
|---|---|
| `src/` | Firmware — core/ (FSM, power, watchdog) + drivers/ + features/ |
| `boards/` | Application devicetree overlay |
| `third_party/` | Seeed XIAO nRF54LM20A board definition |
| `Tanen_Base_pcb/` | KiCad carrier board (schematic, layout, gerbers) |
| `web/` | Web Bluetooth config page (ملف واحد) |
| `ttndecoder/` | TTN payload formatters |
| `test_apps/measure_loop/` | Standalone sensor bring-up |

## البيئة على السيرفر (2026-08-03)

### مثبّت ✅
- `west 1.5.0` + كل متطلبات Python (pyelftools, PyYAML, pykwalify, jsonschema, canopen, packaging, patool, psutil, pylink-square, pyserial, requests, semver)
- `ninja` + `cmake` (نظام)
- Python 3.13 (عبر venv `/opt/data/pylibs`)
- .NET 10 SDK (`/opt/data/dotnet`) — لـ AgOpenWeb
- **nRF Connect SDK v3.4.0** كامل في `/opt/data/ncs` (5.7GB — Zephyr 4.4 + nrf + nrfxlib + modules)
- **Zephyr SDK 1.0.1** في `/opt/data/zephyr-sdk-1.0.1` (829MB) — toolchain `arm-zephyr-eabi` (gcc 14.3.0) في `gnu/`

### ✅ تم التحقق من البناء (2026-08-03)
```bash
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d build_test . \
  --no-sysbuild -- -DBOARD_ROOT=/opt/data/projects/tanen/third_party/seeed-xiao-nrf54lm20a
```
**النتيجة:** نجح — FLASH 11.95% / RAM 10.45% — `build_test/zephyr/zephyr.{elf,hex,bin}`

### أخطاء واجهناها وحلولها
| الخطأ | الحل |
|---|---|
| `No board named 'xiao_nrf54lm20a'` | أضف `-DBOARD_ROOT=.../third_party/seeed-xiao-nrf54lm20a` (بعد `--`) |
| `ZEPHYR_EXTRA_MODULES not a valid module` | لا تستخدمه — استخدم BOARD_ROOT |
| `Unable to find 'x86_64-zephyr-elf' in .../gnu` | toolchain لازم يكون بمسار `gnu/arm-zephyr-eabi/` داخل الـ SDK |
| MCUboot/sysbuild configure fail | استخدم `--no-sysbuild` (موثق في README — sysbuild غير مدعوم على LM20A) |

### مطلوب لاحقاً
- **KiCad** (للـ PCB — اختياري على السيرفر، أهم على جهاز المستخدم)

## أوامر البناء

```bash
export PATH=/opt/data/pylibs/bin:$PATH
export ZEPHYR_BASE=/opt/data/ncs/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

# من مجلد المشروع:
cd /opt/data/projects/tanen
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp .
```

## ملاحظات

- سكربتات `build.ps1` / `flash_*.ps1` مصممة لـ **Windows** — على Linux نستخدم west مباشرة
- `prj_credentials.conf.example` → انسخه لـ `prj_credentials.conf` مع مفاتيح TTN
- الـ sysbuild (MCUboot/OTA) غير موثّق على LM20A/NCS v3.4.0 — استخدم `--no-sysbuild`
