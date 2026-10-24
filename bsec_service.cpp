#include "firmware_features.h"
#if SENSOR_SERVICE_ENABLED && SENSOR_BSEC_ENABLED
#include "bsec_service.h"
#include "storage_service.h"
#include <atomic>
#include "config.h"
#include "log_service.h"
#include <Arduino.h>
#include <Wire.h>
#include <mbed.h>
#include <bsec2.h>
#include <inc/bsec_interface.h>
#include <math.h>
#include <string.h>
#include <chrono>

// Only public Bosch APIs are used. The worker owns the sensor and algorithm;
// main owns Serial and calibration persistence, outside radio transactions.
static Bme68x gBme;
static std::atomic<bool> gStopRequested(false), gWorkerStopped(true);
static constexpr uint32_t CALIBRATION_PROFILE = 0x26103304UL;
static rtos::Mutex gBsecMutex;
alignas(8) static unsigned char gWorkerStack[8192];
static rtos::Thread gWorker(osPriorityNormal, sizeof(gWorkerStack), gWorkerStack, "bsec");
static const uint8_t gProfile[] = {
#include <config/bme688/bme688_sel_33v_3s_4d/bsec_selectivity.txt>
};
static uint8_t gWork[BSEC_MAX_WORKBUFFER_SIZE];
static uint8_t gRestore[BSEC_MAX_STATE_BLOB_SIZE];
static uint16_t gRestoreSize = 0;
static uint8_t gSave[BSEC_MAX_STATE_BLOB_SIZE];
static uint16_t gSaveSize = 0;
static bool gSavePending = false;
static SensorSnapshot gSnapshot = {};
static uint32_t gSnapshotMs = 0;
static bool gHaveSnapshot = false;
static bool gPresent = false;
static int gDiagnostic = 0;
static bool gStarted = false;
static bool gWireStarted = false;
static bool gReady = false;
static bool gPending = false;
static bool gEverSaved = false;
static uint32_t gLastSaveMs = 0;
static uint32_t gRetryStartedMs = 0;
static uint32_t gTriggerMs = 0;
static uint32_t gWaitMs = 0;
static uint64_t gTimeMs = 0;
static uint32_t gLastTimeMs = 0;
static int64_t gNextCallNs = 0;
static bsec_bme_settings_t gSettings = {};
static constexpr uint32_t SAVE_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
static_assert(BSEC_MAX_STATE_BLOB_SIZE == 238, "Review calibration record size");
static_assert(SENSOR_I2C_ADDR == 0x76 || SENSOR_I2C_ADDR == 0x77, "Invalid BME68x address");

static void diagnostic(int code) {
  gBsecMutex.lock();
  gDiagnostic = code;
  gBsecMutex.unlock();
}

static void workerFailure(int code, uint32_t now) {
  gReady = false;
  gPending = false;
  gRetryStartedMs = now;
  gBsecMutex.lock();
  gPresent = false;
  gHaveSnapshot = false;
  gDiagnostic = code;
  gBsecMutex.unlock();
}

static bool configureAlgorithm() {
  return bsec_init() == BSEC_OK &&
    bsec_set_configuration(gProfile, sizeof(gProfile), gWork, sizeof(gWork)) == BSEC_OK;
}

static bool workerBegin(uint32_t now) {
#if defined(SENSOR_USE_WIRE1) && (SENSOR_USE_WIRE1 == 1)
  if(!gWireStarted) { Wire1.begin(); gWireStarted = true; }
  gBme.begin(SENSOR_I2C_ADDR, Wire1);
#else
  if(!gWireStarted) { Wire.begin(); gWireStarted = true; }
  gBme.begin(SENSOR_I2C_ADDR, Wire);
#endif
  if(gBme.status != BME68X_OK) { workerFailure(gBme.status, now); return false; }
  if(!configureAlgorithm()) { workerFailure(-1001, now); return false; }
  if(gRestoreSize && bsec_set_state(gRestore, gRestoreSize, gWork, sizeof(gWork)) != BSEC_OK) {
    // A CRC-valid record may belong to an incompatible algorithm. Start clean.
    gRestoreSize = 0;
    gEverSaved = false;
    if(!configureAlgorithm()) { workerFailure(-1001, now); return false; }
    diagnostic(-1002);
  }
  const uint8_t ids[] = {BSEC_OUTPUT_IAQ, BSEC_OUTPUT_CO2_EQUIVALENT,
    BSEC_OUTPUT_BREATH_VOC_EQUIVALENT, BSEC_OUTPUT_STABILIZATION_STATUS,
    BSEC_OUTPUT_RUN_IN_STATUS, BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY};
  bsec_sensor_configuration_t requested[sizeof(ids)] = {};
  for(size_t i = 0; i < sizeof(ids); ++i) {
    requested[i].sensor_id = ids[i];
    requested[i].sample_rate = BSEC_SAMPLE_RATE_LP;
  }
  bsec_sensor_configuration_t required[BSEC_MAX_PHYSICAL_SENSOR] = {};
  uint8_t count = BSEC_MAX_PHYSICAL_SENSOR;
  const int state = bsec_update_subscription(requested, sizeof(ids), required, &count);
  if(state != BSEC_OK) { workerFailure(state, now); return false; }
  gSettings = {};
  gNextCallNs = 0;
  gPending = false;
  gReady = true;
  gBsecMutex.lock();
  gPresent = true;
  gBsecMutex.unlock();
  return true;
}

static void addInput(bsec_input_t* inputs, uint8_t& count, uint8_t id, float value, int64_t time) {
  if(gSettings.process_data & (1UL << (id - 1))) {
    inputs[count].sensor_id = id;
    inputs[count].signal = value;
    inputs[count++].time_stamp = time;
  }
}

static void workerFetch(uint32_t now) {
  if(uint32_t(now - gTriggerMs) < gWaitMs) { return; }
  if(uint32_t(now - gTriggerMs) > gWaitMs + 1000UL) { workerFailure(-1003, now); return; }
  const uint8_t count = gBme.fetchData();
  if(gBme.status < BME68X_OK) { workerFailure(gBme.status, now); return; }
  bme68xData raw = {};
  if(count) { gBme.getData(raw); }  // Return value is fields remaining, not success.
  if(!count || !(raw.status & BME68X_NEW_DATA_MSK)) { return; }
  gPending = false;
  const bool tphValid = isfinite(raw.temperature) && raw.temperature >= -40 && raw.temperature <= 85 &&
    isfinite(raw.humidity) && raw.humidity >= 0 && raw.humidity <= 100 &&
    isfinite(raw.pressure) && raw.pressure >= 30000 && raw.pressure <= 110000;
  // A numeric failure belongs to this acquisition, not the entire device.
  // Publish independently valid raw fields and keep the normal cadence. BSEC
  // outputs are unavailable until a conversion has all required valid inputs.
  if(!tphValid) { diagnostic(-1004); }
  SensorSnapshot sample = {};
  sample.environment = {raw.temperature, raw.humidity, raw.pressure, 0, false};
  const uint8_t gasMask = BME68X_GASM_VALID_MSK | BME68X_HEAT_STAB_MSK;
  const bool gasValid = (raw.status & gasMask) == gasMask &&
    isfinite(raw.gas_resistance) && raw.gas_resistance > 0 && double(raw.gas_resistance) <= UINT32_MAX;
  if(gasValid) {
    sample.environment.gasOhms = uint32_t(raw.gas_resistance);
    sample.environment.gasValid = true;
  }
  const uint64_t completedMs = gTimeMs - uint32_t(now - gTriggerMs) + gWaitMs;
  sample.sampleUptimeSeconds = uint32_t(completedMs / 1000ULL);
  if(gasValid && tphValid) {
    bsec_input_t inputs[BSEC_MAX_PHYSICAL_SENSOR] = {};
    uint8_t nInputs = 0;
    const int64_t stamp = int64_t(completedMs) * 1000000LL;
    addInput(inputs, nInputs, BSEC_INPUT_TEMPERATURE, raw.temperature, stamp);
    addInput(inputs, nInputs, BSEC_INPUT_HUMIDITY, raw.humidity, stamp);
    addInput(inputs, nInputs, BSEC_INPUT_PRESSURE, raw.pressure, stamp); // Pa, per public C API.
    addInput(inputs, nInputs, BSEC_INPUT_GASRESISTOR, raw.gas_resistance, stamp);
    addInput(inputs, nInputs, BSEC_INPUT_HEATSOURCE, 0.0f, stamp);
    addInput(inputs, nInputs, BSEC_INPUT_PROFILE_PART, raw.gas_index, stamp);
    bsec_output_t outputs[BSEC_NUMBER_OUTPUTS] = {};
    uint8_t nOutputs = BSEC_NUMBER_OUTPUTS;
    const int result = bsec_do_steps(inputs, nInputs, outputs, &nOutputs);
    if(result < BSEC_OK) { workerFailure(result, now); return; }
    if(result > BSEC_OK) { diagnostic(result); }
    float compensatedTemperature = NAN;
    float compensatedHumidity = NAN;
    for(uint8_t i = 0; i < nOutputs; ++i) {
      const bsec_output_t& value = outputs[i];
      if(!isfinite(value.signal)) { continue; }
      switch(value.sensor_id) {
        case BSEC_OUTPUT_IAQ:
          if(value.signal >= 0 && value.signal <= 500 && value.accuracy <= 3) {
            sample.airQuality.iaq = value.signal;
            sample.airQuality.iaqAccuracy = value.accuracy;
            sample.airQuality.validMask |= 1;
          }
          break;
        case BSEC_OUTPUT_CO2_EQUIVALENT:
          if(value.signal >= 0 && value.signal <= 65535) {
            sample.airQuality.co2EquivalentPpm = value.signal;
            sample.airQuality.validMask |= 2;
          }
          break;
        case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
          if(value.signal >= 0) {
            sample.airQuality.breathVocEquivalentPpm = value.signal;
            sample.airQuality.validMask |= 4;
          }
          break;
        case BSEC_OUTPUT_STABILIZATION_STATUS: sample.airQuality.stabilized = value.signal == 1; break;
        case BSEC_OUTPUT_RUN_IN_STATUS: sample.airQuality.runIn = value.signal == 1; break;
        case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE: compensatedTemperature = value.signal; break;
        case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY: compensatedHumidity = value.signal; break;
        default: break;
      }
    }
    if(isfinite(compensatedTemperature) && compensatedTemperature >= -40 && compensatedTemperature <= 85 &&
       isfinite(compensatedHumidity) && compensatedHumidity >= 0 && compensatedHumidity <= 100) {
      sample.environment.temperatureC = compensatedTemperature;
      sample.environment.humidityPct = compensatedHumidity;
      sample.temperatureHumidityCompensated = true;
    }
  }
  gBsecMutex.lock();
  gSnapshot = sample;
  gSnapshotMs = gTriggerMs + gWaitMs;
  gHaveSnapshot = true;
  const bool canSave = !gSavePending;
  gBsecMutex.unlock();
  storageAppend(sample, gTriggerMs + gWaitMs);
  if(canSave && sample.airQuality.iaqAccuracy == 3 && sample.airQuality.validMask == 7 &&
     sample.airQuality.stabilized && sample.airQuality.runIn &&
     (!gEverSaved || uint32_t(now - gLastSaveMs) >= SAVE_INTERVAL_MS)) {
    uint8_t state[BSEC_MAX_STATE_BLOB_SIZE] = {};
    uint32_t size = sizeof(state);
    if(bsec_get_state(0, state, sizeof(state), gWork, sizeof(gWork), &size) == BSEC_OK && size && size <= sizeof(state)) {
      memcpy(gRestore, state, size);
      gRestoreSize = size;
      gBsecMutex.lock();
      memcpy(gSave, state, size);
      gSaveSize = size;
      gSavePending = true;
      gBsecMutex.unlock();
      gEverSaved = true;
      gLastSaveMs = now;
    }
  }
}

// A bounded step is also exercised by host tests. No logging/flash in this thread.
static void workerStep() {
  const uint32_t now = millis();
  gTimeMs += uint32_t(now - gLastTimeMs);
  gLastTimeMs = now;
  if(!gReady) {
    if(uint32_t(now - gRetryStartedMs) < 60000UL) { return; }
    if(!workerBegin(now)) { return; }
  }
  if(gPending) { workerFetch(now); return; }
  const int64_t timeNs = int64_t(gTimeMs) * 1000000LL;
  if(timeNs < gNextCallNs) { return; }
  const int state = bsec_sensor_control(timeNs, &gSettings);
  if(state < BSEC_OK) { workerFailure(state, now); return; }
  if(state > BSEC_OK) { diagnostic(state); }
  gNextCallNs = gSettings.next_call;
  if(gNextCallNs <= timeNs || gNextCallNs - timeNs > 600000000000LL) { workerFailure(-1005, now); return; }
  if(!gSettings.trigger_measurement) { return; }
  if(gSettings.op_mode != BME68X_FORCED_MODE) { workerFailure(-1006, now); return; }
  gBme.setTPH(gSettings.temperature_oversampling, gSettings.pressure_oversampling, gSettings.humidity_oversampling);
  if(gBme.status != BME68X_OK) { workerFailure(gBme.status, now); return; }
  gBme.setHeaterProf(gSettings.heater_temperature, gSettings.heater_duration);
  if(gBme.status != BME68X_OK) { workerFailure(gBme.status, now); return; }
  const uint32_t duration = gBme.getMeasDur(BME68X_FORCED_MODE);
  if(!duration || duration > 1000000UL || gSettings.heater_duration > 1000) { workerFailure(-1007, now); return; }
  gWaitMs = (duration + 999UL) / 1000UL + gSettings.heater_duration + 5UL;
  gBme.setOpMode(BME68X_FORCED_MODE);
  if(gBme.status != BME68X_OK) { workerFailure(gBme.status, now); return; }
  gTriggerMs = millis();
  gPending = true;
}

static void workerLoop() {
  while(!gStopRequested.load()) {
    workerStep();
    rtos::ThisThread::sleep_for(std::chrono::milliseconds(10));
  }
  gWorkerStopped.store(true);
}

void bsecStart() {
  if(gStarted) { return; }
  gRestoreSize = storageLoadCalibration(gRestore, sizeof(gRestore), CALIBRATION_PROFILE);
  const bool restored = gRestoreSize != 0;
  gEverSaved = restored;
  gLastSaveMs = millis();
  gLastTimeMs = millis();
  gTimeMs = gLastTimeMs;
  gRetryStartedMs = gLastTimeMs - 60000UL;
  gStopRequested.store(false);
  gWorkerStopped.store(false);
  if(gWorker.start(mbed::callback(workerLoop)) != osOK) {
    gWorkerStopped.store(true);
    LOG_ERROR("Sensor", "BSEC worker could not start; sensor reports unavailable.");
    return;
  }
  gStarted = true;
  LOG_INFO("Sensor", "BSEC enabled: 3-second acquisitions commit directly to RAM history; calibration %s.",
    restored ? "record found" : "starts fresh");
}

bool bsecLatest(SensorSnapshot* data) {
  if(data == nullptr) { return false; }
  gBsecMutex.lock();
  const bool fresh = gHaveSnapshot && gPresent && uint32_t(millis() - gSnapshotMs) <= 10000UL;
  if(fresh) { *data = gSnapshot; }
  gBsecMutex.unlock();
  return fresh;
}

bool bsecPresent() {
  gBsecMutex.lock();
  const bool present = gPresent;
  gBsecMutex.unlock();
  return present;
}

void bsecPoll() {
  static bool wasPresent = false;
  const bool present = bsecPresent();
  if(present && !wasPresent) {
    LOG_INFO("Sensor", "BME68x/BSEC ready at I2C address 0x%02X.", unsigned(SENSOR_I2C_ADDR));
  }
  wasPresent = present;
  static uint32_t lastSaveAttempt = millis() - 60000UL;
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_INFO
  // Value formatting belongs to the sensor facade, with active unit settings.
#endif
  uint8_t state[BSEC_MAX_STATE_BLOB_SIZE] = {};
  uint16_t size = 0;
  gBsecMutex.lock();
  const int code = gDiagnostic;
  gDiagnostic = 0;
  if(gSavePending && uint32_t(millis() - lastSaveAttempt) >= 60000UL) {
    size = gSaveSize;
    memcpy(state, gSave, size);
    gSavePending = false;
  }
  gBsecMutex.unlock();
  if(code) { LOG_WARN("Sensor", "BSEC/sensor status=%d; acquisition retries automatically.", code); }
  if(size) {
    lastSaveAttempt = millis();
    if(!storageSaveCalibration(state, size, CALIBRATION_PROFILE)) {
      LOG_WARN("Sensor", "Calibration save failed; retry in 60 seconds.");
      gBsecMutex.lock();
      if(!gSavePending) { memcpy(gSave, state, size); gSaveSize = size; gSavePending = true; }
      gBsecMutex.unlock();
    }
  }

}
bool bsecStop() {
  gStopRequested.store(true);
  return !gStarted || gWorkerStopped.load();
}
#endif
