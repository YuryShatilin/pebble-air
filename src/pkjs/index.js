// Phone side: fetches the station reading and passes the computed AQI to the watch.
var aqi = require('./aqi');
var station = require('./station');

var STATION_URL = 'https://api.airgradient.com/public/api/v1/world/locations/' + station.locationId + '/measures/current';
var REQUEST_TIMEOUT_MS = 15000;
var ERROR_NETWORK = 1;

function send(message) {
  Pebble.sendAppMessage(message, null, function (e) {
    console.log('Could not deliver message to the watch: ' + JSON.stringify(e));
  });
}

function sendError(code, reason) {
  console.log('Reading failed: ' + reason);
  send({ ErrorCode: code });
}

function handleResponse(text) {
  try {
    send(aqi.parseReading(text));
  } catch (e) {
    if (e instanceof aqi.DataError) {
      sendError(e.code, e.message);
    } else {
      sendError(aqi.ERROR_BAD_DATA, String(e));
    }
  }
}

function fetchReading() {
  var request = new XMLHttpRequest();
  request.timeout = REQUEST_TIMEOUT_MS;
  request.onload = function () {
    if (request.status === 200) {
      handleResponse(request.responseText);
    } else {
      sendError(ERROR_NETWORK, 'HTTP ' + request.status);
    }
  };
  request.onerror = function () {
    sendError(ERROR_NETWORK, 'Request error');
  };
  request.ontimeout = function () {
    sendError(ERROR_NETWORK, 'Request timeout');
  };
  request.open('GET', STATION_URL);
  request.send();
}

Pebble.addEventListener('ready', fetchReading);

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload.RequestRefresh !== undefined) {
    fetchReading();
  }
});
