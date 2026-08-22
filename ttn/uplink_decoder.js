function u16(msb, lsb) {
  return (msb << 8) | lsb;
}

function i16(msb, lsb) {
  var value = (msb << 8) | lsb;
  if (value & 0x8000) value -= 0x10000;
  return value;
}

function decodeUplink(input) {
  var b = input.bytes;

  if (b.length < 10) {
    return {
      data: {},
      warnings: ["Payload too short. Expected 10 bytes."]
    };
  }

  var temperature_c = i16(b[0], b[1]) / 100;
  var humidity_percent = u16(b[2], b[3]) / 100;
  var pm25_ugm3 = u16(b[4], b[5]);
  var pm10_ugm3 = u16(b[6], b[7]);
  var pressure_hPa = u16(b[8], b[9]) / 10;

  return {
    data: {
      field1: temperature_c,
      field2: humidity_percent,
      field3: pm25_ugm3,
      field4: pm10_ugm3,
      field5: pressure_hPa,

      temperature_c: temperature_c,
      humidity_percent: humidity_percent,
      pm25_ugm3: pm25_ugm3,
      pm10_ugm3: pm10_ugm3,
      pressure_hPa: pressure_hPa
    }
  };
}
