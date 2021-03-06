/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "calibration/gyroscope/gyro_cal.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "calibration/util/cal_log.h"
#include "common/math/vec.h"

/////// DEFINITIONS AND MACROS ///////////////////////////////////////

// Maximum gyro bias correction (should be set based on expected max bias
// of the given sensor).
#define MAX_GYRO_BIAS (0.1f)  // [rad/sec]

// Converts units of radians to milli-degrees.
#define RAD_TO_MILLI_DEGREES (float)(1e3f * 180.0f / NANO_PI)

#ifdef GYRO_CAL_DBG_ENABLED
// The time value used to throttle debug messaging.
#define GYROCAL_WAIT_TIME_NANOS (300000000)

// Unit conversion: nanoseconds to seconds.
#define NANOS_TO_SEC (1.0e-9f)

// A debug version label to help with tracking results.
#define GYROCAL_DEBUG_VERSION_STRING "[Jan 20, 2017]"

// Debug log tag string used to identify debug report output data.
#define GYROCAL_REPORT_TAG "[GYRO_CAL:REPORT]"

// Debug log tag string used to identify debug tuning output data.
#define GYROCAL_TUNE_TAG "[GYRO_CAL:TUNE]"
#endif  // GYRO_CAL_DBG_ENABLED

/////// FORWARD DECLARATIONS /////////////////////////////////////////

static void deviceStillnessCheck(struct GyroCal* gyro_cal,
                                 uint64_t sample_time_nanos);

static void computeGyroCal(struct GyroCal* gyro_cal,
                           uint64_t calibration_time_nanos);

static void checkWatchdog(struct GyroCal* gyro_cal, uint64_t sample_time_nanos);

// Data tracker command enumeration.
enum GyroCalTrackerCommand {
  DO_RESET = 0,    // Resets the local data used for data tracking.
  DO_UPDATE_DATA,  // Updates the local tracking data.
  DO_STORE_DATA,   // Stores intermediate results for later recall.
  DO_EVALUATE      // Computes and provides the results of the gate function.
};

/*
 * Updates the temperature min/max and mean during the stillness period. Returns
 * 'true' if the min and max temperature values exceed the range set by
 * 'temperature_delta_limit_celsius'.
 *
 * INPUTS:
 *   gyro_cal:     Pointer to the GyroCal data structure.
 *   temperature_celsius:  New temperature sample to include.
 *   do_this:      Command enumerator that controls function behavior:
 */
static bool gyroTemperatureStatsTracker(struct GyroCal* gyro_cal,
                                        float temperature_celsius,
                                        enum GyroCalTrackerCommand do_this);

/*
 * Tracks the minimum and maximum gyroscope stillness window means.
 * Returns 'true' when the difference between gyroscope min and max window
 * means are outside the range set by 'stillness_mean_delta_limit'.
 *
 * INPUTS:
 *   gyro_cal:     Pointer to the GyroCal data structure.
 *   do_this:      Command enumerator that controls function behavior.
 */
static bool gyroStillMeanTracker(struct GyroCal* gyro_cal,
                                 enum GyroCalTrackerCommand do_this);

#ifdef GYRO_CAL_DBG_ENABLED
// Defines the type of debug data to print.
enum DebugPrintData {
  OFFSET = 0,
  STILLNESS_DATA,
  SAMPLE_RATE_AND_TEMPERATURE,
  GYRO_MINMAX_STILLNESS_MEAN,
  ACCEL_STATS,
  GYRO_STATS,
  MAG_STATS,
  ACCEL_STATS_TUNING,
  GYRO_STATS_TUNING,
  MAG_STATS_TUNING
};

/*
 * Updates running calculation of the gyro's mean sampling rate.
 *
 * Behavior:
 *   1)  If 'debug_mean_sampling_rate_hz' pointer is not NULL then the local
 *       calculation of the sampling rate is copied, and the function returns.
 *   2)  Else, if 'reset_stats' is 'true' then the local estimate is reset and
 *       the function returns.
 *   3)  Otherwise, the local estimate of the mean sampling rates is updated.
 *
 * INPUTS:
 *   debug_mean_sampling_rate_hz:   Pointer to the mean sampling rate to update.
 *   timestamp_nanos:  Time stamp (nanoseconds).
 *   reset_stats:  Flag that signals a reset of the sampling rate estimate.
 */
static void gyroSamplingRateUpdate(float* debug_mean_sampling_rate_hz,
                                   uint64_t timestamp_nanos, bool reset_stats);

// Updates the information used for debug printouts.
static void gyroCalUpdateDebug(struct GyroCal* gyro_cal);

// Helper function for printing out common debug data.
static void gyroCalDebugPrintData(const struct GyroCal* gyro_cal,
                                  char* debug_tag,
                                  enum DebugPrintData print_data);

// This conversion function is necessary for Nanohub firmware compilation (i.e.,
// can't cast a uint64_t to a float directly). This conversion function was
// copied from: /third_party/contexthub/firmware/src/floatRt.c
static float floatFromUint64(uint64_t v)
{
    uint32_t hi = v >> 32, lo = v;

    if (!hi) //this is very fast for cases where we fit into a uint32_t
        return(float)lo;
    else {
        return ((float)hi) * 4294967296.0f + (float)lo;
    }
}

#ifdef GYRO_CAL_DBG_TUNE_ENABLED
// Prints debug information useful for tuning the GyroCal parameters.
static void gyroCalTuneDebugPrint(const struct GyroCal* gyro_cal,
                                  uint64_t timestamp_nanos);
#endif  // GYRO_CAL_DBG_TUNE_ENABLED
#endif  // GYRO_CAL_DBG_ENABLED

/////// FUNCTION DEFINITIONS /////////////////////////////////////////

// Initialize the gyro calibration data structure.
void gyroCalInit(struct GyroCal* gyro_cal, uint64_t min_still_duration_nanos,
                 uint64_t max_still_duration_nanos, float bias_x, float bias_y,
                 float bias_z, uint64_t calibration_time_nanos,
                 uint64_t window_time_duration_nanos, float gyro_var_threshold,
                 float gyro_confidence_delta, float accel_var_threshold,
                 float accel_confidence_delta, float mag_var_threshold,
                 float mag_confidence_delta, float stillness_threshold,
                 float stillness_mean_delta_limit,
                 float temperature_delta_limit_celsius,
                 bool gyro_calibration_enable) {
  // Clear gyro_cal structure memory.
  memset(gyro_cal, 0, sizeof(struct GyroCal));

  // Initialize the stillness detectors.
  // Gyro parameter input units are [rad/sec].
  // Accel parameter input units are [m/sec^2].
  // Magnetometer parameter input units are [uT].
  gyroStillDetInit(&gyro_cal->gyro_stillness_detect, gyro_var_threshold,
                   gyro_confidence_delta);
  gyroStillDetInit(&gyro_cal->accel_stillness_detect, accel_var_threshold,
                   accel_confidence_delta);
  gyroStillDetInit(&gyro_cal->mag_stillness_detect, mag_var_threshold,
                   mag_confidence_delta);

  // Reset stillness flag and start timestamp.
  gyro_cal->prev_still = false;
  gyro_cal->start_still_time_nanos = 0;

  // Set the min and max window stillness duration.
  gyro_cal->min_still_duration_nanos = min_still_duration_nanos;
  gyro_cal->max_still_duration_nanos = max_still_duration_nanos;

  // Sets the duration of the stillness processing windows.
  gyro_cal->window_time_duration_nanos = window_time_duration_nanos;

  // Set the watchdog timeout duration.
  gyro_cal->gyro_watchdog_timeout_duration_nanos =
      2 * window_time_duration_nanos;

  // Load the last valid cal from system memory.
  gyro_cal->bias_x = bias_x;  // [rad/sec]
  gyro_cal->bias_y = bias_y;  // [rad/sec]
  gyro_cal->bias_z = bias_z;  // [rad/sec]
  gyro_cal->calibration_time_nanos = calibration_time_nanos;

  // Set the stillness threshold required for gyro bias calibration.
  gyro_cal->stillness_threshold = stillness_threshold;

  // Current window end-time used to assist in keeping sensor data collection in
  // sync. Setting this to zero signals that sensor data will be dropped until a
  // valid end-time is set from the first gyro timestamp received.
  gyro_cal->stillness_win_endtime_nanos = 0;

  // Gyro calibrations will be applied (see, gyroCalRemoveBias()).
  gyro_cal->gyro_calibration_enable = (gyro_calibration_enable > 0);

  // Sets the stability limit for the stillness window mean acceptable delta.
  gyro_cal->stillness_mean_delta_limit = stillness_mean_delta_limit;

  // Sets the min/max temperature delta limit for the stillness period.
  gyro_cal->temperature_delta_limit_celsius = temperature_delta_limit_celsius;

  // Ensures that the data tracking functionality is reset.
  gyroStillMeanTracker(gyro_cal, DO_RESET);
  gyroTemperatureStatsTracker(gyro_cal, 0.0f, DO_RESET);

#ifdef GYRO_CAL_DBG_ENABLED
  CAL_DEBUG_LOG("[GYRO_CAL:MEMORY]", "sizeof(struct GyroCal): %lu",
                (unsigned long int)sizeof(struct GyroCal));

  if (gyro_cal->gyro_calibration_enable) {
    CAL_DEBUG_LOG("[GYRO_CAL:INIT]", "Online gyroscope calibration ENABLED.");
  } else {
    CAL_DEBUG_LOG("[GYRO_CAL:INIT]", "Online gyroscope calibration DISABLED.");
  }

  // Ensures that the gyro sampling rate estimate is reset.
  gyroSamplingRateUpdate(NULL, 0, /*reset_stats=*/true);
#endif  // GYRO_CAL_DBG_ENABLED
}

// Void pointer in the gyro calibration data structure (doesn't do anything
// except prevent compiler warnings).
void gyroCalDestroy(struct GyroCal* gyro_cal) {
  (void)gyro_cal;
}

// Get the most recent bias calibration value.
void gyroCalGetBias(struct GyroCal* gyro_cal, float* bias_x, float* bias_y,
                    float* bias_z, float* temperature_celsius) {
  *bias_x = gyro_cal->bias_x;
  *bias_y = gyro_cal->bias_y;
  *bias_z = gyro_cal->bias_z;
  *temperature_celsius = gyro_cal->bias_temperature_celsius;
}

// Set an initial bias calibration value.
void gyroCalSetBias(struct GyroCal* gyro_cal, float bias_x, float bias_y,
                    float bias_z, uint64_t calibration_time_nanos) {
  gyro_cal->bias_x = bias_x;
  gyro_cal->bias_y = bias_y;
  gyro_cal->bias_z = bias_z;
  gyro_cal->calibration_time_nanos = calibration_time_nanos;

#ifdef GYRO_CAL_DBG_ENABLED
  CAL_DEBUG_LOG("[GYRO_CAL:RECALL]",
                "Gyro Bias Calibration [mdps]: %s%d.%06d, %s%d.%06d, %s%d.%06d",
                CAL_ENCODE_FLOAT(gyro_cal->bias_x * RAD_TO_MILLI_DEGREES, 6),
                CAL_ENCODE_FLOAT(gyro_cal->bias_y * RAD_TO_MILLI_DEGREES, 6),
                CAL_ENCODE_FLOAT(gyro_cal->bias_z * RAD_TO_MILLI_DEGREES, 6));
#endif  // GYRO_CAL_DBG_ENABLED
}

// Remove bias from a gyro measurement [rad/sec].
void gyroCalRemoveBias(struct GyroCal* gyro_cal, float xi, float yi, float zi,
                       float* xo, float* yo, float* zo) {
  if (gyro_cal->gyro_calibration_enable) {
    *xo = xi - gyro_cal->bias_x;
    *yo = yi - gyro_cal->bias_y;
    *zo = zi - gyro_cal->bias_z;
  }
}

// Returns true when a new gyro calibration is available.
bool gyroCalNewBiasAvailable(struct GyroCal* gyro_cal) {
  bool new_gyro_cal_available =
      (gyro_cal->gyro_calibration_enable && gyro_cal->new_gyro_cal_available);

  // Clear the flag.
  gyro_cal->new_gyro_cal_available = false;

  return new_gyro_cal_available;
}

// Update the gyro calibration with gyro data [rad/sec].
void gyroCalUpdateGyro(struct GyroCal* gyro_cal, uint64_t sample_time_nanos,
                       float x, float y, float z, float temperature_celsius) {
  static float latest_temperature_celsius = 0.0f;

  // Make sure that a valid window end-time is set, and start the watchdog
  // timer.
  if (gyro_cal->stillness_win_endtime_nanos <= 0) {
    gyro_cal->stillness_win_endtime_nanos =
        sample_time_nanos + gyro_cal->window_time_duration_nanos;

    // Start the watchdog timer.
    gyro_cal->gyro_watchdog_start_nanos = sample_time_nanos;
  }

  // Update the temperature statistics (only on a temperature change).
  if (NANO_ABS(temperature_celsius - latest_temperature_celsius) > FLT_MIN) {
    gyroTemperatureStatsTracker(gyro_cal, temperature_celsius, DO_UPDATE_DATA);
  }

#ifdef GYRO_CAL_DBG_ENABLED
  // Update the gyro sampling rate estimate.
  gyroSamplingRateUpdate(NULL, sample_time_nanos, /*reset_stats=*/false);
#endif  // GYRO_CAL_DBG_ENABLED

  // Pass gyro data to stillness detector
  gyroStillDetUpdate(&gyro_cal->gyro_stillness_detect,
                     gyro_cal->stillness_win_endtime_nanos, sample_time_nanos,
                     x, y, z);

  // Perform a device stillness check, set next window end-time, and
  // possibly do a gyro bias calibration and stillness detector reset.
  deviceStillnessCheck(gyro_cal, sample_time_nanos);
}

// Update the gyro calibration with mag data [micro Tesla].
void gyroCalUpdateMag(struct GyroCal* gyro_cal, uint64_t sample_time_nanos,
                      float x, float y, float z) {
  // Pass magnetometer data to stillness detector.
  gyroStillDetUpdate(&gyro_cal->mag_stillness_detect,
                     gyro_cal->stillness_win_endtime_nanos, sample_time_nanos,
                     x, y, z);

  // Received a magnetometer sample; incorporate it into detection.
  gyro_cal->using_mag_sensor = true;

  // Perform a device stillness check, set next window end-time, and
  // possibly do a gyro bias calibration and stillness detector reset.
  deviceStillnessCheck(gyro_cal, sample_time_nanos);
}

// Update the gyro calibration with accel data [m/sec^2].
void gyroCalUpdateAccel(struct GyroCal* gyro_cal, uint64_t sample_time_nanos,
                        float x, float y, float z) {
  // Pass accelerometer data to stillnesss detector.
  gyroStillDetUpdate(&gyro_cal->accel_stillness_detect,
                     gyro_cal->stillness_win_endtime_nanos, sample_time_nanos,
                     x, y, z);

  // Perform a device stillness check, set next window end-time, and
  // possibly do a gyro bias calibration and stillness detector reset.
  deviceStillnessCheck(gyro_cal, sample_time_nanos);
}

// TODO(davejacobs): Consider breaking this function up to improve readability.
// Checks the state of all stillness detectors to determine
// whether the device is "still".
void deviceStillnessCheck(struct GyroCal* gyro_cal,
                          uint64_t sample_time_nanos) {
  bool stillness_duration_exceeded = false;
  bool stillness_duration_too_short = false;
  bool min_max_temp_exceeded = false;
  bool mean_not_stable = false;
  bool device_is_still = false;
  float conf_not_rot = 0;
  float conf_not_accel = 0;
  float conf_still = 0;

  // Check the watchdog timer.
  checkWatchdog(gyro_cal, sample_time_nanos);

  // Is there enough data to do a stillness calculation?
  if ((!gyro_cal->mag_stillness_detect.stillness_window_ready &&
       gyro_cal->using_mag_sensor) ||
      !gyro_cal->accel_stillness_detect.stillness_window_ready ||
      !gyro_cal->gyro_stillness_detect.stillness_window_ready) {
    return;  // Not yet, wait for more data.
  }

  // Set the next window end-time for the stillness detectors.
  gyro_cal->stillness_win_endtime_nanos =
      sample_time_nanos + gyro_cal->window_time_duration_nanos;

  // Update the confidence scores for all sensors.
  gyroStillDetCompute(&gyro_cal->accel_stillness_detect);
  gyroStillDetCompute(&gyro_cal->gyro_stillness_detect);
  if (gyro_cal->using_mag_sensor) {
    gyroStillDetCompute(&gyro_cal->mag_stillness_detect);
  } else {
    // Not using magnetometer, force stillness confidence to 100%.
    gyro_cal->mag_stillness_detect.stillness_confidence = 1.0f;
  }

  // Updates the mean tracker data.
  gyroStillMeanTracker(gyro_cal, DO_UPDATE_DATA);

  // Determine motion confidence scores (rotation, accelerating, and stillness).
  conf_not_rot = gyro_cal->gyro_stillness_detect.stillness_confidence *
                 gyro_cal->mag_stillness_detect.stillness_confidence;
  conf_not_accel = gyro_cal->accel_stillness_detect.stillness_confidence;
  conf_still = conf_not_rot * conf_not_accel;

  // Evaluate the mean and temperature gate functions.
  mean_not_stable = gyroStillMeanTracker(gyro_cal, DO_EVALUATE);
  min_max_temp_exceeded =
      gyroTemperatureStatsTracker(gyro_cal, 0.0f, DO_EVALUATE);

  // Determines if the device is currently still.
  device_is_still = (conf_still > gyro_cal->stillness_threshold) &&
      !mean_not_stable && !min_max_temp_exceeded ;

  if (device_is_still) {
    // Device is "still" logic:
    // If not previously still, then record the start time.
    // If stillness period is too long, then do a calibration.
    // Otherwise, continue collecting stillness data.

    // If device was not previously still, set new start timestamp.
    if (!gyro_cal->prev_still) {
      // Record the starting timestamp of the current stillness window.
      // This enables the calculation of total duration of the stillness period.
      gyro_cal->start_still_time_nanos =
          gyro_cal->gyro_stillness_detect.window_start_time;
    }

    // Check to see if current stillness period exceeds the desired limit.
    stillness_duration_exceeded =
        ((gyro_cal->gyro_stillness_detect.last_sample_time -
          gyro_cal->start_still_time_nanos) >
         gyro_cal->max_still_duration_nanos);

    // Track the new stillness mean and temperature data.
    gyroStillMeanTracker(gyro_cal, DO_STORE_DATA);
    gyroTemperatureStatsTracker(gyro_cal, 0.0f, DO_STORE_DATA);

    if (stillness_duration_exceeded) {
      // The current stillness has gone too long. Do a calibration with the
      // current data and reset.

      // Updates the gyro bias estimate with the current window data and
      // resets the stats.
      gyroStillDetReset(&gyro_cal->accel_stillness_detect,
                        /*reset_stats=*/true);
      gyroStillDetReset(&gyro_cal->gyro_stillness_detect, /*reset_stats=*/true);
      gyroStillDetReset(&gyro_cal->mag_stillness_detect, /*reset_stats=*/true);

      // Resets the local calculations because the stillness period is over.
      gyroStillMeanTracker(gyro_cal, DO_RESET);
      gyroTemperatureStatsTracker(gyro_cal, 0.0f, DO_RESET);

      // Computes a new gyro offset estimate.
      computeGyroCal(gyro_cal,
                     gyro_cal->gyro_stillness_detect.last_sample_time);

#ifdef GYRO_CAL_DBG_ENABLED
      // Resets the sampling rate estimate.
      gyroSamplingRateUpdate(NULL, sample_time_nanos, /*reset_stats=*/true);
#endif  // GYRO_CAL_DBG_ENABLED

      // Update stillness flag. Force the start of a new stillness period.
      gyro_cal->prev_still = false;
    } else {
      // Continue collecting stillness data.

      // Extend the stillness period.
      gyroStillDetReset(&gyro_cal->accel_stillness_detect,
                        /*reset_stats=*/false);
      gyroStillDetReset(&gyro_cal->gyro_stillness_detect,
                        /*reset_stats=*/false);
      gyroStillDetReset(&gyro_cal->mag_stillness_detect, /*reset_stats=*/false);

      // Update the stillness flag.
      gyro_cal->prev_still = true;
    }
  } else {
    // Device is NOT still; motion detected.

    // If device was previously still and the total stillness duration is not
    // "too short", then do a calibration with the data accumulated thus far.
    stillness_duration_too_short =
        ((gyro_cal->gyro_stillness_detect.window_start_time -
          gyro_cal->start_still_time_nanos) <
         gyro_cal->min_still_duration_nanos);

    if (gyro_cal->prev_still && !stillness_duration_too_short) {
      computeGyroCal(gyro_cal,
                     gyro_cal->gyro_stillness_detect.window_start_time);
    }

    // Reset the stillness detectors and the stats.
    gyroStillDetReset(&gyro_cal->accel_stillness_detect, /*reset_stats=*/true);
    gyroStillDetReset(&gyro_cal->gyro_stillness_detect, /*reset_stats=*/true);
    gyroStillDetReset(&gyro_cal->mag_stillness_detect, /*reset_stats=*/true);

    // Resets the temperature and sensor mean data.
    gyroTemperatureStatsTracker(gyro_cal, 0.0f, DO_RESET);
    gyroStillMeanTracker(gyro_cal, DO_RESET);

#ifdef GYRO_CAL_DBG_ENABLED
    // Resets the sampling rate estimate.
    gyroSamplingRateUpdate(NULL, sample_time_nanos, /*reset_stats=*/true);
#endif  // GYRO_CAL_DBG_ENABLED

    // Update stillness flag.
    gyro_cal->prev_still = false;
  }

  // Reset the watchdog timer after we have processed data.
  gyro_cal->gyro_watchdog_start_nanos = sample_time_nanos;
}

// Calculates a new gyro bias offset calibration value.
void computeGyroCal(struct GyroCal* gyro_cal, uint64_t calibration_time_nanos) {
  // Check to see if new calibration values is within acceptable range.
  if (!(gyro_cal->gyro_stillness_detect.prev_mean_x < MAX_GYRO_BIAS &&
        gyro_cal->gyro_stillness_detect.prev_mean_x > -MAX_GYRO_BIAS &&
        gyro_cal->gyro_stillness_detect.prev_mean_y < MAX_GYRO_BIAS &&
        gyro_cal->gyro_stillness_detect.prev_mean_y > -MAX_GYRO_BIAS &&
        gyro_cal->gyro_stillness_detect.prev_mean_z < MAX_GYRO_BIAS &&
        gyro_cal->gyro_stillness_detect.prev_mean_z > -MAX_GYRO_BIAS)) {
#ifdef GYRO_CAL_DBG_ENABLED
    CAL_DEBUG_LOG("[GYRO_CAL:REJECT]",
                  "Offset|Temp|Time [mdps|C|nsec]: %s%d.%06d, %s%d.%06d, "
                  "%s%d.%06d, %s%d.%06d, %llu",
                  CAL_ENCODE_FLOAT(gyro_cal->gyro_stillness_detect.prev_mean_x *
                                       RAD_TO_MILLI_DEGREES,
                                   6),
                  CAL_ENCODE_FLOAT(gyro_cal->gyro_stillness_detect.prev_mean_y *
                                       RAD_TO_MILLI_DEGREES,
                                   6),
                  CAL_ENCODE_FLOAT(gyro_cal->gyro_stillness_detect.prev_mean_z *
                                       RAD_TO_MILLI_DEGREES,
                                   6),
                  CAL_ENCODE_FLOAT(gyro_cal->temperature_mean_celsius, 6),
                  (unsigned long long int)calibration_time_nanos);
#endif  // GYRO_CAL_DBG_ENABLED

    // Outside of range. Ignore, reset, and continue.
    return;
  }

  // Record the new gyro bias offset calibration.
  gyro_cal->bias_x = gyro_cal->gyro_stillness_detect.prev_mean_x;
  gyro_cal->bias_y = gyro_cal->gyro_stillness_detect.prev_mean_y;
  gyro_cal->bias_z = gyro_cal->gyro_stillness_detect.prev_mean_z;

  // Store the calibration temperature (using the mean temperature over the
  // "stillness" period).
  gyro_cal->bias_temperature_celsius = gyro_cal->temperature_mean_celsius;

  // Store the calibration time stamp.
  gyro_cal->calibration_time_nanos = calibration_time_nanos;

  // Record the final stillness confidence.
  gyro_cal->stillness_confidence =
      gyro_cal->gyro_stillness_detect.prev_stillness_confidence *
      gyro_cal->accel_stillness_detect.prev_stillness_confidence *
      gyro_cal->mag_stillness_detect.prev_stillness_confidence;

  // Set flag to indicate a new gyro calibration value is available.
  gyro_cal->new_gyro_cal_available = true;

#ifdef GYRO_CAL_DBG_ENABLED
  // Increment the total count of calibration updates.
  gyro_cal->debug_calibration_count++;

  // Update the calibration debug information and trigger a printout.
  gyroCalUpdateDebug(gyro_cal);
#endif
}

// Check for a watchdog timeout condition.
void checkWatchdog(struct GyroCal* gyro_cal, uint64_t sample_time_nanos) {
  bool watchdog_timeout;

  // Check for initialization of the watchdog time (=0).
  if (gyro_cal->gyro_watchdog_start_nanos <= 0) {
    return;
  }

  // Check for the watchdog timeout condition (i.e., the time elapsed since the
  // last received sample has exceeded the allowed watchdog duration).
  watchdog_timeout =
      (sample_time_nanos > gyro_cal->gyro_watchdog_timeout_duration_nanos +
                               gyro_cal->gyro_watchdog_start_nanos);

  // If a timeout occurred then reset to known good state.
  if (watchdog_timeout) {
    // Reset stillness detectors and restart data capture.
    gyroStillDetReset(&gyro_cal->accel_stillness_detect, /*reset_stats=*/true);
    gyroStillDetReset(&gyro_cal->gyro_stillness_detect, /*reset_stats=*/true);
    gyroStillDetReset(&gyro_cal->mag_stillness_detect, /*reset_stats=*/true);

    // Resets the temperature and sensor mean data.
    gyroTemperatureStatsTracker(gyro_cal, 0.0f, DO_RESET);
    gyroStillMeanTracker(gyro_cal, DO_RESET);

#ifdef GYRO_CAL_DBG_ENABLED
    // Resets the sampling rate estimate.
    gyroSamplingRateUpdate(NULL, sample_time_nanos, /*reset_stats=*/true);
#endif  // GYRO_CAL_DBG_ENABLED

    // Resets the stillness window end-time.
    gyro_cal->stillness_win_endtime_nanos = 0;

    // Force stillness confidence to zero.
    gyro_cal->accel_stillness_detect.prev_stillness_confidence = 0;
    gyro_cal->gyro_stillness_detect.prev_stillness_confidence = 0;
    gyro_cal->mag_stillness_detect.prev_stillness_confidence = 0;
    gyro_cal->stillness_confidence = 0;
    gyro_cal->prev_still = false;

    // If there are no magnetometer samples being received then
    // operate the calibration algorithm without this sensor.
    if (!gyro_cal->mag_stillness_detect.stillness_window_ready &&
        gyro_cal->using_mag_sensor) {
      gyro_cal->using_mag_sensor = false;
    }

    // Assert watchdog timeout flags.
    gyro_cal->gyro_watchdog_timeout |= watchdog_timeout;
    gyro_cal->gyro_watchdog_start_nanos = 0;
#ifdef GYRO_CAL_DBG_ENABLED
    gyro_cal->debug_watchdog_count++;
    CAL_DEBUG_LOG("[GYRO_CAL:WATCHDOG]", "Total#, Timestamp [nsec]: %lu, %llu",
                  (unsigned long int)gyro_cal->debug_watchdog_count,
                  (unsigned long long int)sample_time_nanos);
#endif  // GYRO_CAL_DBG_ENABLED
  }
}

// TODO(davejacobs) -- Combine the following two functions into one or consider
// implementing a separate helper module for tracking the temperature and mean
// statistics.
bool gyroTemperatureStatsTracker(struct GyroCal* gyro_cal,
                                 float temperature_celsius,
                                 enum GyroCalTrackerCommand do_this) {
  // This is used for local calculations of the running mean.
  static float mean_accumulator = 0.0f;
  static float temperature_min_max_celsius[2] = {0.0f, 0.0f};
  static size_t num_points = 0;
  bool min_max_temp_exceeded = false;

  switch (do_this) {
    case DO_RESET:
      // Resets the mean accumulator.
      num_points = 0;
      mean_accumulator = 0.0f;

      // Initializes the min/max temperatures values.
      temperature_min_max_celsius[0] = FLT_MAX;
      temperature_min_max_celsius[1] = -1.0f * (FLT_MAX - 1.0f);
      break;

    case DO_UPDATE_DATA:
      // Does the mean accumulation.
      mean_accumulator += temperature_celsius;
      num_points++;

      // Tracks the min and max temperature values.
      if (temperature_min_max_celsius[0] > temperature_celsius) {
        temperature_min_max_celsius[0] = temperature_celsius;
      }
      if (temperature_min_max_celsius[1] < temperature_celsius) {
        temperature_min_max_celsius[1] = temperature_celsius;
      }
      break;

    case DO_STORE_DATA:
      // Store the most recent "stillness" mean data to the GyroCal data
      // structure. This functionality allows previous results to be recalled
      // when the device suddenly becomes "not still".
      if (num_points > 0) {
        memcpy(gyro_cal->temperature_min_max_celsius,
               temperature_min_max_celsius, 2 * sizeof(float));
        gyro_cal->temperature_mean_celsius = mean_accumulator / num_points;
      }
      break;

    case DO_EVALUATE:
      // Determines if the min/max delta exceeded the set limit.
      if (num_points > 0) {
        min_max_temp_exceeded =
            (temperature_min_max_celsius[1] -
             temperature_min_max_celsius[0]) >
            gyro_cal->temperature_delta_limit_celsius;

#ifdef GYRO_CAL_DBG_ENABLED
        if (min_max_temp_exceeded) {
          CAL_DEBUG_LOG(
              "[GYRO_CAL:TEMP_GATE]",
              "Exceeded the max temperature variation during stillness.");
        }
#endif  // GYRO_CAL_DBG_ENABLED
      }
      break;

    default:
      break;
  }

  return min_max_temp_exceeded;
}

bool gyroStillMeanTracker(struct GyroCal* gyro_cal,
                          enum GyroCalTrackerCommand do_this) {
  static float gyro_winmean_min[3] = {0.0f, 0.0f, 0.0f};
  static float gyro_winmean_max[3] = {0.0f, 0.0f, 0.0f};
  bool mean_not_stable = false;
  size_t i;

  switch (do_this) {
    case DO_RESET:
      // Resets the min/max window mean values to a default value.
      for (i = 0; i < 3; i++) {
        gyro_winmean_min[i] = FLT_MAX;
        gyro_winmean_max[i] = -1.0f * (FLT_MAX - 1.0f);
      }
      break;

    case DO_UPDATE_DATA:
      // Computes the min/max window mean values.
      if (gyro_winmean_min[0] > gyro_cal->gyro_stillness_detect.win_mean_x) {
        gyro_winmean_min[0] = gyro_cal->gyro_stillness_detect.win_mean_x;
      }
      if (gyro_winmean_max[0] < gyro_cal->gyro_stillness_detect.win_mean_x) {
        gyro_winmean_max[0] = gyro_cal->gyro_stillness_detect.win_mean_x;
      }

      if (gyro_winmean_min[1] > gyro_cal->gyro_stillness_detect.win_mean_y) {
        gyro_winmean_min[1] = gyro_cal->gyro_stillness_detect.win_mean_y;
      }
      if (gyro_winmean_max[1] < gyro_cal->gyro_stillness_detect.win_mean_y) {
        gyro_winmean_max[1] = gyro_cal->gyro_stillness_detect.win_mean_y;
      }

      if (gyro_winmean_min[2] > gyro_cal->gyro_stillness_detect.win_mean_z) {
        gyro_winmean_min[2] = gyro_cal->gyro_stillness_detect.win_mean_z;
      }
      if (gyro_winmean_max[2] < gyro_cal->gyro_stillness_detect.win_mean_z) {
        gyro_winmean_max[2] = gyro_cal->gyro_stillness_detect.win_mean_z;
      }
      break;

    case DO_STORE_DATA:
      // Store the most recent "stillness" mean data to the GyroCal data
      // structure. This functionality allows previous results to be recalled
      // when the device suddenly becomes "not still".
      memcpy(gyro_cal->gyro_winmean_min, gyro_winmean_min, 3 * sizeof(float));
      memcpy(gyro_cal->gyro_winmean_max, gyro_winmean_max, 3 * sizeof(float));
    break;

    case DO_EVALUATE:
      // Performs the stability check and returns the 'true' if the difference
      // between min/max window mean value is outside the stable range.
      for (i = 0; i < 3; i++) {
        mean_not_stable |= (gyro_winmean_max[i] - gyro_winmean_min[i]) >
                           gyro_cal->stillness_mean_delta_limit;
      }
#ifdef GYRO_CAL_DBG_ENABLED
      if (mean_not_stable) {
        CAL_DEBUG_LOG("[GYRO_CAL:MEAN_STABILITY_GATE]",
                      "Exceeded the max variation in the gyro's stillness "
                      "window mean values.");
      }
#endif  // GYRO_CAL_DBG_ENABLED
      break;

    default:
      break;
  }

  return mean_not_stable;
}

#ifdef GYRO_CAL_DBG_ENABLED
void gyroSamplingRateUpdate(float* debug_mean_sampling_rate_hz,
                            uint64_t timestamp_nanos, bool reset_stats) {
  // This is used for local calculations of average sampling rate.
  static uint64_t last_timestamp_nanos = 0;
  static uint64_t time_delta_accumulator = 0;
  static size_t num_samples = 0;

  // If 'debug_mean_sampling_rate_hz' is not NULL then this function just reads
  // out the estimate of the sampling rate.
  if (debug_mean_sampling_rate_hz) {
    if (num_samples > 1 && time_delta_accumulator > 0) {
      // Computes the final mean calculation.
      *debug_mean_sampling_rate_hz =
          num_samples /
          (floatFromUint64(time_delta_accumulator) * NANOS_TO_SEC);
    } else {
      // Not enough samples to compute a valid sample rate estimate. Indicate
      // this with a -1 value.
      *debug_mean_sampling_rate_hz = -1.0f;
    }
    reset_stats = true;
  }

  // Resets the sampling rate mean estimator data.
  if (reset_stats) {
    last_timestamp_nanos = 0;
    time_delta_accumulator = 0;
    num_samples = 0;
    return;
  }

  // Skip adding this data to the accumulator if:
  //   1. A bad timestamp was received (i.e., time not monotonic).
  //   2. 'last_timestamp_nanos' is zero.
  if (timestamp_nanos <= last_timestamp_nanos || last_timestamp_nanos == 0) {
    last_timestamp_nanos = timestamp_nanos;
    return;
  }

  // Increments the number of samples.
  num_samples++;

  // Accumulate the time steps.
  time_delta_accumulator += timestamp_nanos - last_timestamp_nanos;
  last_timestamp_nanos = timestamp_nanos;
}

void gyroCalUpdateDebug(struct GyroCal* gyro_cal) {
  // Only update this data if debug printing is not currently in progress
  // (i.e., don't want to risk overwriting debug information that is actively
  // being reported).
  if (gyro_cal->debug_state != GYRO_IDLE) {
    return;
  }

  // Probability of stillness (acc, rot, still), duration, timestamp.
  gyro_cal->debug_gyro_cal.accel_stillness_conf =
      gyro_cal->accel_stillness_detect.prev_stillness_confidence;
  gyro_cal->debug_gyro_cal.gyro_stillness_conf =
      gyro_cal->gyro_stillness_detect.prev_stillness_confidence;
  gyro_cal->debug_gyro_cal.mag_stillness_conf =
      gyro_cal->mag_stillness_detect.prev_stillness_confidence;

  // Magnetometer usage.
  gyro_cal->debug_gyro_cal.using_mag_sensor = gyro_cal->using_mag_sensor;

  // Stillness start, stop, and duration times.
  gyro_cal->debug_gyro_cal.start_still_time_nanos =
      gyro_cal->start_still_time_nanos;
  gyro_cal->debug_gyro_cal.end_still_time_nanos =
      gyro_cal->calibration_time_nanos;
  gyro_cal->debug_gyro_cal.stillness_duration_nanos =
      gyro_cal->calibration_time_nanos - gyro_cal->start_still_time_nanos;

  // Records the current calibration values.
  gyro_cal->debug_gyro_cal.calibration[0] = gyro_cal->bias_x;
  gyro_cal->debug_gyro_cal.calibration[1] = gyro_cal->bias_y;
  gyro_cal->debug_gyro_cal.calibration[2] = gyro_cal->bias_z;

  // Records the mean gyroscope sampling rate.
  gyroSamplingRateUpdate(&gyro_cal->debug_gyro_cal.mean_sampling_rate_hz, 0,
                         /*reset_stats=*/true);

  // Records the min/max and mean temperature values.
  gyro_cal->debug_gyro_cal.temperature_mean_celsius =
      gyro_cal->temperature_mean_celsius;
  memcpy(gyro_cal->debug_gyro_cal.temperature_min_max_celsius,
         gyro_cal->temperature_min_max_celsius, 2 * sizeof(float));

  // Records the min/max gyroscope window stillness mean values.
  memcpy(gyro_cal->debug_gyro_cal.gyro_winmean_min, gyro_cal->gyro_winmean_min,
         3 * sizeof(float));
  memcpy(gyro_cal->debug_gyro_cal.gyro_winmean_max, gyro_cal->gyro_winmean_max,
         3 * sizeof(float));

  // Records the previous stillness window means.
  gyro_cal->debug_gyro_cal.accel_mean[0] =
      gyro_cal->accel_stillness_detect.prev_mean_x;
  gyro_cal->debug_gyro_cal.accel_mean[1] =
      gyro_cal->accel_stillness_detect.prev_mean_y;
  gyro_cal->debug_gyro_cal.accel_mean[2] =
      gyro_cal->accel_stillness_detect.prev_mean_z;

  gyro_cal->debug_gyro_cal.gyro_mean[0] =
      gyro_cal->gyro_stillness_detect.prev_mean_x;
  gyro_cal->debug_gyro_cal.gyro_mean[1] =
      gyro_cal->gyro_stillness_detect.prev_mean_y;
  gyro_cal->debug_gyro_cal.gyro_mean[2] =
      gyro_cal->gyro_stillness_detect.prev_mean_z;

  gyro_cal->debug_gyro_cal.mag_mean[0] =
      gyro_cal->mag_stillness_detect.prev_mean_x;
  gyro_cal->debug_gyro_cal.mag_mean[1] =
      gyro_cal->mag_stillness_detect.prev_mean_y;
  gyro_cal->debug_gyro_cal.mag_mean[2] =
      gyro_cal->mag_stillness_detect.prev_mean_z;

  // Records the variance data.
  // NOTE: These statistics include the final captured window, which may be
  // outside of the "stillness" period. Therefore, these values may exceed the
  // stillness thresholds.
  gyro_cal->debug_gyro_cal.accel_var[0] =
      gyro_cal->accel_stillness_detect.win_var_x;
  gyro_cal->debug_gyro_cal.accel_var[1] =
      gyro_cal->accel_stillness_detect.win_var_y;
  gyro_cal->debug_gyro_cal.accel_var[2] =
      gyro_cal->accel_stillness_detect.win_var_z;

  gyro_cal->debug_gyro_cal.gyro_var[0] =
      gyro_cal->gyro_stillness_detect.win_var_x;
  gyro_cal->debug_gyro_cal.gyro_var[1] =
      gyro_cal->gyro_stillness_detect.win_var_y;
  gyro_cal->debug_gyro_cal.gyro_var[2] =
      gyro_cal->gyro_stillness_detect.win_var_z;

  gyro_cal->debug_gyro_cal.mag_var[0] =
      gyro_cal->mag_stillness_detect.win_var_x;
  gyro_cal->debug_gyro_cal.mag_var[1] =
      gyro_cal->mag_stillness_detect.win_var_y;
  gyro_cal->debug_gyro_cal.mag_var[2] =
      gyro_cal->mag_stillness_detect.win_var_z;

  // Trigger a printout of the debug information.
  gyro_cal->debug_print_trigger = true;
}

void gyroCalDebugPrintData(const struct GyroCal* gyro_cal, char* debug_tag,
                           enum DebugPrintData print_data) {
  // Prints out the desired debug data.
  float mag_data;
  switch (print_data) {
    case OFFSET:
      CAL_DEBUG_LOG(debug_tag,
                    "Cal#|Offset|Temp|Time [mdps|C|nsec]: %lu, %s%d.%06d, "
                    "%s%d.%06d, %s%d.%06d, %s%d.%03d, %llu",
                    (unsigned long int)gyro_cal->debug_calibration_count,
                    CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.calibration[0] *
                                         RAD_TO_MILLI_DEGREES,
                                     6),
                    CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.calibration[1] *
                                         RAD_TO_MILLI_DEGREES,
                                     6),
                    CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.calibration[2] *
                                         RAD_TO_MILLI_DEGREES,
                                     6),
                    CAL_ENCODE_FLOAT(
                        gyro_cal->debug_gyro_cal.temperature_mean_celsius, 3),
                    (unsigned long long int)
                        gyro_cal->debug_gyro_cal.end_still_time_nanos);
      break;

    case STILLNESS_DATA:
      mag_data = (gyro_cal->debug_gyro_cal.using_mag_sensor)
                     ? gyro_cal->debug_gyro_cal.mag_stillness_conf
                     : -1.0f;  // Signals that magnetometer was not used.
      CAL_DEBUG_LOG(
          debug_tag,
          "Cal#|Start|End|Confidence [nsec]: %lu, %llu, %llu, "
          "%s%d.%03d, %s%d.%03d, %s%d.%03d",
          (unsigned long int)gyro_cal->debug_calibration_count,
          (unsigned long long int)
              gyro_cal->debug_gyro_cal.start_still_time_nanos,
          (unsigned long long int)gyro_cal->debug_gyro_cal.end_still_time_nanos,
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.gyro_stillness_conf, 3),
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.accel_stillness_conf, 3),
          CAL_ENCODE_FLOAT(mag_data, 3));
      break;

    case SAMPLE_RATE_AND_TEMPERATURE:
      CAL_DEBUG_LOG(
          debug_tag,
          "Cal#|Mean|Min|Max|Delta|Sample Rate [C|Hz]: %lu, %s%d.%03d, "
          "%s%d.%03d, %s%d.%03d, %s%d.%04d, %s%d.%03d",
          (unsigned long int)gyro_cal->debug_calibration_count,
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.temperature_mean_celsius,
                           3),
          CAL_ENCODE_FLOAT(
              gyro_cal->debug_gyro_cal.temperature_min_max_celsius[0], 3),
          CAL_ENCODE_FLOAT(
              gyro_cal->debug_gyro_cal.temperature_min_max_celsius[1], 3),
          CAL_ENCODE_FLOAT(
              gyro_cal->debug_gyro_cal.temperature_min_max_celsius[1] -
                  gyro_cal->debug_gyro_cal.temperature_min_max_celsius[0],
              4),
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.mean_sampling_rate_hz, 3));
      break;

    case GYRO_MINMAX_STILLNESS_MEAN:
      CAL_DEBUG_LOG(
          debug_tag,
          "Cal#|Gyro Peak Stillness Variation [mdps]: %lu, %s%d.%06d, "
          "%s%d.%06d, %s%d.%06d",
          (unsigned long int)gyro_cal->debug_calibration_count,
          CAL_ENCODE_FLOAT((gyro_cal->debug_gyro_cal.gyro_winmean_max[0] -
                            gyro_cal->debug_gyro_cal.gyro_winmean_min[0]) *
                               RAD_TO_MILLI_DEGREES,
                           6),
          CAL_ENCODE_FLOAT((gyro_cal->debug_gyro_cal.gyro_winmean_max[1] -
                            gyro_cal->debug_gyro_cal.gyro_winmean_min[1]) *
                               RAD_TO_MILLI_DEGREES,
                           6),
          CAL_ENCODE_FLOAT((gyro_cal->debug_gyro_cal.gyro_winmean_max[2] -
                            gyro_cal->debug_gyro_cal.gyro_winmean_min[2]) *
                               RAD_TO_MILLI_DEGREES,
                           6));
      break;

    case ACCEL_STATS:
      CAL_DEBUG_LOG(
          debug_tag,
          "Cal#|Accel Mean|Var [m/sec^2|(m/sec^2)^2]: %lu, "
          "%s%d.%06d, %s%d.%06d, %s%d.%06d, %s%d.%08d, %s%d.%08d, %s%d.%08d",
          (unsigned long int)gyro_cal->debug_calibration_count,
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.accel_mean[0], 6),
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.accel_mean[1], 6),
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.accel_mean[2], 6),
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.accel_var[0], 8),
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.accel_var[1], 8),
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.accel_var[2], 8));
      break;

    case GYRO_STATS:
      CAL_DEBUG_LOG(
          debug_tag,
          "Cal#|Gyro Mean|Var [mdps|(rad/sec)^2]: %lu, %s%d.%06d, "
          "%s%d.%06d, %s%d.%06d, %s%d.%08d, %s%d.%08d, %s%d.%08d",
          (unsigned long int)gyro_cal->debug_calibration_count,
          CAL_ENCODE_FLOAT(
              gyro_cal->debug_gyro_cal.gyro_mean[0] * RAD_TO_MILLI_DEGREES, 6),
          CAL_ENCODE_FLOAT(
              gyro_cal->debug_gyro_cal.gyro_mean[1] * RAD_TO_MILLI_DEGREES, 6),
          CAL_ENCODE_FLOAT(
              gyro_cal->debug_gyro_cal.gyro_mean[2] * RAD_TO_MILLI_DEGREES, 6),
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.gyro_var[0], 8),
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.gyro_var[1], 8),
          CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.gyro_var[2], 8));
      break;

    case MAG_STATS:
      if (gyro_cal->debug_gyro_cal.using_mag_sensor) {
        CAL_DEBUG_LOG(debug_tag,
                      "Cal#|Mag Mean|Var [uT|uT^2]: %lu, %s%d.%06d, "
                      "%s%d.%06d, %s%d.%06d, %s%d.%08d, %s%d.%08d, %s%d.%08d",
                      (unsigned long int)gyro_cal->debug_calibration_count,
                      CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.mag_mean[0], 6),
                      CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.mag_mean[1], 6),
                      CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.mag_mean[2], 6),
                      CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.mag_var[0], 8),
                      CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.mag_var[1], 8),
                      CAL_ENCODE_FLOAT(gyro_cal->debug_gyro_cal.mag_var[2], 8));
      } else {
        CAL_DEBUG_LOG(debug_tag,
                      "Cal#|Mag Mean|Var [uT|uT^2]: %lu, 0, 0, 0, -1.0, -1.0, "
                      "-1.0",
                      (unsigned long int)gyro_cal->debug_calibration_count);
      }
      break;

#ifdef GYRO_CAL_DBG_TUNE_ENABLED
    case ACCEL_STATS_TUNING:
      CAL_DEBUG_LOG(
          debug_tag,
          "Accel Mean|Var [m/sec^2|(m/sec^2)^2]: %s%d.%06d, "
          "%s%d.%06d, %s%d.%06d, %s%d.%08d, %s%d.%08d, %s%d.%08d",
          CAL_ENCODE_FLOAT(gyro_cal->accel_stillness_detect.prev_mean_x, 6),
          CAL_ENCODE_FLOAT(gyro_cal->accel_stillness_detect.prev_mean_y, 6),
          CAL_ENCODE_FLOAT(gyro_cal->accel_stillness_detect.prev_mean_z, 6),
          CAL_ENCODE_FLOAT(gyro_cal->accel_stillness_detect.win_var_x, 8),
          CAL_ENCODE_FLOAT(gyro_cal->accel_stillness_detect.win_var_y, 8),
          CAL_ENCODE_FLOAT(gyro_cal->accel_stillness_detect.win_var_z, 8));
      break;

    case GYRO_STATS_TUNING:
      CAL_DEBUG_LOG(
          debug_tag,
          "Gyro Mean|Var [mdps|(rad/sec)^2]: %s%d.%06d, %s%d.%06d, %s%d.%06d, "
          "%s%d.%08d, %s%d.%08d, %s%d.%08d",
          CAL_ENCODE_FLOAT(gyro_cal->gyro_stillness_detect.prev_mean_x *
                               RAD_TO_MILLI_DEGREES,
                           6),
          CAL_ENCODE_FLOAT(gyro_cal->gyro_stillness_detect.prev_mean_y *
                               RAD_TO_MILLI_DEGREES,
                           6),
          CAL_ENCODE_FLOAT(gyro_cal->gyro_stillness_detect.prev_mean_z *
                               RAD_TO_MILLI_DEGREES,
                           6),
          CAL_ENCODE_FLOAT(gyro_cal->gyro_stillness_detect.win_var_x, 8),
          CAL_ENCODE_FLOAT(gyro_cal->gyro_stillness_detect.win_var_y, 8),
          CAL_ENCODE_FLOAT(gyro_cal->gyro_stillness_detect.win_var_z, 8));
      break;

    case MAG_STATS_TUNING:
      if (gyro_cal->using_mag_sensor) {
        CAL_DEBUG_LOG(
            debug_tag,
            "Mag Mean|Var [uT|uT^2]: %s%d.%06d, %s%d.%06d, %s%d.%06d, "
            "%s%d.%08d, %s%d.%08d, %s%d.%08d",
            CAL_ENCODE_FLOAT(gyro_cal->mag_stillness_detect.prev_mean_x, 6),
            CAL_ENCODE_FLOAT(gyro_cal->mag_stillness_detect.prev_mean_y, 6),
            CAL_ENCODE_FLOAT(gyro_cal->mag_stillness_detect.prev_mean_z, 6),
            CAL_ENCODE_FLOAT(gyro_cal->mag_stillness_detect.win_var_x, 8),
            CAL_ENCODE_FLOAT(gyro_cal->mag_stillness_detect.win_var_y, 8),
            CAL_ENCODE_FLOAT(gyro_cal->mag_stillness_detect.win_var_z, 8));
      } else {
        CAL_DEBUG_LOG(GYROCAL_TUNE_TAG,
                      "Mag Mean|Var [uT|uT^2]: 0, 0, 0, -1.0, -1.0, -1.0");
      }
      break;
#endif  // GYRO_CAL_DBG_TUNE_ENABLED

    default:
      break;
  }
}

void gyroCalDebugPrint(struct GyroCal* gyro_cal, uint64_t timestamp_nanos) {
  static enum GyroCalDebugState next_state = GYRO_IDLE;
  static uint64_t wait_timer_nanos = 0;

  // This is a state machine that controls the reporting out of debug data.
  switch (gyro_cal->debug_state) {
    case GYRO_IDLE:
      // Wait for a trigger and start the debug printout sequence.
      if (gyro_cal->debug_print_trigger) {
        CAL_DEBUG_LOG(GYROCAL_REPORT_TAG, "");
        CAL_DEBUG_LOG(GYROCAL_REPORT_TAG, "Debug Version: %s",
                      GYROCAL_DEBUG_VERSION_STRING);
        gyro_cal->debug_print_trigger = false;  // Resets trigger.
        gyro_cal->debug_state = GYRO_PRINT_OFFSET;
      } else {
        gyro_cal->debug_state = GYRO_IDLE;
      }
      break;

    case GYRO_WAIT_STATE:
      // This helps throttle the print statements.
      if (timestamp_nanos >= GYROCAL_WAIT_TIME_NANOS + wait_timer_nanos) {
        gyro_cal->debug_state = next_state;
      }
      break;

    case GYRO_PRINT_OFFSET:
      gyroCalDebugPrintData(gyro_cal, GYROCAL_REPORT_TAG, OFFSET);
      wait_timer_nanos = timestamp_nanos;       // Starts the wait timer.
      next_state = GYRO_PRINT_STILLNESS_DATA;   // Sets the next state.
      gyro_cal->debug_state = GYRO_WAIT_STATE;  // First, go to wait state.
      break;

    case GYRO_PRINT_STILLNESS_DATA:
      gyroCalDebugPrintData(gyro_cal, GYROCAL_REPORT_TAG, STILLNESS_DATA);
      wait_timer_nanos = timestamp_nanos;       // Starts the wait timer.
      next_state = GYRO_PRINT_SAMPLE_RATE_AND_TEMPERATURE;  // Sets next state.
      gyro_cal->debug_state = GYRO_WAIT_STATE;  // First, go to wait state.
      break;

    case GYRO_PRINT_SAMPLE_RATE_AND_TEMPERATURE:
      gyroCalDebugPrintData(gyro_cal, GYROCAL_REPORT_TAG,
                            SAMPLE_RATE_AND_TEMPERATURE);
      wait_timer_nanos = timestamp_nanos;       // Starts the wait timer.
      next_state = GYRO_PRINT_GYRO_MINMAX_STILLNESS_MEAN;  // Sets next state.
      gyro_cal->debug_state = GYRO_WAIT_STATE;  // First, go to wait state.
      break;

    case GYRO_PRINT_GYRO_MINMAX_STILLNESS_MEAN:
      gyroCalDebugPrintData(gyro_cal, GYROCAL_REPORT_TAG,
                            GYRO_MINMAX_STILLNESS_MEAN);
      wait_timer_nanos = timestamp_nanos;       // Starts the wait timer.
      next_state = GYRO_PRINT_ACCEL_STATS;      // Sets the next state.
      gyro_cal->debug_state = GYRO_WAIT_STATE;  // First, go to wait state.
      break;

    case GYRO_PRINT_ACCEL_STATS:
      gyroCalDebugPrintData(gyro_cal, GYROCAL_REPORT_TAG, ACCEL_STATS);
      wait_timer_nanos = timestamp_nanos;       // Starts the wait timer.
      next_state = GYRO_PRINT_GYRO_STATS;       // Sets the next state.
      gyro_cal->debug_state = GYRO_WAIT_STATE;  // First, go to wait state.
      break;

    case GYRO_PRINT_GYRO_STATS:
      gyroCalDebugPrintData(gyro_cal, GYROCAL_REPORT_TAG, GYRO_STATS);
      wait_timer_nanos = timestamp_nanos;       // Starts the wait timer.
      next_state = GYRO_PRINT_MAG_STATS;        // Sets the next state.
      gyro_cal->debug_state = GYRO_WAIT_STATE;  // First, go to wait state.
      break;

    case GYRO_PRINT_MAG_STATS:
      gyroCalDebugPrintData(gyro_cal, GYROCAL_REPORT_TAG, MAG_STATS);
      wait_timer_nanos = timestamp_nanos;       // Starts the wait timer.
      next_state = GYRO_IDLE;                   // Sets the next state.
      gyro_cal->debug_state = GYRO_WAIT_STATE;  // First, go to wait state.
      break;

    default:
      // Sends this state machine to its idle state.
      wait_timer_nanos = timestamp_nanos;       // Starts the wait timer.
      gyro_cal->debug_state = GYRO_WAIT_STATE;  // First, go to wait state.
  }

#ifdef GYRO_CAL_DBG_TUNE_ENABLED
  if (gyro_cal->debug_state == GYRO_IDLE) {
    // This check keeps the tuning printout from interleaving with the above
    // debug print data.
    gyroCalTuneDebugPrint(gyro_cal, timestamp_nanos);
  }
#endif  // GYRO_CAL_DBG_TUNE_ENABLED
}

#ifdef GYRO_CAL_DBG_TUNE_ENABLED
void gyroCalTuneDebugPrint(const struct GyroCal* gyro_cal,
                           uint64_t timestamp_nanos) {
  static enum GyroCalDebugState debug_state = GYRO_IDLE;
  static enum GyroCalDebugState next_state = GYRO_IDLE;
  static uint64_t wait_timer_nanos = 0;

  // Output sensor variance levels to assist with tuning thresholds.
  //   i.  Within the first 300 seconds of boot: output interval = 5
  //       seconds.
  //   ii. Thereafter: output interval is 60 seconds.
  bool condition_i =
      ((timestamp_nanos <= 300000000000) &&
       (timestamp_nanos > 5000000000 + wait_timer_nanos));  // nsec
  bool condition_ii = (timestamp_nanos > 60000000000 + wait_timer_nanos);

  // This is a state machine that controls the reporting out of tuning data.
  switch (debug_state) {
    case GYRO_IDLE:
      // Wait for a trigger and start the data tuning printout sequence.
      if (condition_i || condition_ii) {
        CAL_DEBUG_LOG(GYROCAL_TUNE_TAG, "Temp [C]: %s%d.%03d",
                      CAL_ENCODE_FLOAT(gyro_cal->temperature_mean_celsius, 3));
        wait_timer_nanos = timestamp_nanos;   // Starts the wait timer.
        next_state = GYRO_PRINT_ACCEL_STATS;  // Sets the next state.
        debug_state = GYRO_WAIT_STATE;        // First, go to wait state.
      } else {
        debug_state = GYRO_IDLE;
      }
      break;

    case GYRO_WAIT_STATE:
      // This helps throttle the print statements.
      if (timestamp_nanos >= GYROCAL_WAIT_TIME_NANOS + wait_timer_nanos) {
        debug_state = next_state;
      }
      break;

    case GYRO_PRINT_ACCEL_STATS:
      gyroCalDebugPrintData(gyro_cal, GYROCAL_TUNE_TAG, ACCEL_STATS_TUNING);
      wait_timer_nanos = timestamp_nanos;  // Starts the wait timer.
      next_state = GYRO_PRINT_GYRO_STATS;  // Sets the next state.
      debug_state = GYRO_WAIT_STATE;       // First, go to wait state.
      break;

    case GYRO_PRINT_GYRO_STATS:
      gyroCalDebugPrintData(gyro_cal, GYROCAL_TUNE_TAG, GYRO_STATS_TUNING);
      wait_timer_nanos = timestamp_nanos;  // Starts the wait timer.
      next_state = GYRO_PRINT_MAG_STATS;   // Sets the next state.
      debug_state = GYRO_WAIT_STATE;       // First, go to wait state.
      break;

    case GYRO_PRINT_MAG_STATS:
      gyroCalDebugPrintData(gyro_cal, GYROCAL_TUNE_TAG, MAG_STATS_TUNING);
      wait_timer_nanos = timestamp_nanos;  // Starts the wait timer.
      next_state = GYRO_IDLE;              // Sets the next state.
      debug_state = GYRO_WAIT_STATE;       // First, go to wait state.
      break;

    default:
      // Sends this state machine to its idle state.
      wait_timer_nanos = timestamp_nanos;  // Starts the wait timer.
      debug_state = GYRO_IDLE;
  }
}
#endif  // GYRO_CAL_DBG_TUNE_ENABLED
#endif  // GYRO_CAL_DBG_ENABLED
