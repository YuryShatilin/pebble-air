// Pure logic: PM2.5 to US AQI (EPA 2024 breakpoints) and AirGradient response parsing.
var BREAKPOINTS = [
  [0, 9, 0, 50],
  [9.1, 35.4, 51, 100],
  [35.5, 55.4, 101, 150],
  [55.5, 125.4, 151, 200],
  [125.5, 225.4, 201, 300],
  [225.5, 325.4, 301, 500]
];
var MAX_AQI = 500;
var MAX_PM = 325.4;

var ERROR_STATION_OFFLINE = 2;
var ERROR_BAD_DATA = 3;

function DataError(code, message) {
  this.code = code;
  this.message = message;
}

function usAqi(pm) {
  if (typeof pm !== 'number' || !isFinite(pm) || pm < 0) {
    throw new DataError(ERROR_BAD_DATA, 'Invalid PM2.5: ' + pm);
  }
  // The epsilon keeps values like 26.9 from truncating to 26.8 through float error.
  var c = Math.floor(pm * 10 + 1e-9) / 10;
  if (c > MAX_PM) {
    return MAX_AQI;
  }
  for (var i = 0; i < BREAKPOINTS.length; i++) {
    var b = BREAKPOINTS[i];
    if (c >= b[0] && c <= b[1]) {
      return Math.round(((b[3] - b[2]) / (b[1] - b[0])) * (c - b[0]) + b[2]);
    }
  }
  throw new DataError(ERROR_BAD_DATA, 'PM2.5 outside breakpoints: ' + pm);
}

// EPA five-part correction for the cf_atm reading of low-cost PM2.5 sensors (AirNow Fire and Smoke Map,
// October 2021). The two transition bands keep the result continuous between the three main equations.
function correctedPm(rawPm, humidity) {
  if (typeof rawPm !== 'number' || !isFinite(rawPm) || rawPm < 0) {
    throw new DataError(ERROR_BAD_DATA, 'Invalid PM2.5: ' + rawPm);
  }
  if (typeof humidity !== 'number' || !isFinite(humidity) || humidity < 0 || humidity > 100) {
    throw new DataError(ERROR_BAD_DATA, 'Invalid humidity: ' + humidity);
  }
  var x = rawPm;
  var rh = humidity;
  var w;
  var result;
  if (x < 30) {
    result = 0.524 * x - 0.0862 * rh + 5.75;
  } else if (x < 50) {
    w = x / 20 - 3 / 2;
    result = (0.786 * w + 0.524 * (1 - w)) * x - 0.0862 * rh + 5.75;
  } else if (x < 210) {
    result = 0.786 * x - 0.0862 * rh + 5.75;
  } else if (x < 260) {
    w = x / 50 - 21 / 5;
    result = (0.69 * w + 0.786 * (1 - w)) * x - 0.0862 * rh * (1 - w) +
      2.966 * w + 5.75 * (1 - w) + 8.84e-4 * x * x * w;
  } else {
    result = 2.966 + 0.69 * x + 8.84e-4 * x * x;
  }
  return Math.max(0, result);
}

function parseReading(text) {
  var data;
  try {
    data = JSON.parse(text);
  } catch (e) {
    throw new DataError(ERROR_BAD_DATA, 'Response is not JSON');
  }
  if (data === null || typeof data !== 'object') {
    throw new DataError(ERROR_BAD_DATA, 'Response is not an object');
  }
  if (data.offline === true) {
    throw new DataError(ERROR_STATION_OFFLINE, 'Station is offline');
  }
  var measuredAtMs = Date.parse(data.timestamp);
  if (isNaN(measuredAtMs)) {
    throw new DataError(ERROR_BAD_DATA, 'Invalid timestamp: ' + data.timestamp);
  }
  return {
    Aqi: usAqi(correctedPm(data.pm02, data.rhum)),
    MeasuredAt: Math.floor(measuredAtMs / 1000)
  };
}

module.exports = {
  usAqi: usAqi,
  correctedPm: correctedPm,
  parseReading: parseReading,
  DataError: DataError,
  ERROR_STATION_OFFLINE: ERROR_STATION_OFFLINE,
  ERROR_BAD_DATA: ERROR_BAD_DATA
};
