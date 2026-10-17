var HOUR_OPTIONS = [];
for (var hour = 0; hour < 24; hour++) {
  HOUR_OPTIONS.push({
    "label": (hour < 10 ? "0" : "") + hour + ":00",
    "value": String(hour)
  });
}

module.exports = [
  {
    "type": "heading",
    "defaultValue": "Casual Salaryman Settings"
  },
  {
    "type": "text",
    "defaultValue": "Retro LCD watchface"
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Time"
      },
      {
        "type": "select",
        "messageKey": "ClockFormat",
        "label": "Clock format",
        "defaultValue": "0",
        "options": [
          { "label": "Use watch setting", "value": "0" },
          { "label": "24-hour", "value": "1" },
          { "label": "12-hour", "value": "2" }
        ]
      },
      {
        "type": "toggle",
        "messageKey": "ShowSeconds",
        "label": "Show seconds",
        "defaultValue": true
      },
      {
        "type": "toggle",
        "messageKey": "HideSecondsAtNight",
        "label": "Hide seconds at night",
        "description": "Updates once a minute instead of every second during these hours, to save battery.",
        "defaultValue": false
      },
      {
        "type": "select",
        "messageKey": "NightStartHour",
        "label": "Night starts",
        "defaultValue": "22",
        "options": HOUR_OPTIONS
      },
      {
        "type": "select",
        "messageKey": "NightEndHour",
        "label": "Night ends",
        "defaultValue": "7",
        "options": HOUR_OPTIONS
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Date"
      },
      {
        "type": "select",
        "messageKey": "DateFormat",
        "label": "Date format",
        "defaultValue": "0",
        "options": [
          { "label": "DD-MM (31-12)", "value": "0" },
          { "label": "MM-DD (12-31)", "value": "1" }
        ]
      },
      {
        "type": "select",
        "messageKey": "FirstDayOfWeek",
        "label": "First day of week",
        "defaultValue": "0",
        "options": [
          { "label": "Monday", "value": "0" },
          { "label": "Sunday", "value": "1" }
        ]
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Weather"
      },
      {
        "type": "select",
        "messageKey": "TemperatureUnit",
        "label": "Temperature unit",
        "defaultValue": "0",
        "options": [
          { "label": "Celsius (°C)", "value": "0" },
          { "label": "Fahrenheit (°F)", "value": "1" }
        ]
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];
