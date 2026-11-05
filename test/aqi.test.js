// Run with: node --test test/aqi.test.js
const test = require('node:test');
const assert = require('node:assert');
const { usAqi, correctedPm, parseReading, ERROR_STATION_OFFLINE, ERROR_BAD_DATA } = require('../src/pkjs/aqi.js');

const TIMESTAMP = '2026-11-05T07:08:52.000Z';
const TIMESTAMP_SECONDS = 1793862532;

test('usAqi matches the values seen on IQAir and the current reading', () => {
  assert.strictEqual(usAqi(19.6), 71);
  assert.strictEqual(usAqi(26.9), 84);
});

test('usAqi follows the EPA breakpoints at every boundary', () => {
  const cases = [
    [0, 0], [9, 50], [9.1, 51], [35.4, 100], [35.5, 101], [55.4, 150],
    [55.5, 151], [125.4, 200], [125.5, 201], [225.4, 300], [225.5, 301], [325.4, 500],
  ];
  for (const [pm, expected] of cases) {
    assert.strictEqual(usAqi(pm), expected, `PM2.5 ${pm}`);
  }
});

test('usAqi truncates to one decimal, so no value falls between breakpoints', () => {
  assert.strictEqual(usAqi(9.05), 50);
  assert.strictEqual(usAqi(35.49), 100);
});

test('usAqi caps at the top of the scale', () => {
  assert.strictEqual(usAqi(325.5), 500);
  assert.strictEqual(usAqi(1000), 500);
});

test('usAqi rejects values that are not a valid concentration', () => {
  for (const bad of [-1, null, undefined, '26.9', NaN, Infinity]) {
    assert.throws(() => usAqi(bad), (e) => e.code === ERROR_BAD_DATA, `value ${bad}`);
  }
});

function assertClose(actual, expected, tolerance, label) {
  assert.ok(Math.abs(actual - expected) <= tolerance, `${label}: got ${actual}, expected ${expected}`);
}

test('correctedPm applies each part of the EPA formula', () => {
  assertClose(correctedPm(26.9, 42), 16.2, 0.05, 'low');
  assertClose(correctedPm(41.7, 41), 30.46, 0.05, 'low to mid transition');
  assertClose(correctedPm(100, 42), 80.73, 0.05, 'mid');
  assertClose(correctedPm(235, 42), 200.39, 0.05, 'mid to high transition');
  assertClose(correctedPm(300, 42), 289.52, 0.05, 'high');
});

test('correctedPm matches the reference ranges published by EPA', () => {
  const cases = [
    [30, 13, 21], [50, 36, 45], [210, 162, 171],
  ];
  for (const [raw, atFullHumidity, atZeroHumidity] of cases) {
    assertClose(correctedPm(raw, 100), atFullHumidity, 0.5, `raw ${raw} at 100% RH`);
    assertClose(correctedPm(raw, 0), atZeroHumidity, 0.5, `raw ${raw} at 0% RH`);
  }
  assertClose(correctedPm(260, 50), 242, 0.5, 'raw 260');
});

test('correctedPm has no jumps at the breakpoints', () => {
  for (const breakpoint of [30, 50, 210, 260]) {
    for (const humidity of [0, 42, 100]) {
      assertClose(correctedPm(breakpoint - 1e-6, humidity), correctedPm(breakpoint, humidity), 0.001, `${breakpoint} at ${humidity}%`);
    }
  }
});

test('correctedPm never goes below zero', () => {
  assert.strictEqual(correctedPm(0, 100), 0);
});

test('correctedPm rejects invalid input', () => {
  for (const [pm, humidity] of [[null, 42], [-1, 42], [26.9, null], [26.9, undefined], [26.9, -1], [26.9, 101], [26.9, '42']]) {
    assert.throws(() => correctedPm(pm, humidity), (e) => e.code === ERROR_BAD_DATA, `${pm}, ${humidity}`);
  }
});

test('parseReading sends the corrected AQI, matching IQAir for the observed sample', () => {
  const reading = parseReading(JSON.stringify({ offline: false, pm02: 26.9, rhum: 42, timestamp: TIMESTAMP }));
  assert.deepStrictEqual(reading, { Aqi: 64, MeasuredAt: TIMESTAMP_SECONDS });
});

test('parseReading reports an offline station instead of a stale number', () => {
  const text = JSON.stringify({ offline: true, pm02: 26.9, rhum: 42, timestamp: TIMESTAMP });
  assert.throws(() => parseReading(text), (e) => e.code === ERROR_STATION_OFFLINE);
});

test('parseReading rejects malformed responses instead of defaulting to zero', () => {
  const bad = [
    'not json',
    'null',
    JSON.stringify({ offline: false, pm02: null, rhum: 42, timestamp: TIMESTAMP }),
    JSON.stringify({ offline: false, rhum: 42, timestamp: TIMESTAMP }),
    JSON.stringify({ offline: false, pm02: 26.9, timestamp: TIMESTAMP }),
    JSON.stringify({ offline: false, pm02: 26.9, rhum: null, timestamp: TIMESTAMP }),
    JSON.stringify({ offline: false, pm02: 26.9, rhum: 42 }),
    JSON.stringify({ offline: false, pm02: 26.9, rhum: 42, timestamp: 'yesterday' }),
  ];
  for (const text of bad) {
    assert.throws(() => parseReading(text), (e) => e.code === ERROR_BAD_DATA, text);
  }
});
