// TanenBase → beelogger communityServer payload formatter
// يحول payload الـ 8 بايت تبع TanenBase إلى JSON بأسماء حقول beelogger
// (Gewicht/TempOut/VBatt) يفهمها الـ community server
//
// بنية payload TanenBase (8 بايت):
//   [0-2]  الوزن     uint24 BE بالغرام     (0xFFFFFF = غير صالح)
//   [3-4]  الحرارة   int16  BE بسنتي-سيلسيوس (0x7FFF = غير صالح)
//   [5-6]  البطارية  uint16 BE بالميلي-فولت (0xFFFF = غير صالح)
//   [7]    flags     bit0=anomaly_weight bit1=anomaly_temp bit2=heartbeat
//
// الوحدات المتوقعة من beelogger:
//   Gewicht: كيلوغرام (من الـ payloadformatter_220227.js: /100)
//   TempOut: درجة مئوية (/100)
//   VBatt:   فولت     (/1000)
function decodeUplink(input) {
  var b = input.bytes;
  if (b.length < 8) {
    return { data: {}, warnings: ["Payload too short"] };
  }

  // الوزن — غرام → كيلوغرام
  var weight_g = (b[0] << 16) | (b[1] << 8) | b[2];
  var weight_valid = weight_g !== 0xFFFFFF;
  var Gewicht = weight_valid ? Math.round(weight_g) / 1000 : null;

  // الحرارة — سنتي-سيلسيوس → °C (موقعة)
  var temp_cc = (b[3] << 8) | b[4];
  if (temp_cc > 32767) temp_cc -= 65536;
  var temp_valid = temp_cc !== 32767;
  var TempOut = temp_valid ? temp_cc / 100.0 : null;

  // البطارية — ميلي-فولت → فولت
  var batt_mv = (b[5] << 8) | b[6];
  var batt_valid = batt_mv !== 0xFFFF;
  var VBatt = batt_valid ? batt_mv / 1000.0 : null;

  var flags = b[7];

  return {
    data: {
      // مفاتيح beelogger — الـ community server يخزن هذه فقط
      Gewicht: Gewicht,
      TempOut: TempOut,
      VBatt:   VBatt,

      // حقول تشخيصية إضافية (يراهما فقط من يراقب TTN live)
      flags:          flags,
      anomaly_weight: !!(flags & 0x01),
      anomaly_temp:   !!(flags & 0x02),
      heartbeat:      !!(flags & 0x04)
    }
  };
}
