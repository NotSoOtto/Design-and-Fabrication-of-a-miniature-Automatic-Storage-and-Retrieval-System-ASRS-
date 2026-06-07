#include <AccelStepper.h>
#include <string.h>

#define LIMIT_SWITCH_PIN 3
#define PUL_PIN_1 4
#define DIR_PIN_1 5
#define PUL_PIN_2 6
#define DIR_PIN_2 7

#define link Serial2

// Initialise the AccelStepper library, the first argument is the interface type (DRIVER for step/direction)
AccelStepper stepper1(AccelStepper::DRIVER, PUL_PIN_1, DIR_PIN_1);
AccelStepper stepper2(AccelStepper::DRIVER, PUL_PIN_2, DIR_PIN_2);

//Insert the data input from the Webapp and the Raspberry Pi
//This includes 1) the tray number that is being involved and 2) 

// Variables to be set by the user
const long PULSES_PER_REVOLUTION = 1600;         // The iSV57T sets the PPR at 1600
const float lead = 5.0;                          // Linear distance (in mm) the nut travels along the screw for one full revolution of the ball screw actuator
const int gearRatio = 3;                         // Gear ratio between the herringbone pinion and gear (for 3 revolutions of pinion, there is 1 revolution of gear)
const float maxAngVelInRPM = 1500.0;              // Maximum angular velocity in RPM
const float maxAngAccInRPMperSecond = 500.0;     // Maximum angular acceleration in RPM

int trayNumber;                                                       // tray number involved in the current operation
float linearDistanceToTravel;                                         // Linear distance (in mm) the platform needs to travel upwards or downwards
bool waitingForExtractor;                                             // Safety feature in ensuring lift does not move when the extractor is conducting its motion
float totalRevolutionsByHerringbonePinion;                            // Number of revolutions to be underwent by the stepper motor based on the number of steps
const float maxAngVelInRPS = maxAngVelInRPM / 60.0;                   // Maximum angular velocity in RPS (Revolutions per second)
const float maxAngAccInRPSSquared = maxAngAccInRPMperSecond / 60.0;   // Maximum angular acceleration in RPS (Revolutions per second)

// liftModeOfMotion (0-4) dictates the vertical movement of the VLM's lift
int liftModeOfMotion;
// 0 - no vertical movement of the lift
// 1 - retrival motion from ground to storage;  Lift goes up from base to where the targeted tray is stored at
// 2 - retrival motion from storage to user;    Lift brings the targeted tray up to the user
// 3 - no vertical movement of the lift; Activated when the user clicks "Return Tray" button on the WebApp
// 4 - returning motion from user to storage;   Lift brings the targeted tray back down to where it was stored
// 5 - returning motion from storage to ground; Lift returns to the base after storing the targeted tray

//Set up a dictionary to links the Tray Number to the vertical distance it is held at and the column (left/right) the tray occupies

//the vertical distance each tray is stored at
const int trayPosition[] = {200, 220, 240, 260};
//  Example
//  trayPosition[0] = 150;   // Base level
//  trayPosition[1] = 250;   // 1st Tray Holder is at 250mm off the ground
//  trayPosition[2] = 400;   // 2nd Tray Holder is at 400mm off the ground
//  trayPosition[3] = 550;   // Top level where user interacts with the tray 

//the column each tray is stored at
const char* trayColumn[] = {"LEFT", "LEFT", "RIGHT", "RIGHT"};
//  Example
//  trayColumn[0] = "LEFT";   // Base level (can be left or right, it does not matter here)
//  trayColumn[1] = "LEFT";   // 1st Tray is stored on the left column
//  trayColumn[2] = "RIGHT";  // 2nd Tray is stored on the right column
//  trayColumn[3] (or the last value) = "RIGHT"; // Top right column is where user interacts with the tray

void actuatorMovement()
{
    totalRevolutionsByHerringbonePinion = (linearDistanceToTravel * gearRatio) / lead;
    stepper1.moveTo(totalRevolutionsByHerringbonePinion * PULSES_PER_REVOLUTION);
    stepper2.moveTo(totalRevolutionsByHerringbonePinion * PULSES_PER_REVOLUTION);
    stepper1.run(); //must be called repeatedly to run the motor
    stepper2.run();
}

void checkLimitSwitch()
{
  if (digitalRead(LIMIT_SWITCH_PIN) == LOW)
  {
    liftModeOfMotion = 0;
    stepper1.stop();
    stepper2.stop();
    stepper1.setCurrentPosition(0);
    stepper2.setCurrentPosition(0);
  }
}

//void readWebAppCommand()
//{
//  if (Serial.available() > 0 && liftModeOfMotion == 0)
//  {
//    String incoming = Serial.readStringUntil('\n');
//    incoming.trim();
//    int commaIndex = incoming.indexOf(',');
//    if (commaIndex == -1) 
//    {
//      return;
//    }
//    int receivedTrayNumber = incoming.substring(0, commaIndex).toInt();
//    String actionPart = incoming.substring(commaIndex + 1);
//    if (receivedTrayNumber < 1 || receivedTrayNumber > 3) 
//    {
//      return;
//    }
//    trayNumber = receivedTrayNumber;
//    if (actionPart == "BRING_UP_TRAY")
//    {
//      liftModeOfMotion = 1;
//    }
//    else if (actionPart == "RETURN_TRAY")
//    {
//      liftModeOfMotion = 3;
//    }
//  }
//}

void readExtractorCommand()
{
  if (link.available() > 0 && liftModeOfMotion == 0)
  {
    int incomingMode = link.parseInt();
    if (waitingForExtractor == true)
    {
      if (incomingMode == 2 || incomingMode == 4 || incomingMode == 5)
      {
        liftModeOfMotion = incomingMode;
        waitingForExtractor = false;
      }
    }
  }
}

void setup() 
{
  Serial.begin(9600);
  link.begin(9600); //for sending messages back and forth with the platform arduino
  delay(1000); //short delay for stability reasons

  pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);
  
  //initalise the liftModeOfMotion where it conducts no vertical actuation
  liftModeOfMotion = 1;
  waitingForExtractor = false;

  trayNumber = 2;
  
  // Sets the current stepper position as the start point (homing)
  stepper1.setCurrentPosition(0);
  stepper2.setCurrentPosition(0);

  // Intitalises the target position (absolute coordinate)
  stepper1.moveTo(0); 
  stepper2.moveTo(0); 
  
  // Set the maximum speed in steps per second
  stepper1.setMaxSpeed(maxAngVelInRPS * PULSES_PER_REVOLUTION);
  stepper2.setMaxSpeed(maxAngVelInRPS * PULSES_PER_REVOLUTION);
  
  // Set the acceleration in steps per second per second
  stepper1.setAcceleration(maxAngAccInRPSSquared * PULSES_PER_REVOLUTION);
  stepper2.setAcceleration(maxAngAccInRPSSquared * PULSES_PER_REVOLUTION);
  
  //temporary startup delay
  delay(3000); 
}

void loop() 
{
  //trayNumber we get from the webApp, along with the status of motion (one of the four), through serial communications with the Raspberry Pi. RPi sends signal for
  //2 signals that be sent are liftModeOfMotion = 1 or 3
  //readWebAppCommand();
  readExtractorCommand();
  
  if(liftModeOfMotion == 1)
  {
    linearDistanceToTravel = trayPosition[trayNumber]- trayPosition[0];
    actuatorMovement();
    if (stepper1.distanceToGo() == 0 && stepper2.distanceToGo() == 0)
    {
      liftModeOfMotion = 0; //stop the motor completely here
      stepper1.setCurrentPosition(0);
      stepper2.setCurrentPosition(0);
      waitingForExtractor = true;
      //send a signal to the extractor to run its course and the column it is in (the signal to send are to activate the motion of pulling out, either left or right)
      if (strcmp(trayColumn[trayNumber], "LEFT") == 0)
      {
        link.println(3);
      }
      else if (strcmp(trayColumn[trayNumber], "RIGHT") == 0)
      {
        link.println(4);
      } 
    }
  }
  
  if(liftModeOfMotion == 2)
  {
    linearDistanceToTravel = trayPosition[3] - trayPosition[trayNumber];
    actuatorMovement();
    if (stepper1.distanceToGo() == 0 && stepper2.distanceToGo() == 0)
    {
      liftModeOfMotion = 0;
      stepper1.setCurrentPosition(0);
      stepper2.setCurrentPosition(0);
      waitingForExtractor = true;
      if (strcmp(trayColumn[3], "LEFT") == 0)
      {
        link.println(1);
      }
      else if (strcmp(trayColumn[3], "RIGHT") == 0)
      {
        link.println(2);
      } 
    }
  }

  if(liftModeOfMotion == 3)
  {
    liftModeOfMotion = 0;
    waitingForExtractor = true;
    if (strcmp(trayColumn[3], "LEFT") == 0)
    {
      link.println(3);
    }
    else if (strcmp(trayColumn[3], "RIGHT") == 0)
    {
      link.println(4);
    } 
  }

  if(liftModeOfMotion == 4)
  {
    linearDistanceToTravel = trayPosition[trayNumber] - trayPosition[3];
    actuatorMovement();
    if (stepper1.distanceToGo() == 0 && stepper2.distanceToGo() == 0)
    {
      liftModeOfMotion = 0;
      stepper1.setCurrentPosition(0);
      stepper2.setCurrentPosition(0);
      waitingForExtractor = true;
      if (strcmp(trayColumn[trayNumber], "LEFT") == 0)
      {
        link.println(1);
      }
      else if (strcmp(trayColumn[trayNumber], "RIGHT") == 0)
      {
        link.println(2);
      } 
    }
  }

  if(liftModeOfMotion == 5)
  {
    linearDistanceToTravel = -500; //move all the way down to homing switch
    actuatorMovement();
    checkLimitSwitch(); // If it hits the homing switch, it stops automatically
    if (stepper1.distanceToGo() == 0 && stepper2.distanceToGo() == 0)
    {
      liftModeOfMotion = 0;
      stepper1.setCurrentPosition(0);
      stepper2.setCurrentPosition(0);
    }
  }
}
