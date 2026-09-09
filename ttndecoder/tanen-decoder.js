// TanenBase -> TTN payload formatter (unified: BEEP + beelogger)
// Emits both key sets; each server keeps the keys it knows and ignores the rest.
//
// TanenBase payload layout (8 bytes):
//   [0-2]  weight   uint24 BE, grams          (0xFFFFFF = invalid)
//   [3-4]  temp     int16  BE, centi-Celsius  (0x7FFF   = invalid)
//   [5-6]  battery  uint16 BE, millivolts     (0xFFFF   = invalid)
//   [7]    flags    bit0=anomaly_weight bit1=anomaly_temp bit2=heartbeat
//
// Keys:
//   BEEP      : weight_kg (kg) · t (degC) · bv (V)
//   beelogger : Gewicht  (kg) · TempOut (degC) · VBatt (V)
function decodeUplink(input) {
  var b = input.bytes;
  if (b.length < 8) {
    return { data: {}, warnings: ["Payload too short"] };
  }

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

  return {
    data: {
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
      heartbeat:      !!(flags & 0x04)
    }
  };
}
