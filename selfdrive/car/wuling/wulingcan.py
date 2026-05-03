import copy
from cereal import car
from common.numpy_fast import clip
from selfdrive.car import make_can_msg

VisualAlert = car.CarControl.HUDControl.VisualAlert

def wuling_checksum(dat):
  return sum(dat) & 0xFF

def create_steering_control(packer, apply_steer, idx, steer_req):

  values = {
        # "STEER_TORQUE_CMD": -apply_steer,
        # "SET_ME_X0": 0x00,
        # "STEER_REQUEST": steer_req,
        
        "ACTIVE": 0x64, # Always active 
        "ACTIVE_2": 0x64 if steer_req else 0,
        "STEER_LOCK": 1 if steer_req else 0,
        "COUNTER": idx,
        "STEER_ANGLE_CMD": -apply_steer
  }
  
  values["COUNTER"] = (values["COUNTER"] + 1) % 0x11
  
  dat = packer.make_can_msg("STEERING_LKA", 0, values)[2]

  crc = wuling_checksum(dat[:-1])
  values["CHECKSUM"] = crc

  return packer.make_can_msg("STEERING_LKA", 0, values)

def create_gas_command(packer, idx, acc_req, apply_start, throttle):
  values = {
    "ALWAYS_ON_1": 5,
    "ALWAYS_ON_2": 0x78,
    "ACC_ACTIVE_1": 32 if acc_req else 0,
    "ACC_ACTIVE_2": 1 if acc_req else 0,
    
    "GAS_ACTIVE_2": 1 if acc_req and throttle != 1696 else 0, # change this later
    "INV_GAS_ACTIVE_2": 1 if acc_req and throttle == 1696 else 0, #change this later

    "COUNTER": idx,
    "GAS_ACTIVE": 4 if acc_req and throttle != 1696 else 0, # need to check is it based on throttle.
    "GAS_THROTTLE": throttle # need to change later
  }
  
  values["COUNTER"] = (values["COUNTER"] + 1) % 0x11
  
  dat = packer.make_can_msg("AccStatus", 0, values)[2]

  crc = wuling_checksum(dat[:-1])
  values["CHECKSUM"] = crc

  return packer.make_can_msg("AccStatus", 0, values)


def create_brake_command(packer, apply_brake, idx, brake_value):
  print(brake_value)
  values = {
    "BRAKE_CMD": 15 if apply_brake < 200 else 0,
    "BRAKE_CMD_2": 16 if apply_brake < 200 else 0,
    "COUNTER": idx, # 0 - 255
    "BRAKE_VAL": brake_value,
  }
  
  values["COUNTER"] = (values["COUNTER"] + 1) % 0x11
  
  dat = packer.make_can_msg("BrakeCMD", 0, values)[2]

  crc = wuling_checksum(dat[:-1])
  values["CHECKSUM"] = crc

  return packer.make_can_msg("BrakeCMD", 0, values)


def create_acc_hud_control(packer, idx, acc_req, target_speed_kph):
  target_speed = min(target_speed_kph, 255)
  values = {
    "ACCSpeedSetpoint": target_speed,
    "ALWAYS_208": 208,
    "ALWAYS_6": 6, 
    "ACC_ENABLED_1": 1 if acc_req else 0, 
    "ACC_ENABLED_2": 32 if acc_req else 0, 
    "ACC_ENABLED_3": 1 if acc_req else 0, 
    "COUNTER": idx,
  }
  
  values["COUNTER"] = (values["COUNTER"] + 1) % 0x11
  
  dat = packer.make_can_msg("ASCMActiveCruiseControlStatus", 0, values)[2]

  crc = wuling_checksum(dat[:-1])
  values["CHECKSUM"] = crc

  return packer.make_can_msg("ASCMActiveCruiseControlStatus", 0, values)

def create_lkas_hud_control(packer, idx, acc_req):

  values = {
    "ALWAYS_108": 108,
    "LKA_ACTIVE": 1 if acc_req else 0, 
    "COUNTER": idx,
  }
  
  values["COUNTER"] = (values["COUNTER"] + 1) % 0x11
  
  dat = packer.make_can_msg("LkasHud", 0, values)[2]

  crc = wuling_checksum(dat[:-1])
  values["CHECKSUM"] = crc

  return packer.make_can_msg("LkasHud", 0, values)

  