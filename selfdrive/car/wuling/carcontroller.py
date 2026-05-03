from cereal import car
from common.conversions import Conversions as CV
from common.numpy_fast import interp
from common.realtime import DT_CTRL
from opendbc.can.packer import CANPacker
from selfdrive.car import apply_driver_steer_torque_limits, apply_std_steer_angle_limits
from selfdrive.car.wuling import wulingcan
from selfdrive.car.wuling.values import DBC, CanBus, PREGLOBAL_CARS, CarControllerParams
from common.numpy_fast import clip

LongCtrlState = car.CarControl.Actuators.LongControlState

class CarController:
  def __init__(self, dbc_name, CP, VM):
    self.CP = CP
    
    self.lka_steering_cmd_counter_last = -1
    self.apply_angle_last = 0
    
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
      brake_value = int(round(interp(actuators.accel, P.BRAKE_LOOKUP_BP, P.BRAKE_LOOKUP_V)))
    else:
      apply_stop = 0
      apply_gas = 1696
      brake_value = 255
    
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
      can_sends.append(wulingcan.create_brake_command(self.packer_pt, apply_stop, idx, brake_value))
      can_sends.append(wulingcan.create_gas_command(self.packer_pt, idx, acc_enabled, apply_start, apply_gas))

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