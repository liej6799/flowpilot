from collections import defaultdict
from dataclasses import dataclass
from enum import Enum
from typing import Dict, List, Union

from cereal import car
from selfdrive.car import AngleRateLimit, dbc_dict
from selfdrive.car.docs_definitions import CarFootnote, CarInfo, Column
Ecu = car.CarParams.Ecu


class CarControllerParams:

  STEER_STEP = 2  # control frames per command
  BUTTONS_STEP = 2
  HUD_STEP = 5


  STEER_MAX = 350  # Safety limit, not LKA max. Trucks use 600.
  STEER_DELTA_UP = 3      # 3 is stock. 100 is fine. 200 is too much it seems
  STEER_DELTA_DOWN = 3    # no faults on the way down it seems
  STEER_ERROR_MAX = 80
  MIN_STEER_SPEED = 3.  # m/s

  STEER_DRIVER_ALLOWANCE = 80
  STEER_DRIVER_MULTIPLIER = 3    # weight driver torque heavily
  STEER_DRIVER_FACTOR = 1        # from dbc
  NEAR_STOP_BRAKE_PHASE = 0.5  # m/s
  INACTIVE_STEER_STEP = 10  # Inactive control frames per command (10hz)

  # Heartbeat for dash "Service Adaptive Cruise" and "Service Front Camera"
  ADAS_KEEPALIVE_STEP = 100
  CAMERA_KEEPALIVE_STEP = 100
  STEER_THRESHOLD = 60
  HUD_MULTIPLIER = 0.685

  ANGLE_RATE_LIMIT_UP = AngleRateLimit(speed_bp=[0., 5., 15.], angle_v=[5., .8, .15])
  ANGLE_RATE_LIMIT_DOWN = AngleRateLimit(speed_bp=[0., 5., 15.], angle_v=[5., 3.5, 0.4])

  # Allow small margin below -3.5 m/s^2 from ISO 15622:2018 since we
  # perform the closed loop control, and might need some
  # to apply some more braking if we're on a downhill slope.
  # Our controller should still keep the 2 second average above
  # -3.5 m/s^2 as per planner limits
  ACCEL_MAX = 2.  # m/s^2
  ACCEL_MIN = -3.5  # m/s^2

  # ACCEL_LOOKUP_BP = [0, 2]
  # ACCEL_LOOKUP_V = [1696, 1900]

  ACCEL_LOOKUP_BP = [0., 0., 2]
  ACCEL_LOOKUP_V = [0, 0, 100]

  def __init__(self, CP):
    # Gas/brake lookups
    self.ZERO_GAS = 1730  # Coasting
    self.MAX_BRAKE = 255  # ~ -4.0 m/s^2 with regen

    self.MAX_GAS = 1954  # Safety limit, not ACC max. Stock ACC >4096 from standstill.
    self.MAX_ACC_REGEN = 1696  # Max ACC regen is slightly less than max paddle regen
    self.INACTIVE_REGEN = 1404
    # ICE has much less engine braking force compared to regen in EVs,
    # lower threshold removes some braking deadzone
    max_regen_acceleration = 0.

    self.GAS_LOOKUP_BP = [0., 0., self.ACCEL_MAX]
    self.GAS_LOOKUP_V = [self.MAX_ACC_REGEN, self.ZERO_GAS, self.MAX_GAS]

    self.BRAKE_LOOKUP_BP = [self.ACCEL_MIN, max_regen_acceleration]
    self.BRAKE_LOOKUP_V = [1, self.MAX_BRAKE]

    # assert(CP.lateralParams.torqueBP[0] == 0)
    # assert(CP.lateralParams.torqueBP[0] == 0)
    # self.STEER_LOOKUP_BP = [v * -1 for v in CP.lateralParams.torqueBP][1:][::-1] + list(CP.lateralParams.torqueBP)
    # self.STEER_LOOKUP_V = [v * -1 for v in CP.lateralParams.torqueV][1:][::-1] + list(CP.lateralParams.torqueV)


class CAR:
 ALMAS_RS_PRO = "WULING ALMAZ RS PRO 2022"
 ALVEZ = "WULING ALVEZ"


class Footnote(Enum):
  OBD_II = CarFootnote(
    'Wuling Almaz RS WITH Acc and LKAS',
    Column.MODEL)


@dataclass
class GMCarInfo(CarInfo):
  package: str = "Adaptive Cruise Control (ACC)"



CAR_INFO: Dict[str, Union[GMCarInfo, List[GMCarInfo]]] = {
  CAR.ALMAS_RS_PRO: GMCarInfo("Wuling Almaz RS Pro 2022"),
  CAR.ALVEZ: GMCarInfo("Wuling Alvez"),
}

class CruiseButtons:
  INIT = 0
  NONE = 0
  UNPRESS = 0
  GAP_DOWN = 1
  GAP_UP = 2
  DECEL_SET = 4
  RES_ACCEL = 8
  MAIN = 16
  CANCEL = 32
  TJA = 32

class AccState:
  OFF = 0
  ACTIVE = 1
  FAULTED = 3
  STANDSTILL = 4

class CanBus:
  POWERTRAIN = 0
  OBSTACLE = 1
  CAMERA = 2
  CHASSIS = 2
  SW_GMLAN = 3
  LOOPBACK = 128
  DROPPED = 192

FINGERPRINTS = {
  CAR.ALMAS_RS_PRO: [{
    193: 8, 197: 8, 201: 8, 225: 8, 288: 5, 296: 8, 298: 8, 320: 4, 381: 8, 401: 8, 404: 8, 413: 8, 451: 8, 454: 8, 481: 8, 485: 8, 489: 8, 497: 8, 501: 8, 549: 8, 560: 8, 608: 8, 611: 8, 617: 8, 840: 6, 842: 6, 844: 6, 846: 6, 880: 8, 883: 8, 996: 8, 997: 8, 1041: 8, 1043: 8, 1045: 8, 1047: 8, 1053: 8, 1065: 8, 1217: 8, 1225: 8, 1341: 8, 1381: 8, 1406: 8, 1417: 8, 1538: 8, 1541: 8, 1543: 8, 1552: 8, 1569: 8
  }],
  CAR.ALVEZ: [{
    193: 8, 197: 8, 201: 8, 225: 8, 258: 8, 288: 5, 296: 8, 298: 8, 320: 4, 381: 8, 401: 8, 411: 8, 413: 8, 451: 8, 454: 8, 481: 8, 485: 8, 489: 8, 497: 8, 501: 8, 549: 8, 560: 8, 563: 8, 608: 8, 611: 8, 617: 8, 840: 6, 842: 6, 844: 6, 846: 6, 880: 8, 883: 8, 996: 8, 997: 8, 1041: 8, 1043: 8, 1045: 8, 1047: 8, 1053: 8, 1065: 8, 1217: 8, 1225: 8, 1341: 8, 1381: 8, 1406: 8, 1417: 8, 1538: 8, 1541: 8, 1552: 8, 1569: 8
  }]
}

FW_VERSIONS = {
   CAR.ALMAS_RS_PRO: {},
   CAR.ALVEZ: {}
}
DBC = {
  CAR.ALMAS_RS_PRO: dbc_dict('wuling_almazrs_generated', None),
  CAR.ALVEZ: dbc_dict('wuling_almazrs_generated', None),
}

PREGLOBAL_CARS = []