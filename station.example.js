// Copy to src/pkjs/station.js and set the AirGradient locationIds of your stations.
module.exports = {
  // The station normally shown.
  home: { locationId: 0 },
  // Shown instead, with this label, when home is far off from both other stations.
  fallback: { locationId: 0, label: 'Nearby station' },
  // Only used for the comparison.
  comparison: { locationId: 0 }
};
