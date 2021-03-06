// This code should help you get started with your balancing robot.
// The code performs the following steps
// Calibration phase:
//  In this phase the robot should be stationary lying on the ground.
//  The code will record the gyro data for a couple of seconds to zero
//  out any gyro drift.
//
// Waiting phase:
//  The robot will now start to integrate the gyro over time to estimate
//  the angle.  Once the angle gets within +/- 3 degrees of vertical,
//  we transition into the armed phase.  A buzzer will sound to indicate
//  this transition.
//
// Armed phase:
//  The robot is ready to go, however, it will not start executing its control
//  loop until the angle leaves the region of [-3 degrees, 3 degrees].  This
//  allows you to let go of your robot, and it won't start moving until it's started
//  to fall just a little bit.  Once it leaves the region around vertical, it enters
//  the controlled phase.
//
// Controlled phase:
//  Here you can implement your control logic to do your balancing (or any of the
//  other Olympic events.


#include <Balboa32U4.h>
#include <Wire.h>
#include <LSM6.h>
#include "Balance.h"

#define METERS_PER_CLICK 3.141592*80.0*(1/1000.0)/12.0/(162.5)
#define MOTOR_MAX 250
#define MAX_SPEED 0.75  // m/s
#define FORTY_FIVE_DEGREES_IN_RADIANS 0.78

extern int32_t angle_accum;
extern int32_t speedLeft;
extern int32_t driveLeft;
extern int32_t distanceRight;
extern int32_t speedRight;
extern int32_t distanceLeft;
extern int32_t distanceRight;

float testSpeed = 0;          // this is the desired motor speed
float mode = 0; // 0 = balance, 1 = race, 2 = turn, 3 = sumo

static float IL = 0;
static float IR = 0;
static float kpV = 500;
static float alpha  = 17;
static float beta   = 1.0 / 400.0;
static float kiV = 5000;//alpha*(1+beta*kpV)*(1+beta*kpV)/(4*beta);
static float kpT = 6;
static float g = 9.81;
static float l = 0.089;
//static float l = 0.165;
static float kiT = 36;//g+kpT*kpT/(4*l);
static float Itheta = 0;
static float IV = 0;
static float IVThreshold = 15.0 / 180.0 * PI;
static float kiEpsilon = 0.2; //0.1
static float kdEpsilon = 0;
static float epsilon = 0; //0;
static float vD = 0;

static float IV0 = 0.1;
static float vOffset = 0.2; // Changes target position over time, 0.2
static float sumoOffset = 0.2; // Changes target position over time, 0.2
static float turnRate = 0.04;

static float prevVL = 0;
static float prevVR = 0;
static float maxDeltaV = 4;

void balanceDoDriveTicks();

extern int32_t displacement;
int32_t prev_displacement = 0;

LSM6 imu;
Balboa32U4Motors motors;
Balboa32U4Encoders encoders;
Balboa32U4Buzzer buzzer;
Balboa32U4ButtonA buttonA;

uint32_t prev_time;

void setup()
{
  Serial.begin(9600);
  prev_time = 0;
  ledYellow(0);
  ledRed(1);
  balanceSetup();
  ledRed(0);
  angle_accum = 0;
  ledGreen(0);
  ledYellow(0);
  if (mode==1||mode==3) IV = IV0;
}

extern int16_t angle_prev;
int16_t start_flag = 0;
int16_t armed_flag = 0;
int16_t start_counter = 0;
void lyingDown();
extern bool isBalancingStatus;
extern bool balanceUpdateDelayedStatus;

void newBalanceUpdate()
{
  static uint32_t lastMillis;
  uint32_t ms = millis();

  if ((uint32_t)(ms - lastMillis) < UPDATE_TIME_MS) {
    return;
  }
  balanceUpdateDelayedStatus = ms - lastMillis > UPDATE_TIME_MS + 1;
  lastMillis = ms;

  // call functions to integrate encoders and gyros
  balanceUpdateSensors();

  if (imu.a.x < 0)
  {
    lyingDown();
    isBalancingStatus = false;
  }
  else
  {
    isBalancingStatus = true;
  }
}

float clamp(float a, float mi, float ma) {
  return min(max(a, mi), ma);
}

void loop()
{
  uint32_t cur_time = 0;
  static uint32_t prev_print_time = 0;   // this variable is to control how often we print on the serial monitor
  static float angle_rad;                // this is the angle in radians
  static float angle_rad_accum = 0;      // this is the accumulated angle in radians
  static float del_theta = 0;
  static float error_ = 0;      // this is the accumulated velocity error in m/s
  static float error_left_accum = 0;      // this is the accumulated velocity error in m/s
  static float error_right_accum = 0;      // this is the accumulated velocity error in m/s

  cur_time = millis();                   // get the current time in miliseconds



  newBalanceUpdate();                    // run the sensor updates. this function checks if it has been 10 ms since the previous

  if (angle > 3000 || angle < -3000)     // If angle is not within +- 3 degrees, reset counter that waits for start
  {
    start_counter = 0;
  }

  bool shouldPrint = cur_time - prev_print_time > 105;
  if (shouldPrint)  // do the printing every 105 ms. Don't want to do it for an integer multiple of 10ms to not hog the processor
  {
    Serial.print(cur_time);
    Serial.print("\t");
    Serial.print(speedLeft);
    Serial.print("\t");
    Serial.print(speedRight);
    Serial.print("\t");
    Serial.print(IL);
    Serial.print("\t");
    Serial.print(IR);
    Serial.print("\t");
    Serial.print(IV);
    Serial.print("\t");
    Serial.print(Itheta);
    Serial.print("\t");
    Serial.print(vD);
    Serial.println("");
    prev_print_time = cur_time;
  }

  float delta_t = (cur_time - prev_time) / 1000.0;

  // handle the case where this is the first time through the loop
  if (prev_time == 0) {
    delta_t = 0.01;
  }

  // every UPDATE_TIME_MS, check if angle is within +- 3 degrees and we haven't set the start flag yet
  if (cur_time - prev_time > UPDATE_TIME_MS && angle > -3000 && angle < 3000 && !armed_flag)
  {
    // increment the start counter
    start_counter++;
    // If the start counter is greater than 30, this means that the angle has been within +- 3 degrees for 0.3 seconds, then set the start_flag
    if (start_counter > 30)
    {
      armed_flag = 1;
      buzzer.playFrequency(DIV_BY_10 | 445, 1000, 15);
    }
  }

  // angle is in millidegrees, convert it to radians and subtract the desired theta
  angle_rad = ((float)angle) / 1000 / 180 * 3.14159 - del_theta;

  // only start when the angle falls outside of the 3.0 degree band around 0.  This allows you to let go of the
  // robot before it starts balancing
  if (cur_time - prev_time > UPDATE_TIME_MS && (angle < -3000 || angle > 3000) && armed_flag)
  {
    start_flag = 1;
    armed_flag = 0;
    IL = 0;
    IR = 0;
    Itheta = 0;
    IV = 0;
    if (mode==1||mode==3) IV = IV0;
  }

  // every UPDATE_TIME_MS, if the start_flag has been set, do the balancing
  if (cur_time - prev_time > UPDATE_TIME_MS && start_flag)
  {
    // set the previous time to the current time for the next run through the loop
    prev_time = cur_time;

    // speedLeft and speedRight are just the change in the encoder readings
    // wee need to do some math to get them into m/s
    float vL = METERS_PER_CLICK * speedLeft / delta_t;
    float vR = METERS_PER_CLICK * speedRight / delta_t;

    if (abs(vL - prevVL) > maxDeltaV) {
      vL = prevVL;
    }

    if (abs(vR - prevVR) > maxDeltaV) {
      vR = prevVR;
    }

    prevVL = vL;
    prevVR = vR;

    float v = (vL + vR) / 2;


    // set PWM_left and PWM_right here
    float PWM_left;
    float PWM_right;


    float errTheta = angle_rad + IV * kiEpsilon + v * kdEpsilon;
    //angle = angle * 999 / 1000;

    Itheta += errTheta * delta_t;
    vD = kpT * errTheta + kiT * Itheta;
    float errL = (vD) - vL;
    float errR = (vD) - vR;
    if (mode==2) errL -= turnRate; // Turn mode: offset wheel velocities
    if (mode==2) errR += turnRate; // Turn mode: offset wheel velocities
    IL += errL * delta_t;
    IR += errR * delta_t;
    PWM_left = errL * kpV + IL * kiV;
    PWM_right = errR * kpV + IR * kiV;
    IV += v * delta_t;
    if (mode==3 && v > 0 && IV < 0) { // Sumo mode: recover from impacts
      IV = 0.2;
      IL = 0;
      IR = 0;
    }
    if (mode==3) IV += sumoOffset * delta_t; // Sumo mode: increase target position over time
    if (mode==1) IV += vOffset * delta_t; // Race mode: increase target position over time
    //IV = IV * 9995 / 10000;

    if (abs(PWM_left) > MOTOR_MAX) {
      PWM_left = (PWM_left > 0 ? 1 : -1) * MOTOR_MAX;
    }
    if (abs(PWM_right) > MOTOR_MAX) {
      PWM_right = (PWM_right > 0 ? 1 : -1) * MOTOR_MAX;
    }



    // if the robot is more than 45 degrees, shut down the motor
    if (start_flag && fabs(angle_rad) > FORTY_FIVE_DEGREES_IN_RADIANS) // TODO: this was set to angle < -0.78... I changd it to angle_rad
    {
      // reset the accumulated errors here
      //start_flag = 0;   /// wait for restart
      //prev_time = 0;
      //motors.setSpeeds(0, 0);
    } else if (start_flag) {
      //      motors.setSpeeds((int)PWM_left, (int)PWM_right);
    }
    motors.setSpeeds((int)PWM_left, (int)PWM_right);
  }

  // kill switch
  if (buttonA.getSingleDebouncedPress())
  {
    motors.setSpeeds(0, 0);
    armed_flag = 0;
    start_flag = 0;
    start_counter = 0;
    setup();
    while (!buttonA.getSingleDebouncedPress());
  }
}
