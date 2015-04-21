#include "drivetrain.h"
//#define DEBUG

Drivetrain::Drivetrain(int motors[kNumMotors], bool inverted[kNumMotors],
                       int encoder[kNumMotors * 2], int range[kNumMotors])
    : Loop(1e4 /* microseconds=>100Hz */),
      finv_(inverted[0]),
      linv_(inverted[1]),
      binv_(inverted[2]),
      rinv_(inverted[3]),
      fenc_(encoder[0], encoder[1]),
      lenc_(encoder[2], encoder[3]),
      benc_(encoder[4], encoder[5]),
      renc_(encoder[6], encoder[7]),
      cliff_(cliff_ports),
      wall_follow_(false),
      navigating_(false),
      uturn_(false),
      wall_on_left_(false),
      drive_dist_(-1),
      drive_dist_done_(true),
      lcd_(40, 41, 42, 43, 44, 45) {  // TODO: Break out range port definition.
  fmotor_.attach(motors[0], 1000, 2000);
  lmotor_.attach(motors[1], 1000, 2000);
  bmotor_.attach(motors[2], 1000, 2000);
  rmotor_.attach(motors[3], 1000, 2000);
  pos_.x = 0;
  pos_.y = 0;
  pos_.theta = 0;
  lcd_.begin(16, 2);
  for (int i = 0; i < kNumMotors; i++) {
    range_[i].init(range[i]);
    enc_[i] = 0;
    prev_enc_[i] = 0;
    enc_vel_[i] = 0;
  }
}

void Drivetrain::WriteMotors(int front, int left, int back, int right) {
  int fraw = PercentToServo(front);
  int lraw = PercentToServo(left);
  int braw = PercentToServo(back);
  int rraw = PercentToServo(right);
  fraw = finv_ ? 184 - fraw : fraw;
  lraw = linv_ ? 184 - lraw : lraw;
  braw = binv_ ? 184 - braw : braw;
  rraw = rinv_ ? 184 - rraw : rraw;
  int kMaxRaw = 175;
  int kMinRaw = 5;
  fraw = (fraw > kMaxRaw) ? kMaxRaw : (fraw < kMinRaw) ? kMinRaw : fraw;
  lraw = (lraw > kMaxRaw) ? kMaxRaw : (lraw < kMinRaw) ? kMinRaw : lraw;
  braw = (braw > kMaxRaw) ? kMaxRaw : (braw < kMinRaw) ? kMinRaw : braw;
  rraw = (rraw > kMaxRaw) ? kMaxRaw : (rraw < kMinRaw) ? kMinRaw : rraw;
#ifdef DEBUG
  Serial.print("Motor out vals: ");
  Serial.print(fraw);
  Serial.print("\t");
  Serial.print(lraw);
  Serial.print("\t");
  Serial.print(braw);
  Serial.print("\t");
  Serial.println(rraw);
#endif
  fmotor_.write(fraw);
  lmotor_.write(lraw);
  bmotor_.write(braw);
  rmotor_.write(rraw);
}

void Drivetrain::Run() {
  Serial.print(AvgRangeError(kUp));
  Serial.print("\t");
  Serial.print(AvgRangeError(kLeft));
  Serial.print("\t");
  Serial.print(AvgRangeError(kDown));
  Serial.print("\t");
  Serial.println(AvgRangeError(kRight));
  time_ = micros();

  UpdateEncoders();

#ifdef DEBUG
  Serial.print("stopping_: ");
  Serial.print(stopping_);
  Serial.print(" Dir: ");
  Serial.print(dir_);
  Serial.print("\t");
#endif

  // Update direction information:
  if (stopping_) {
    // Decide whether we have stopped yet.
    if (vel_.x < kMinVel && vel_.y < kMinVel) {
      Serial.println("Done Stopping!");
      stopping_ = false;
      Record leg;
      // Based on which coordinate we moved more in, determine which direction
      // we used to be going and add that distance to the total path.
      leg.dist = (pos_.y > pos_.x) ? pos_.y : pos_.x;
      leg.dist = abs(leg.dist);
      leg.heading = (pos_.y > pos_.x) ? // Determine Up/Down vs. Left/Right.
                    ((vel_.y > 0) ? kUp : kDown) : // Determine Up vs. Down.
                    ((vel_.x > 0) ? kRight : kDown); // Right vs. Left.
      path_.push_back(leg);
      pos_.x = 0;
      pos_.y = 0;
      pos_.theta = 0;
    }
  }

  // Determine whether we have hit a wall/edge and need to change our motion.
  if (navigating_) {
    // First, check for if we are about to hit a wall.
    if (AvgRangeError(dir_) >
        -0.02 /*Tune so that inertia doesn't make us hit the wall*/ ||
        cliff_.on_line((CliffDetector::RobotSide)dir_)) {
      Serial.print("\nOh no! Running into wall:\t");
      Serial.println(AvgRangeError(dir_));
      DriveDirection(tabledir(), power_);
    }
    // Check if we have reached the end of a wall and should uturn.
    else if (AvgRangeError(walldir()) < -0.10 /*Tune*/ && !uturn_) {
      Serial.print("\nPAST wall. Distance to wall:\t");
      Serial.println(AvgRangeError(walldir()));
      uturn_ = true;
      uturn_state_ = kForward;
      set_wall_follow(false);
      double cur_dist = ((int)dir_ % 2) ? pos_.x : pos_.y;
      DriveDist(0.4 + cur_dist /*tune*/, dir_, power_, false);
    }
    else if (AvgRangeError(walldir()) > -0.00 /*Tune*/ && uturn_) {
      uturn_ = false;
      set_wall_follow(true);
      DriveDirection(dir_, power_);
    }
    // Check if we are in a uturn and need to change direction.
    else if (uturn_ && drive_dist_ < 0) {
      switch (uturn_state_) {
        case kBack:
          uturn_ = false;
          set_wall_follow(true);
          DriveDirection(dir_, power_);
          break;
        case kSide:
          uturn_state_ = kBack;
          DriveDist(0.6/*tune*/, walldir(), power_, false);
          break;
        case kForward:
          uturn_state_ = kSide;
          DriveDist(0.5/*tune*/, walldir(), power_, false);
          break;
      }
    }
  }

  // Handle drive_dist stuff.
  if (drive_dist_ > 0) {
    Serial.print(pos_.x);
    Serial.print("\t");
    Serial.println(pos_.y);
    // Check if we are going up/down or right/left.
    if ((int)dir_ % 2) {  // side-to-side
      if (pos_.x > drive_dist_) drive_dist_done_ = true;
      else drive_dist_done_ = false;
    }
    else {
      if (pos_.y > drive_dist_) drive_dist_done_ = true;
      else drive_dist_done_ = false;
    }
  }
  if (drive_dist_done) Stop(!stop_drive_dist_);

  UpdateMotors();

  prev_time_ = time_;
}

// Performs a third-order fit to linearize from percent to 0 - 180 scale.
int Drivetrain::PercentToServo(int percent) {
  percent = (percent > 100) ? 100 : (percent < -100) ? -100 : percent;
  const int kMax = 140;
  const int kMin = 40;
  const int kStartDead = 78;
  const int kEndDead = 106;
  // Inputs 0 - ~600; outputs 0 - 80, inclusive.
  const double kc0 = (percent > 0) ? 82.06 : 106;
  const double kc1 = -0.1;
  const double kc2 = 2.68e-4;
  const double kc3 = -4.9e-7;
  double kc2_signed = (percent > 0) ? kc2 : -kc2;
  double x1 = percent * 6;
  double x2 = x1 * x1;
  double x3 = x2 * x1;
  double raw = kc0 + kc1 * x1 + kc2_signed * x2 + kc3 * x3;
  //if (percent < 0) raw = (180 - raw);

  if (percent == 0) raw = (kStartDead + kEndDead) / 2;
  return raw;
}

void Drivetrain::UpdateEncoders() {
  enc_[0] = fenc_.read() * kTicksToMeters;
  enc_[1] = lenc_.read() * kTicksToMeters;
  enc_[2] = benc_.read() * kTicksToMeters;
  enc_[3] = renc_.read() * kTicksToMeters;

  float dt = (time_ - prev_time_) * 1e-6;

  for (int i = 0; i < kNumMotors; i++) {
    enc_vel_[i] = (enc_[i] - prev_enc_[i]) / dt;
    prev_enc_[i] = enc_[i];
  }

  // Warning: If using single encoders, change code to use gyro to determine
  // which direction the robot is turning in.
  float upvel = (enc_vel_[kLeft] + enc_vel_[kRight]) / 2.0;
  float sidevel = (enc_vel_[kUp] + enc_vel_[kDown]) / 2.0;
  float turn = // Positive = CCW
      (enc_vel_[kRight] - enc_vel_[kLeft] + enc_vel_[kDown] - enc_vel_[kUp]) /
      (4.0 * kRobotRadius);
  vel_.x = sidevel;
  vel_.y = upvel;
  vel_.theta = turn;
  pos_.x += vel_.x * dt;
  pos_.y += vel_.y * dt;
  pos_.theta += vel_.theta;
  imu_.set_est_rate(vel_.theta);
  imu_.set_est_rate_weight(0);  // TODO: Tune.
  imu_.set_est_angle(vel_.theta);
  imu_.set_est_angle_weight(0);
#ifdef DEBUG
  Serial.print(pos_.x);
  Serial.print("\t");
  Serial.print(pos_.y);
  Serial.print("\t");
  Serial.print(fenc_.read());
  Serial.print("\t");
  Serial.print(lenc_.read());
  Serial.print("\t");
  Serial.print(benc_.read());
  Serial.print("\t");
  Serial.println(renc_.read());
#endif  // DEBUG
}

void Drivetrain::UpdateMotors() {
  double rate = imu_.get_rate();
  double angle = imu_.get_angle();
  double rate_error =
      rate - 0;  // Replace 0 with something else if we want to turn.
  double angle_error = -angle;
  if (angle_error > PI) angle_error -= 2 * PI;
  if (angle_error < -PI) angle_error += 2 * PI;
  double diffangle =
      constrain(kPangle * angle_error, -30, 30);  // TODO: Expand to full PID, or just PD.
  double diffrate =
      kPrate * rate_error;  // TODO: Expand to full PID, or just PD.
  // TODO: Check that left/right are correct.
  double rightpower = power_ * 100 + diffrate - diffangle;
  double leftpower  = power_ * 100 - diffrate + diffangle;
#ifdef DEBUG
  Serial.print(angle_error);
  Serial.print("\t");
  Serial.println(power_);
#endif  // DEBUG

  // Handle wall following.
  // Always use line following sensor to right of current direction.
  // RangeError() returns negative if we are too far away.
  float rangepower =  - kPrange * RangeError(walldir()) * 100;
  if (cliff_.on_line((CliffDetector::RobotSide)walldir())) rangepower = -50;
  rangepower = wall_follow_ ? rangepower : 0;
  lcd_.clear();
  lcd_.print(rangepower);
  if (stopping_) {
    rightpower = 0;
    leftpower = 0;
  }
  switch (dir_) {
    // TODO: Confirm that these cases are correct.
    // TODO: Check sign on rangepower stuff.
    case kUp:
      WriteMotors(rangepower, leftpower, rangepower, rightpower);
      break;
    case kLeft:
      WriteMotors(-rightpower, rangepower, -leftpower, rangepower);
      break;
    case kDown:
      WriteMotors(-rangepower, -rightpower, -rangepower, -leftpower);
      break;
    case kRight:
      WriteMotors(leftpower, -rangepower, rightpower, -rangepower);
      break;
    case kStop:
      WriteMotors(0, 0, 0, 0);
      break;
  }
}

void Drivetrain::Stop(bool resume) {
  drive_dist_ = -1;
  if (!resume) dir_ = kStop;
  stopping_ = true;
}

void Drivetrain::DriveDirection(Direction heading, float power) {
  Serial.print("Driving Power: ");
  Serial.print(power);
  Serial.print("\tHeading: ");
  Serial.println(heading);
#ifdef DEBUG
#endif  // DEBUG
  drive_dist_ = -1;
  power_ = power;
  if (heading == dir_) return;
  Stop(true); // Stop the robot before heading in a different direction.
  dir_ = heading;
}

void Drivetrain::DriveDist(float distance, Direction heading, float power, bool stop) {
  drive_dist_done_ = false;
  stop_drive_dist_ = stop;
  Serial.println("Driving Dist.");
  DriveDirection(heading, power);
  drive_dist_ = distance;
}
