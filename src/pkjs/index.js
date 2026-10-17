var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

var WEATHER_REFRESH_MS = 30 * 60 * 1000; // 30 minutes

// The temperature goes to the watch in tenths of a degree Celsius; the watch
// converts it to the unit picked in settings, so switching units needs no
// refetch.
function sendWeather(temperature, weatherCode) {
  Pebble.sendAppMessage({
    TemperatureTenths: Math.round(temperature * 10),
    WeatherCondition: weatherCode
  }, function() {}, function(e) {
    console.log('Weather AppMessage failed: ' + JSON.stringify(e));
  });
}

function fetchWeather(latitude, longitude) {
  var url = 'https://api.open-meteo.com/v1/forecast?latitude=' + latitude +
    '&longitude=' + longitude + '&current_weather=true';
  var req = new XMLHttpRequest();
  req.open('GET', url, true);
  req.timeout = 15000;
  req.onload = function() {
    if (req.status === 200) {
      var json = JSON.parse(req.responseText);
      var current = json.current_weather;
      if (current) {
        sendWeather(current.temperature, current.weathercode);
      }
    } else {
      console.log('Weather request failed with status ' + req.status);
    }
  };
  req.onerror = function() {
    console.log('Weather request error');
  };
  req.ontimeout = function() {
    console.log('Weather request timed out');
  };
  req.send();
}

function updateWeather() {
  navigator.geolocation.getCurrentPosition(
    function(pos) {
      fetchWeather(pos.coords.latitude, pos.coords.longitude);
    },
    function(err) {
      console.log('Location error: ' + err.message);
    },
    { timeout: 15000, maximumAge: 60000 }
  );
}

Pebble.addEventListener('ready', function() {
  updateWeather();
  setInterval(updateWeather, WEATHER_REFRESH_MS);
});
