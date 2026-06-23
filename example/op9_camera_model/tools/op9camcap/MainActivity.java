package com.op9.camcap;
import android.app.Activity;
import android.os.*;
import android.hardware.camera2.*;
import android.hardware.camera2.params.*;
import android.media.ImageReader;
import android.graphics.ImageFormat;
import android.util.Log;
import java.util.*;

public class MainActivity extends Activity {
  static final String TAG = "OP9CAM";
  CameraManager cm; CameraDevice dev; ImageReader reader; Handler h; int frames=0;

  public void onCreate(Bundle b) {
    super.onCreate(b);
    HandlerThread t = new HandlerThread("cam"); t.start(); h = new Handler(t.getLooper());
    cm = (CameraManager) getSystemService(CAMERA_SERVICE);
    try {
      for (String id : cm.getCameraIdList()) {
        CameraCharacteristics cc = cm.getCameraCharacteristics(id);
        Integer facing = cc.get(CameraCharacteristics.LENS_FACING);
        Set<String> phys = cc.getPhysicalCameraIds();
        Log.e(TAG, "CAMERA id=" + id + " facing=" + facing + " physicalIds=" + phys);
        for (String p : phys) {
          try { CameraCharacteristics pc = cm.getCameraCharacteristics(p);
            android.util.Size[] sz = pc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                 .getOutputSizes(ImageFormat.YUV_420_888);
            Log.e(TAG, "  phys " + p + " facing=" + pc.get(CameraCharacteristics.LENS_FACING)
                 + " maxYUV=" + (sz!=null&&sz.length>0?sz[0]:"?"));
          } catch (Exception e) { Log.e(TAG, "  phys " + p + " char err " + e); }
        }
      }
    } catch (Exception e) { Log.e(TAG, "enum err", e); }

    final String logical = getIntent().getStringExtra("logical") != null ? getIntent().getStringExtra("logical") : "0";
    final String phys = getIntent().getStringExtra("phys"); // physical id to stream, or null
    final int w = getIntent().getIntExtra("w", 4096), hh = getIntent().getIntExtra("h", 3072);
    Log.e(TAG, "OPENING logical=" + logical + " phys=" + phys + " " + w + "x" + hh);
    reader = ImageReader.newInstance(w, hh, ImageFormat.YUV_420_888, 3);
    reader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
      public void onImageAvailable(ImageReader r) {
        android.media.Image im = r.acquireLatestImage();
        if (im != null) { if (frames++ < 3) Log.e(TAG, "FRAME " + im.getWidth() + "x" + im.getHeight()); im.close(); }
      }
    }, h);
    final java.util.concurrent.Executor exec = new java.util.concurrent.Executor() {
      public void execute(Runnable cmd) { cmd.run(); }
    };
    try {
      cm.openCamera(logical, new CameraDevice.StateCallback() {
        public void onOpened(CameraDevice d) {
          dev = d; Log.e(TAG, "onOpened " + d.getId());
          try {
            OutputConfiguration oc = new OutputConfiguration(reader.getSurface());
            if (phys != null) oc.setPhysicalCameraId(phys);
            SessionConfiguration sc = new SessionConfiguration(SessionConfiguration.SESSION_REGULAR,
              Collections.singletonList(oc), exec, new CameraCaptureSession.StateCallback() {
                public void onConfigured(CameraCaptureSession s) {
                  Log.e(TAG, "onConfigured");
                  try {
                    CaptureRequest.Builder rb = d.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                    rb.addTarget(reader.getSurface());
                    float zoom = getIntent().getFloatExtra("zoom", 1.0f);
                    try {
                      CameraCharacteristics cc0 = cm.getCameraCharacteristics(logical);
                      android.util.Range<Float> zr = cc0.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
                      Log.e(TAG, "ZOOM_RANGE=" + zr + " setting zoom=" + zoom);
                      rb.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom);
                    } catch (Exception e) { Log.e(TAG, "zoom set err " + e); }
                    // [op9] manual exposure/gain perturbation: aeoff -> CONTROL_AE_MODE_OFF +
                    // SENSOR_EXPOSURE_TIME (ns) + SENSOR_SENSITIVITY (ISO) so we can correlate
                    // sensor registers (0x0202/3 exposure, 0x0204/5 gain) to the request values.
                    if (getIntent().getStringExtra("aeoff") != null) {
                      rb.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
                      long expns = getIntent().getLongExtra("exp", 10000000L);
                      int iso = getIntent().getIntExtra("iso", 400);
                      rb.set(CaptureRequest.SENSOR_EXPOSURE_TIME, expns);
                      rb.set(CaptureRequest.SENSOR_SENSITIVITY, iso);
                      Log.e(TAG, "AE_OFF exp_ns=" + expns + " iso=" + iso);
                    }
                    s.setRepeatingRequest(rb.build(), null, h);
                    Log.e(TAG, "STREAMING phys=" + phys + " zoom=" + zoom);
                  } catch (Exception e) { Log.e(TAG, "req err", e); }
                }
                public void onConfigureFailed(CameraCaptureSession s) { Log.e(TAG, "onConfigureFailed"); }
              });
            d.createCaptureSession(sc);
          } catch (Exception e) { Log.e(TAG, "sess err", e); }
        }
        public void onError(CameraDevice d, int e) { Log.e(TAG, "onError " + e); }
        public void onDisconnected(CameraDevice d) { Log.e(TAG, "onDisconnected"); }
      }, h);
    } catch (Exception e) { Log.e(TAG, "open err", e); }
  }
}
