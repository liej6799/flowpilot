from cereal import car
from common.conversions import Conversions as CV
from common.numpy_fast import interp
from common.realtime import DT_CTRL
from opendbc.can.packer import CANPacker
from selfdrive.car import apply_driver_steer_torque_limits, apply_std_steer_angle_limits
from selfdrive.car.wuling import wulingcan
from selfdrive.car.wuling.values import DBC, CanBus, PREGLOBAL_CARS, CarControllerParams
from common.numpy_fast import clip
from common.logger import sLogger
LongCtrlState = car.CarControl.Actuators.LongControlState

# Honda-style brake hysteresis thresholds (normalized 0-1 brake magnitude).
# Prevents the brake from chattering on/off due to small accel oscillations,
# which is what made longitudinal braking feel aggressive. Every direct-control
# brand (honda/gm/toyota) applies this; wuling was missing it.
BRAKE_HYST_ON = 0.02    # brake magnitude must exceed this to ENGAGE
BRAKE_HYST_OFF = 0.005  # brake magnitude must drop below this to RELEASE
BRAKE_HYST_GAP = 0.01   # ignore small oscillations within this band


def brake_hysteresis(brake_norm, braking, brake_steady):
  if (brake_norm < BRAKE_HYST_ON and not braking) or brake_norm < BRAKE_HYST_OFF:
    brake_norm = 0.0
  braking = brake_norm > 0.
  if brake_norm == 0.:
    brake_steady = 0.
  elif brake_norm > brake_steady + BRAKE_HYST_GAP:
    brake_steady = brake_norm - BRAKE_HYST_GAP
  elif brake_norm < brake_steady - BRAKE_HYST_GAP:
    brake_steady = brake_norm + BRAKE_HYST_GAP
  brake_norm = brake_steady
  return brake_norm, braking, brake_steady


class CarController:
  def __init__(self, dbc_name, CP, VM):
    self.CP = CP

    self.lka_steering_cmd_counter_last = -1
    self.apply_angle_last = 0

    self.braking = False
    self.brake_steady = 0.0

    self.frame = 0

    self.params = CarControllerParams(self.CP)

    self.packer_pt = CANPacker(DBC[self.CP.carFingerprint]['pt'])

  def update(self, CC, CS, now_nanos):
    P = self.params

    # Send CAN commands.
    can_sends = []
    actuators = CC.actuators
    hud_control = CC.hudControl
    hud_alert = hud_control.visualAlert
    hud_v_cruise = hud_control.setSpeed


    if CC.latActive:
      apply_angle = apply_std_steer_angle_limits(actuators.steeringAngleDeg, self.apply_angle_last, CS.out.vEgo, CarControllerParams)
    else:
      apply_angle = CS.out.steeringAngleDeg

    if CC.longActive:
      apply_gas = int(round(interp(actuators.accel, P.GAS_LOOKUP_BP, P.GAS_LOOKUP_V)))
      # brake magnitude normalized 0-1 (0 at accel>=0, 1 at ACCEL_MIN), then
      # pass through Honda-style hysteresis so small accel noise doesn't chatter
      # the brake on/off. Only send a brake value once it latches on.
      brake_norm = clip(-actuators.accel / abs(P.ACCEL_MIN), 0.0, 1.0)
      brake_norm, self.braking, self.brake_steady = brake_hysteresis(brake_norm, self.braking, self.brake_steady)
      if self.braking:
        brake_value = int(round(interp(brake_norm, [0.0, 1.0], [1, P.MAX_BRAKE])))
      else:
        brake_value = 1
    else:
      apply_stop = 0
      apply_gas = 1696
      brake_value = 255
      self.braking = False
      self.brake_steady = 0.0

    self.apply_angle_last = apply_angle

    if CS.lka_steering_cmd_counter != self.lka_steering_cmd_counter_last:
      self.lka_steering_cmd_counter_last = CS.lka_steering_cmd_counter
    elif (self.frame % P.STEER_STEP) == 0:
      lkas_enabled = CC.latActive
      acc_enabled = CC.longActive
      apply_stop = actuators.longControlState == LongCtrlState.stopping
      apply_start = actuators.longControlState == LongCtrlState.starting

      idx = (self.frame/2) % 4


      can_sends.append(wulingcan.create_steering_control(self.packer_pt, apply_angle, idx, lkas_enabled))
      # brake enable follows the hysteresis latch (computed above), not raw accel,
      # so the brake CAN only engages for sustained decel commands.
      can_sends.append(wulingcan.create_brake_command(self.packer_pt, self.braking, idx, brake_value))
      can_sends.append(wulingcan.create_gas_command(self.packer_pt, idx, acc_enabled, apply_gas))

    sLogger.Send("0all set")
    if (self.frame % P.HUD_STEP) == 0:
      set_speed = int(round(hud_v_cruise * CV.MS_TO_KPH))
      idx = (self.frame/5) % 4
      cc_enabled = CC.enabled
      can_sends.append(wulingcan.create_acc_hud_control(self.packer_pt, idx, cc_enabled, set_speed))
      can_sends.append(wulingcan.create_lkas_hud_control(self.packer_pt, idx, cc_enabled))


    new_actuators = actuators.copy()
    new_actuators.steeringAngleDeg = apply_angle
    new_actuators.brake = brake_value
    new_actuators.gas = apply_gas

    self.frame += 1
    return new_actuators, can_sends