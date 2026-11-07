# Pebble Air

A Pebble Time 2 watchapp that shows the current US AQI from an air quality station.

<img src="docs/screenshot.png" alt="The app showing a US AQI value and its category" width="200">

## Choose stations

The app reads three nearby stations. It shows `home`, unless `home` differs by more than 40% from both of the others. Then it shows `fallback`, with its label as the title. If `fallback` or `comparison` is unavailable, it shows `home`.

1. Find the locations in the [list of all stations](https://api.airgradient.com/public/api/v1/world/locations/measures/current) and note their `locationId` values.
2. Copy `station.example.js` to `src/pkjs/station.js` and put the IDs and the label in it. This file is not committed, and the app does not build without it.
3. Build and install.

## Troubleshooting

| On the watch | Meaning | What to check |
|---|---|---|
| `No network` | The request failed or was not allowed | Network access for the app in the Pebble phone app, and the phone's internet connection |
| `No phone` | No reply from the phone | Bluetooth, and that the Pebble phone app is running |
| `Station offline` | The `home` station reports that it is offline | Try again later, or use another station |
| `Bad data` | The response has missing or invalid values | Open the station URL and check `pm02`, `rhum` and `timestamp` |
