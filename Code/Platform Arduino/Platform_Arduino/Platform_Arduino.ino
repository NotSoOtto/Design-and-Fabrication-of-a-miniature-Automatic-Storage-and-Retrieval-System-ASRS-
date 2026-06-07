#include <SparkFun_TB6612.h>
#include <Servo.h>
#include <string.h>

#define AIN1 3
#define AIN2 4
#define PWMA 5
#define PWMB 6
#define BIN1 7
#define BIN2 8
#define STBY 9
#define MOTOR1_ENCODER_A 19
#define MOTOR1_ENCODER_B 32
#define MOTOR2_ENCODER_A 18
#define MOTOR2_ENCODER_B 30

#define SERVO_LEFT 29
#define SERVO_RIGHT 31

#define link Serial2

const int offsetA = 1;
const int offsetB = 1;

Motor motor1 = Motor(AIN1, AIN2, PWMA, offsetA, STBY);
Motor motor2 = Motor(BIN1, BIN2, PWMB, offsetB, STBY);

volatile long motorEncoderCount1 = 0;
volatile long motorEncoderCount2 = 0;
volatile uint8_t lastEncoded1 = 0;
volatile uint8_t lastEncoded2 = 0;
const long countsPerRevolution = 200;

Servo servoLeft;
Servo servoRight;
int servoMotorPos1 = 0; //variable to store the Servo Motor 1's position
int servoMotorPos2 = 0; //variable to store the Servo Motor 2's position

int nextLiftModeOfMotion; //tracks the mode of motion the lift underwent
bool extractorBusy;
int extractorModeOfMotion;
//0 -> Extractor remains still
//1 -> Extractor stores tray on left
//2 -> Extractor stores tray on right
//3 -> Extractor retrieves tray from left
//4 -> Extractor retrieves tray from right

//inputs are from the serial communication with the ground Arduino. 
//It recieves the signal of [1)that one of its 4 motions are complete, and we have to do the other]
//and [2)the tray column it is supposed to activate]
//Motor 1 goes to the right while Motor 2 goes to the left

void rightExtension()
{
  unsigned long startTime = millis();
  while(motorEncoderCount2 > -600)
  {
    motor2.drive(-200);
    encoderTracker();
    if (millis() - startTime >= 4000)  // This is added to all 4 Extractor's mode of motion as a safety measure, in case the motor keeps running once it reached its destination
    {
      break;
    }
  }
  motor2.drive(0);
  delay(1000);
}

void rightContraction()
{
  unsigned long startTime = millis();
  while(motorEncoderCount2 < 0)
  {
    motor2.drive(200);
    encoderTracker();
    if (millis() - startTime >= 4000)
    {
      break;
    }
  }
  motor2.drive(0);
  delay(1000);
}

void leftExtension()
{
  unsigned long startTime = millis();
  while(motorEncoderCount1 > -800)
  {
    motor1.drive(-200);
    encoderTracker();
    if (millis() - startTime >= 4000)
    {
      break;
    }
  }
  motor1.drive(0);
  delay(1000);
}

void leftContraction()
{
  unsigned long startTime = millis();
  while(motorEncoderCount1 < 0)
  {
    motor1.drive(200);
    encoderTracker();
    if (millis() - startTime >= 4000)
    {
      break;
    }
  }
  motor1.drive(0);
  delay(1000);
}

void updateEncoder1() {
  if (digitalRead(MOTOR1_ENCODER_B) == HIGH) 
  {
    motorEncoderCount1++;
  } 
  else 
  {
    motorEncoderCount1--;
  }
}

void updateEncoder2() {
  if (digitalRead(MOTOR2_ENCODER_B) == HIGH) 
  {
    motorEncoderCount2++;
  } 
  else 
  {
    motorEncoderCount2--;
  }
}

void encoderTracker()
{
    static unsigned long lastPrintTime = 0;

  if (millis() - lastPrintTime >= 500) {
      lastPrintTime = millis();
      long count1, count2;
  
      noInterrupts();
      count1 = motorEncoderCount1;
      count2 = motorEncoderCount2;
      interrupts();
    }
}

void activateNextLiftModeOfMotion()
{
  nextLiftModeOfMotion = nextLiftModeOfMotion + 1;
  if (nextLiftModeOfMotion == 2 || nextLiftModeOfMotion == 4 || nextLiftModeOfMotion == 5)
  {
  link.println(nextLiftModeOfMotion);
  }
  if (nextLiftModeOfMotion >= 5)
  {
  nextLiftModeOfMotion = 1;
  }
  delay(1000);
}

void setup()
{
  Serial.begin(9600);
  link.begin(9600); //for sending messages back and forth with the ground arduino
  delay(1000); //short delay for stability reasons
  servoLeft.write(0);
  servoRight.write(20);
  pinMode(MOTOR1_ENCODER_A, INPUT_PULLUP);
  pinMode(MOTOR1_ENCODER_B, INPUT_PULLUP);
  pinMode(MOTOR2_ENCODER_A, INPUT_PULLUP);
  pinMode(MOTOR2_ENCODER_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(MOTOR1_ENCODER_A), updateEncoder1, RISING);
  attachInterrupt(digitalPinToInterrupt(MOTOR2_ENCODER_A), updateEncoder2, RISING);
  servoLeft.attach(SERVO_LEFT); //attaches left servo motor to pin 30
  servoRight.attach(SERVO_RIGHT); //attaches right servo motor to pin 31
  extractorModeOfMotion = 0;
  nextLiftModeOfMotion = 1;
  extractorBusy = false;
} 


void loop()
{ 
  if (Serial2.available() && extractorModeOfMotion == 0 && extractorBusy == false)
  {
    extractorModeOfMotion = Serial2.parseInt();
  }
  if (extractorModeOfMotion == 1)
  {
    extractorBusy = true;
    servoLeft.write(90);   //Raises the left arm barrier up 90 degrees to latch onto the tray's handle
    delay(1000);
    leftExtension();       //While arm raised up, the extractor pushes the tray from the platform into the left column
    servoLeft.write(20);    //Right arm barrier lowers down after storing the tray
    delay(1000);
    leftContraction();     //Extractor returns to its original postion
    activateNextLiftModeOfMotion();
    extractorBusy = false;
    extractorModeOfMotion = 0;
  }
  
  if (extractorModeOfMotion == 2)
  {
    extractorBusy = true;
    servoRight.write(90);   //Raises the right arm barrier up 90 degrees to latch onto the tray's handle
    delay(1000);
    rightExtension();       //While arm raised up, the extractor pushes the tray from the platform into the right column
    servoRight.write(20);    //Right arm barrier lowers down after storing the tray
    delay(1000);
    rightContraction();     //Extractor returns to its original postion
    //SEND A SIGNAL TO THE GROUND ARDUINO THAT MOTION HERE IS DONE  
    activateNextLiftModeOfMotion();
    extractorBusy = false;
    extractorModeOfMotion = 0;
  }
  
  if (extractorModeOfMotion == 3)
  {
    extractorBusy = true;
    leftExtension();       //Extractor extends into the left column to position itself right under the target tray's handle
    servoLeft.write(90);   //Raises the left arm barrier up 90 degrees to latch onto the tray's handle
    delay(1000);
    leftContraction();     //Extractor contracts and drags the tray onto the platform
    servoLeft.write(20);    //Left arm barrier lowers down after securing the tray on the platform
    delay(1000);
    activateNextLiftModeOfMotion();
    extractorBusy = false;
    extractorModeOfMotion = 0;
  }
  
  if (extractorModeOfMotion == 4)
  {
    extractorBusy = true;
    rightExtension();       //Extractor extends into the right column to position itself right under the target tray's handle
    servoRight.write(90);   //Raises the right arm barrier up 90 degrees to latch onto the tray's handle
    delay(1000);
    rightContraction();     //Extractor contracts and drags the tray onto the platform
    servoRight.write(20);    //Right arm barrier lowers down after securing the tray on the platform
    delay(1000);
    activateNextLiftModeOfMotion();
    extractorBusy = false;
    extractorModeOfMotion = 0;
  }
}
