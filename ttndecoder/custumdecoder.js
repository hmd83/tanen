function decodeUplink(input) {
  var b = input.bytes;
  if (b.length < 8) {
    return { data: {}, warnings: ["Payload too short"] };
  }

  // Weight: uint24 BE, grams. 0xFFFFFF = sensor invalid.
  var weight_g = (b[0] << 16) | (b[1] << 8) | b[2];
  var weight_valid = weight_g !== 0xFFFFFF;

  // Temp: int16 BE, centi-Celsius, signed. 0x7FFF = sensor invalid.
  var temp_cc = (b[3] << 8) | b[4];
  if (temp_cc > 32767) temp_cc -= 65536;
  var temp_valid = temp_cc !== 32767;

  // Battery: uint16 BE, mV. 0xFFFF = sensor invalid.
  var batt_mv = (b[5] << 8) | b[6];
  var batt_valid = batt_mv !== 0xFFFF;

  var flags = b[7];

  return {
    data: {
      // مفاتيح BEEP القياسية — هذه فقط ما سيُخزَّن
      weight_kg: weight_valid ? Math.round(weight_g / 10) / 100 : null,
      t:         temp_valid ? temp_cc / 100.0 : null,
      bv:        batt_valid ? Math.round(batt_mv / 10) / 100 : null,

      // حقول تشخيصية — BEEP سيتجاهلها (تظهر فقط في Live data بـ TTN)
      flags:          flags,
      anomaly_weight: !!(flags & 0x01),
      anomaly_temp:   !!(flags & 0x02),
      heartbeat:      !!(flags & 0x04)
    }
  };
}