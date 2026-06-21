#!/system/bin/sh
# Stop/start the Android camera HAL so the proot can own the Spectra ISP.
# Run from the ANDROID side (termux: sudo sh scripts/hal.sh stop), NOT the proot.
#
#   hal.sh stop    -> releases /dev/video0 (cam-req-mgr) from the camera provider
#   hal.sh start   -> gives the camera back to Android (flowpilot app, etc.)
#   hal.sh status  -> show whether the provider holds the camera

SVC=vendor.camera-provider-2-4

case "$1" in
  stop)
    setprop ctl.stop $SVC
    sleep 2
    n=$(ps -A | grep -c camera.provider)
    echo "camera HAL processes after stop: $n (expect 0)"
    ;;
  start)
    setprop ctl.start $SVC
    sleep 2
    n=$(ps -A | grep -c camera.provider)
    echo "camera HAL processes after start: $n (expect 1)"
    ;;
  status)
    n=$(ps -A | grep -c camera.provider)
    echo "camera HAL processes: $n"
    ;;
  *)
    echo "usage: hal.sh {stop|start|status}"
    exit 1
    ;;
esac
