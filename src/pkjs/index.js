// Phone side: fetches the station readings and passes the AQI to show to the watch.
var aqi = require('./aqi');
var station = require('./station');

var STATION_URL_PREFIX = 'https://api.airgradient.com/public/api/v1/world/locations/';
var STATION_URL_SUFFIX = '/measures/current';
var REQUEST_TIMEOUT_MS = 15000;
var ERROR_NETWORK = 1;

function send(message) {
  Pebble.sendAppMessage(message, null, function (e) {
    console.log('Could not deliver message to the watch: ' + JSON.stringify(e));
  });
}

function failure(code, reason) {
  return { failed: true, code: code, reason: reason };
}

function parseResponse(text) {
  try {
    return { failed: false, reading: aqi.parseReading(text) };
  } catch (e) {
    if (e instanceof aqi.DataError) {
      return failure(e.code, e.message);
    }
    return failure(aqi.ERROR_BAD_DATA, String(e));
  }
}

function fetchStation(locationId, onResult) {
  var request = new XMLHttpRequest();
  request.timeout = REQUEST_TIMEOUT_MS;
  request.onload = function () {
    if (request.status === 200) {
      onResult(parseResponse(request.responseText));
    } else {
      onResult(failure(ERROR_NETWORK, 'HTTP ' + request.status));
    }
  };
  request.onerror = function () {
    onResult(failure(ERROR_NETWORK, 'Request error'));
  };
  request.ontimeout = function () {
    onResult(failure(ERROR_NETWORK, 'Request timeout'));
  };
  request.open('GET', STATION_URL_PREFIX + locationId + STATION_URL_SUFFIX);
  request.send();
}

function deliver(home, fallback, comparison) {
  if (home.failed) {
    console.log('Reading failed: ' + home.reason);
    send({ ErrorCode: home.code });
    return;
  }
  if (fallback.failed || comparison.failed) {
    // Without both references the home station cannot be judged, so it stays on screen.
    console.log('Comparison unavailable: ' + (fallback.reason || comparison.reason));
    send(home.reading);
    return;
  }
  if (aqi.isOutlier(home.reading.Aqi, [fallback.reading.Aqi, comparison.reading.Aqi])) {
    console.log('Home AQI ' + home.reading.Aqi + ' is an outlier against ' +
      fallback.reading.Aqi + ' and ' + comparison.reading.Aqi);
    send({
      Aqi: fallback.reading.Aqi,
      MeasuredAt: fallback.reading.MeasuredAt,
      StationLabel: station.fallback.label
    });
    return;
  }
  send(home.reading);
}

function refresh() {
  var results = {};
  var names = ['home', 'fallback', 'comparison'];
  var remaining = names.length;
  names.forEach(function (name) {
    fetchStation(station[name].locationId, function (result) {
      results[name] = result;
      remaining--;
      if (remaining === 0) {
        deliver(results.home, results.fallback, results.comparison);
      }
    });
  });
}

// The watch decides when to fetch; it can only ask once this side is running.
Pebble.addEventListener('ready', function () {
  send({ JsReady: 1 });
});

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload.RequestRefresh !== undefined) {
    refresh();
  }
});
