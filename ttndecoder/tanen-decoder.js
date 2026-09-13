// TanenBase -> TTN payload formatter (unified: BEEP + beelogger)
// Emits both key sets; each server keeps the keys it knows and ignores the rest.
//
// TanenBase payload layout (8 bytes + 5 per BLE sensor heard):
//   [0-2]  weight   uint24 BE, grams          (0xFFFFFF = invalid)
//   [3-4]  temp     int16  BE, centi-Celsius  (0x7FFF   = invalid)  1-Wire probe
//   [5-6]  battery  uint16 BE, millivolts     (0xFFFF   = invalid)
//   [7]    flags    bit0=anomaly_weight bit1=anomaly_temp bit2=heartbeat
//                   bit3=ext_missing (an enabled BLE sensor was not heard)
// Extended Mode, 0-3 blocks of 5 bytes, one per SwitchBot sensor heard:
//   [0]    desc     bit7-4 type (1 = SwitchBot T/H), bit2 role (1 = outside),
//                   bit1-0 slot
//   [1-2]  temp     int16 BE, centi-Celsius
//   [3]    humidity uint8, %RH
//   [4]    battery  uint8, %  (0xFF = unknown)
//
// Keys:
//   BEEP      : weight_kg (kg) · t (degC) · bv (V)
//               h · t_i · h_i · t_1 · t_0   (Extended Mode)
//   beelogger : Gewicht  (kg) · TempOut (degC) · VBatt (V)
//               FeuchteOut · TempIn · FeuchteIn · TempIn2 · FeuchteIn2
// An outside BLE sensor takes over t / TempOut; the 1-Wire probe then moves
// to t_0. Without one, t / TempOut stay the 1-Wire probe.
var EXT_BLOCK_LEN = 5;
var EXT_TYPE_SWITCHBOT_TH = 1;

function decodeUplink(input) {
  var b = input.bytes;
  if (b.length < 8) {
    return { data: {}, warnings: ["Payload too short"] };
  }
  var warnings = [];

  // Weight - grams -> kilograms
  var weight_g = (b[0] << 16) | (b[1] << 8) | b[2];
  var weight_valid = weight_g !== 0xFFFFFF;

  // Temp - centi-Celsius -> degC (signed)
  var temp_cc = (b[3] << 8) | b[4];
  if (temp_cc > 32767) temp_cc -= 65536;
  var temp_valid = temp_cc !== 32767;
  var temp_c = temp_valid ? temp_cc / 100.0 : null;

  // Battery - millivolts -> volts
  var batt_mv = (b[5] << 8) | b[6];
  var batt_valid = batt_mv !== 0xFFFF;

  var flags = b[7];

  var data = {
    // BEEP - quantized (10 g / 10 mV) as in the original decoder
    weight_kg: weight_valid ? Math.round(weight_g / 10) / 100 : null,
    t:         temp_c,
    bv:        batt_valid ? Math.round(batt_mv / 10) / 100 : null,

    // beelogger - full precision
    Gewicht: weight_valid ? weight_g / 1000 : null,
    TempOut: temp_c,
    VBatt:   batt_valid ? batt_mv / 1000.0 : null,

    // Diagnostics - ignored by both servers (visible in TTN live data only)
    flags:          flags,
    anomaly_weight: !!(flags & 0x01),
    anomaly_temp:   !!(flags & 0x02),
    heartbeat:      !!(flags & 0x04),
    ext_missing:    !!(flags & 0x08),
    ext_count:      0
  };

  // Extended Mode - the length alone says how many blocks follow
  var ext_len = b.length - 8;
  if (ext_len % EXT_BLOCK_LEN !== 0) {
    warnings.push("Unknown extension length " + ext_len + " bytes - BLE sensors ignored");
    ext_len = 0;
  }

  var have_out = false;
  var in_count = 0;
  for (var i = 8; i < 8 + ext_len; i += EXT_BLOCK_LEN) {
    var desc = b[i];
    if ((desc >> 4) !== EXT_TYPE_SWITCHBOT_TH) {
      warnings.push("Unknown BLE block type " + (desc >> 4) + " - rest ignored");
      break;
    }
    var outside = !!(desc & 0x04);
    var t_cc = (b[i + 1] << 8) | b[i + 2];
    if (t_cc > 32767) t_cc -= 65536;
    var t = t_cc / 100.0;
    var h = b[i + 3];
    var bat = b[i + 4] === 0xFF ? null : b[i + 4];

    if (outside) {
      if (have_out) {
        warnings.push("Second outside sensor ignored (slot " + (desc & 0x03) + ")");
        continue;
      }
      have_out = true;
      // The 1-Wire probe gives up t / TempOut to the outside sensor
      data.t_0 = temp_c;
      data.t_1wire = temp_c;
      data.t = t;
      data.h = h;
      data.TempOut = t;
      data.FeuchteOut = h;
      data.bat_out = bat;
    } else if (in_count === 0) {
      in_count++;
      data.t_i = t;
      data.h_i = h;
      data.TempIn = t;
      data.FeuchteIn = h;
      data.bat_in1 = bat;
    } else if (in_count === 1) {
      in_count++;
      data.t_1 = t;
      data.h_i2 = h;      // BEEP has no second inside humidity key
      data.TempIn2 = t;
      data.FeuchteIn2 = h;
      data.bat_in2 = bat;
    } else {
      warnings.push("Third inside sensor ignored (slot " + (desc & 0x03) + ")");
      continue;
    }
    data.ext_count++;
  }

  var result = { data: data };
  if (warnings.length) result.warnings = warnings;
  return result;
}
