# op9camcap — minimal Camera2 perturbation/debug app

A tiny Camera2 app to open any camera id (incl. the ultrawide) and stream it with a chosen
resolution / zoom / exposure / gain — used to (a) capture a sensor's real init from the stock HAL
and (b) reverse-engineer registers by perturbing one setting and diffing the CCI writes.

## Build (server, Android SDK at /opt/android-sdk)
```sh
SDK=/opt/android-sdk; AJAR=$SDK/platforms/android-34/android.jar; BT=$SDK/build-tools/34.0.0
javac -source 8 -target 8 -bootclasspath $AJAR -d classes src/com/op9/camcap/MainActivity.java
$SDK/build-tools/29.0.2/d8 --min-api 21 --output . classes/com/op9/camcap/*.class
aapt package -f -M AndroidManifest.xml -I $AJAR -F app-unsigned.apk
aapt add app-unsigned.apk classes.dex
$BT/zipalign -f 4 app-unsigned.apk op9camcap.apk
$BT/apksigner sign --ks debug.ks --ks-pass pass:android op9camcap.apk
```
Install: `pm install -r -g op9camcap.apk` (needs the **stock** camera.ko so the HAL exposes all cams).

## Usage
```sh
am start -n com.op9.camcap/.MainActivity \
  --es logical 2           # 0=IMX689 main, 1=front, 2=IMX766 ultrawide
  --ei w 4096 --ei h 3072  # ImageReader size (resolution)
  --ef zoom 2.0            # CONTROL_ZOOM_RATIO
  --es aeoff 1 --el exp 30000000 --ei iso 1600   # manual exposure (ns) + ISO (AE off)
```
Then `dmesg | grep ALLREG` (requires the ALLREG-instrumented camera.ko) gives the CCI register
writes. Diff two captures differing by one setting to attribute registers. See
`../../docs/SENSOR-REGISTERS.md` for the decoded result and `../../sensors/captured/perturbation/`
for the raw captures.
