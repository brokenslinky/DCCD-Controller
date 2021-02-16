#include "Brocks_ACDC.h"

void setup() {
  Serial.begin(9600);
  Wire.begin();
  
  lcd_keyad.begin(16, 2);
  lcd_keyad.print("ACDC-duino by   ");
  lcd_keyad.setCursor(0,1);
  lcd_keyad.print("   Brock Palmer ");

  pinMode(SPEEDOMETER_PIN, INPUT);
  pinMode(CALIBRATION_PIN, INPUT);
  pinMode(SLICKNESS_PIN,   INPUT);
  pinMode(RAMP_PIN,        INPUT);

  pinMode(LED_GROUND_PIN,  OUTPUT);
  pinMode(POWER_OUT_PIN,   OUTPUT);
  pinMode(RED_PIN,         OUTPUT);
  pinMode(BLUE_PIN,        OUTPUT);
  pinMode(GREEN_PIN,       OUTPUT);

  // Change PWM frequency of POWER_OUT_PIN
  // Pin 9 should be on TCA0-WO0
  // TODO: Confirm this works
  // TCA0.SINGLE.CTRLA = (TCA0.SINGLE.CTRLA & 0xF1) | (0x4 << 1);
  TCA0.SINGLE.CTRLA = TCA_SINGLE_CLKSEL_DIV16_gc; // "| 0x1 might be needed to enable."
  // Default frequency is 976 Hz
  // 1/8 of this should be 122 Hz
  // I think 0x1 is already used. This is a divisor of 4, so I want a divisor of 32.
  // See https://ww1.microchip.com/downloads/en/DeviceDoc/ATmega4808-09-DataSheet-DS40002173B.pdf 
  
  lcdMode = 0;
  displayMode = 0;
  printIterationCounter = 0;

  // Initialize working variables
  speedoPeriod = 100.0; // 0.1 coresponds to ~100 mph. 100.0 coresponds to ~0.1 mph
  rollAngle    = 0.0;
  slip         = 0.0;
  pitchAngle   = 0.0;
  
  // Prepare LED
  digitalWrite(LED_GROUND_PIN, LOW);
  // Display orange light for initialization
  led_light(255, 125, 0);

  float orientationCal[3];

  // Default calibration values
  // orientationCal[0] = sqrt(3)/2; // x/g
  // orientationCal[1] = 0;         // y/g
  // orientationCal[2] = 1/2;       // z/g
  // for (int i = 0; i < 3; i++) {
  //   accelZero[i]  = 0;
  //   accelScale[i] = 0.1;
  // }
  // This estimates no yaw offset, 60 degree pitch, no roll, perfect zero balance and scale

  // Pull calibration data from EEPROM
  lcd_print("Getting cal data", "from EEPROM...  ");
  EEPROM_read_vector(ZERO_CAL_ADDR,        accelZero);
  EEPROM_read_vector(ZERO_CAL_ADDR + 3*4,  gyroOffset);
  EEPROM_read_vector(SCALE_CAL_ADDR,       accelScale);
  EEPROM_read_vector(ORIENTATION_CAL_ADDR, orientationCal);

  // Create calibration matrix from orientation data
  orientation_matrix.update(orientationCal);

  // Display blue light to indicate readiness
  led_light(0, 0, 255);
  lcd_print("Ready to race.  ");
}

void loop() {
  // Check the speedometer signal every iteration to make sure no pulses are missed.
  checkSpeedo();
  
  // Refresh data from Inertial Measurement Unit.
  IMU.readAcceleration(unadjusted_accel[0], unadjusted_accel[1], unadjusted_accel[2]);
  IMU.readGyroscope(unadjusted_rotation[0], unadjusted_rotation[1], unadjusted_rotation[2]);

  // Zero and scale the sensor data.
  float x = (unadjusted_accel[0] - accelZero[0]) * accelScale[0];
  float y = (unadjusted_accel[1] - accelZero[1]) * accelScale[1];
  float z = (unadjusted_accel[2] - accelZero[2]) * accelScale[2];
  float x_rot = (unadjusted_rotation[0] - accelZero[0]) * accelScale[0];
  float y_rot = (unadjusted_rotation[1] - accelZero[1]) * accelScale[1];
  float z_rot = (unadjusted_rotation[2] - accelZero[2]) * accelScale[2];

  // Convert from sensor coordinates to vehicle coordinates.
  float longitudinalAccel = orientation_matrix.longitudinal(x, y, z);
  float lateralAccel      = orientation_matrix.lateral     (x, y, z);
  float verticalAccel     = orientation_matrix.vertical    (x, y, z);
  float rollRate          = orientation_matrix.longitudinal(x_rot, y_rot, z_rot);
  float pitchRate         = orientation_matrix.lateral     (x_rot, y_rot, z_rot);
  float yawRate           = orientation_matrix.vertical    (x_rot, y_rot, z_rot);
  
  //convert gyros to SI units
  rollRate  *= 0.01745329251; // rad/s
  pitchRate *= 0.01745329251; // pi / 180
  yawRate   *= 0.01745329251;

  time = micros();
  if (time - previousIteration > 0) {
    iterationTime = time - previousIteration;
  }
  previousIteration = time;
  
  // Track orientation changes
  rollAngle  += rollRate  * iterationTime / 1000000;
  pitchAngle += pitchRate * iterationTime / 1000000;
  // Slip angle is more complicated since the reference frame is free to rotate in the yaw direction
  // these angles are zeroed when lateral acceleration is small
    
  // Zero the orientation any time the car is inertial.
  if (abs(longitudinalAccel) < 0.1 && abs(lateralAccel) < 0.1) {
    pitchAngle = 0.0;
    rollAngle  = 0.0;
    slip       = 0.0;
  }
  
  // Check user input
  if (lcd_keyad.readButtons() == BUTTON_SELECT) {
    perform_calibration();
  }
  
  // read the aggression and slip dials
  float friction = 2.4 - 0.2 * getRotaryKey(analogRead(SLICKNESS_PIN));
  //float desiredSlip = 0.0872664626 * getRotaryKey(analogRead(RAMP_PIN)); // 5 degrees per position
  rampRate = 1.0 / ( 3.0 - 0.25 * getRotaryKey(analogRead(RAMP_PIN)));
  
  // determine lockup amount (127 = 50% duty cycle MAX)
  int lockup = 0;
  if (rampRate == 0) {
    lockup = 127.0 * getRotaryKey(analogRead(SLICKNESS_PIN)) / NUM_KEYS; // manual mode if rampRate set to zero
  } else {
    float lock_from_acceleration = 1.625 * (longitudinalAccel * cos(slip) - abs(lateralAccel) * sin(slip)) / ( friction * verticalAccel) - 1;
    float lock_from_yaw_rate     = rampRate * 0.04559 * abs(yawRate) * longitudinalSpeed / 
                                   (abs(lateralAccel) * cos(slip) * cos(slip) + longitudinalAccel * cos(slip) * sin(slip));
    lockup = 127.0 * (lock_from_acceleration + lock_from_yaw_rate);
  }

  if (lockup > 127.0) {
    lockup = 127.0; // don't exceed 50% duty cycle
  }
  if (lockup < 0.0) {
    lockup = 0.0; // no negative PWM values
  }
  
  // send PWM signal to power shield
  analogWrite(POWER_OUT_PIN, lockup);

  // LED changes from green to red and becomes brighter as the diff locks.
  led_light(2 * lockup, 127 - lockup, 0);
  led_light(2 * lockup, 127 - lockup, 75 - lockup / 2.0);
  
  // print to LCD
  printIterationCounter++;
  if (printIterationCounter > iterationsBetweenPrints) {
    printIterationCounter = 0;
    lcd_keyad.clear();
    switch (lcdMode) {
      case LcdMode::STATS:
        if (displayMode == 0) {
          lcd_keyad.print("Center diff lock:");
          lcd_keyad.setCursor(0,1);
          lcd_keyad.print((float)lockup * 100.0 / 127.0);
          lcd_keyad.print(" %");
        } else if (displayMode == 1) {
          float horizontalAccel = sqrt(longitudinalAccel * longitudinalAccel + lateralAccel * lateralAccel);
          lcd_keyad.print("Hoizontal Accel:");
          lcd_keyad.setCursor(0,1);
          lcd_keyad.print(horizontalAccel / verticalAccel);
          lcd_keyad.print(" g");
        } else if (displayMode == 2) {
          lcd_keyad.print("Roll Angle:");
          lcd_keyad.setCursor(0,1);
          lcd_keyad.print(rollAngle * 180.0 / PI);
          lcd_keyad.print(" degrees");
        } else if (displayMode == 3) {
          lcd_keyad.print("Pitch Angle:");
          lcd_keyad.setCursor(0,1);
          lcd_keyad.print(pitchAngle * 180.0 / PI);
          lcd_keyad.print(" degrees");
        } else if (displayMode > 3) {
          displayMode = 0;
        } else if (displayMode < 0) {
          displayMode = 3;
        }
        break;

      case LcdMode::INPUTS:
        if (displayMode == 0) {
          lcd_keyad.print("Longitudinal Acc:");
          lcd_keyad.setCursor(0,1);
          lcd_keyad.print(longitudinalAccel / verticalAccel);
          lcd_keyad.print(" g");
        } else if (displayMode == 1) {
          lcd_keyad.print("Lateral Accel:");
          lcd_keyad.setCursor(0,1);
          lcd_keyad.print(lateralAccel / verticalAccel);
          lcd_keyad.print(" g");
        } else if (displayMode == 2) {
          lcd_keyad.print("Yaw Rate:");
          lcd_keyad.setCursor(0,1);
          lcd_keyad.print(yawRate * 180.0 / PI);
          lcd_keyad.print(" deg/s");
        } else if (displayMode == 3) {
          lcd_keyad.print("Roll Rate:");
          lcd_keyad.setCursor(0,1);
          lcd_keyad.print(rollRate * 180.0 / PI);
          lcd_keyad.print(" deg/s");
        } else if (displayMode == 4) {
          lcd_keyad.print("Pitch Rate:");
          lcd_keyad.setCursor(0,1);
          lcd_keyad.print(pitchRate * 180.0 / PI);
          lcd_keyad.print(" deg/s");
        } else if (displayMode == 5) {
          lcd_keyad.print("Gravity:");
          lcd_keyad.setCursor(0,1);
          lcd_keyad.print(verticalAccel / 256.0);
          lcd_keyad.print(" g");
        } else if (displayMode == 6) {
          lcd_keyad.print("Speed:");
          lcd_keyad.setCursor(0,1);
          lcd_keyad.print(longitudinalSpeed);
          lcd_keyad.print(" mph");
        } else if (displayMode > 6) {
          displayMode = 0;
        } else if (displayMode < 0) {
          displayMode = 6;
        };
    }
  }

  read_buttons();
}

void read_buttons() {
  uint8_t button = lcd_keyad.readButtons();
  switch (button) {
    case 0x00:
      break;
    case BUTTON_RIGHT:
      displayMode++;
      delay(200);
    case BUTTON_LEFT:
      displayMode--;
      delay(200);
    case BUTTON_DOWN:
      displayMode = 0;
      if (lcdMode == LcdMode::ENUM_END - 1) {
        lcdMode = 0;
      } else {
        lcdMode++;
      }
      if        (lcdMode == LcdMode::STATS) {
        lcd_keyad.clear();
        lcd_keyad.print("Status");
        delay(2000);
      } else if (lcdMode == LcdMode::INPUTS) {
        lcd_keyad.clear();
        lcd_keyad.print("Inputs");
        delay(2000);
      } else if (lcdMode == LcdMode::ERRORS) {
        lcd_keyad.clear();
        lcd_keyad.print("Errors");
        delay(2000);
      }

    case BUTTON_UP:
      displayMode = 0;
      if (lcdMode == 0) {
        lcdMode = LcdMode::ENUM_END - 1;
      } else {
        lcdMode--;
      }
      if        (lcdMode == LcdMode::STATS) {
        lcd_keyad.clear();
        lcd_keyad.print("Status");
        delay(2000);
      } else if (lcdMode == LcdMode::INPUTS) {
        lcd_keyad.clear();
        lcd_keyad.print("Inputs");
        delay(2000);
      } else if (lcdMode == LcdMode::ERRORS) {
        lcd_keyad.clear();
        lcd_keyad.print("Errors");
        delay(2000);
      }
  }
}

void checkSpeedo() {
  if (analogRead(SPEEDOMETER_PIN) > 64 /* ~0.3125V */) {
    if (!speedo_triggered) {      
      speedo_triggered = true;
      time = micros();
      if (time - lastTick > 0) 
        speedoPeriod = (time - lastTick) / 1000.0; // milliseconds
      lastTick = time;
      longitudinalSpeed = /*speedCorrection **/ 877.193 / speedoPeriod; //+0.125; //approximate mph
    }
  }
  if (analogRead(SPEEDOMETER_PIN) < 32 /* ~0.15625V */) {
    speedo_triggered = false;
  }
}

void perform_calibration() {
  // while car is stationary, calibrate orientation and zero gyros
  float x = 0.0;
  float y = 0.0;
  float z = 0.0;
  float g = 0.0;
  for (int j = 0; j<3; j++) {
    gyroOffset[j] = 0;
  }
  float tmp[3];

  lcd_print("  Measuring     ", 
            "  Orientation   ");

  for (int i = 0; i < calibrationIterations; i++) {
    // Display orange light for wait
    led_light(255, 150, 0);
    
    // Determine direction of gravity
    IMU.readAcceleration(unadjusted_accel[0], unadjusted_accel[1], unadjusted_accel[2]);
    for ( int j = 0; j < 3; j++)
    {
      tmp[j] = (unadjusted_accel[j] - accelZero[j]) * accelScale[j];
    }
    x += tmp[0];
    y += tmp[1];
    z += tmp[2];
    g += sqrt(tmp[0] * tmp[0] + tmp[1] * tmp[1] + tmp[2] * tmp[2]);

    // Zero the gyro.
    IMU.readGyroscope(unadjusted_rotation[0], unadjusted_rotation[1], unadjusted_rotation[2]);
    for (int j = 0; j < 3; j++) {
      gyroOffset[j] += unadjusted_rotation[j];
    }

    delay(2); // Give the IMU time to refresh.
  }

  // save orientation to ROM and RAM
  {
    float orientation[3] = { x / g, y / g, z / g };
    EEPROM_write_vector(ORIENTATION_CAL_ADDR, orientation);
    orientation_matrix.update(orientation);
  }
  
  // convert gyroOffset from sum to average and write to EEPROM
  for (int j = 0; j < 3; j++) {
    gyroOffset[j] /= calibrationIterations;
  }
  EEPROM_write_vector(ZERO_CAL_ADDR + 4*3, gyroOffset);

  lcd_print("  Calibration   ",
            "   Complete     ");
  led_light(0, 255, 0); // green light
}
