--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Returns the provided nanosecond duration, which is the default
-- representation of time durations in trace processor. Provided for
-- consistency with other functions.
CREATE PERFETTO FUNCTION time_from_ns(
  -- Time duration in nanoseconds.
  nanos INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $nanos;

-- Converts a duration in microseconds to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION time_from_us(
  -- Time duration in microseconds.
  micros INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $micros * 1000;

-- Converts a duration in millseconds to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION time_from_ms(
  -- Time duration in milliseconds.
  millis INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $millis * 1000 * 1000;

-- Converts a duration in seconds to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION time_from_s(
  -- Time duration in seconds.
  seconds INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $seconds * 1000 * 1000 * 1000;

-- Converts a duration in minutes to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION time_from_min(
  -- Time duration in minutes.
  minutes INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $minutes * 60 * 1000 * 1000 * 1000;

-- Converts a duration in hours to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION time_from_hours(
  -- Time duration in hours.
  hours INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $hours * 60 * 60 * 1000 * 1000 * 1000;

-- Converts a duration in days to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION time_from_days(
  -- Time duration in days.
  days INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $days * 24 * 60 * 60 * 1000 * 1000 * 1000;
