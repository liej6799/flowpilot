package com.op9.camcap;
import android.app.*;
import android.content.Context;
import android.content.Intent;
import android.os.*;
import android.hardware.camera2.*;
import android.hardware.camera2.params.*;
import android.media.ImageReader;
import android.graphics.ImageFormat;
import android.util.Log;
import java.io.*;
import java.nio.ByteBuffer;
import java.util.*;

// Headless NV12 capture: runs the Camera2 -> ImageReader(YUV_420_888) pipeline from a
// foreground Service so it needs NO display/Window surface (the device's WindowManager
// surface-alloc path is wedged, so an Activity can't launch). The ISP still produces the
// processed NV12 buffer; we write it to disk and log the plane layout.
public class CamService extends Service {
  static final String TAG = "OP9CAM";
  CameraManager cm; CameraDevice dev; ImageReader reader; Handler h; int frames=0;
  int dumpCount=0, dumpSkip=0; File outDir;

  public IBinder onBind(Intent i) { return null; }

  public int onStartCommand(final Intent intent, int flags, int startId) {
    try {
      NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
      NotificationChannel ch = new NotificationChannel("cap", "cap", NotificationManager.IMPORTANCE_LOW);
      nm.createNotificationChannel(ch);
      Notification n = new Notification.Builder(this, "cap")
          .setContentTitle("OP9CamCap").setSmallIcon(android.R.drawable.ic_menu_camera).build();
      startForeground(1, n);
    } catch (Throwable e) { Log.e(TAG, "fg err", e); }

    HandlerThread t = new HandlerThread("cam"); t.start(); h = new Handler(t.getLooper());
    cm = (CameraManager) getSystemService(CAMERA_SERVICE);
    outDir = getFilesDir();
    final String logical = intent.getStringExtra("logical") != null ? intent.getStringExtra("logical") : "0";
    final String phys = intent.getStringExtra("phys");
    final int w = intent.getIntExtra("w", 4096), hh = intent.getIntExtra("h", 3072);
    dumpCount = intent.getIntExtra("dump", 2);
    dumpSkip  = intent.getIntExtra("skip", 40);
    try {
      for (String id : cm.getCameraIdList()) {
        CameraCharacteristics cc = cm.getCameraCharacteristics(id);
        Log.e(TAG, "CAMERA id=" + id + " facing=" + cc.get(CameraCharacteristics.LENS_FACING)
            + " physicalIds=" + cc.getPhysicalCameraIds());
      }
    } catch (Exception e) { Log.e(TAG, "enum err", e); }
    Log.e(TAG, "OPENING(svc) logical=" + logical + " phys=" + phys + " " + w + "x" + hh + " dump=" + dumpCount + " skip=" + dumpSkip);

    reader = ImageReader.newInstance(w, hh, ImageFormat.YUV_420_888, 4);
    reader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
      public void onImageAvailable(ImageReader r) {
        android.media.Image im = r.acquireLatestImage();
        if (im == null) return;
        try {
          int fn = frames++;
          if (fn < 3) Log.e(TAG, "FRAME " + im.getWidth() + "x" + im.getHeight() + " fmt=" + im.getFormat());
          if (dumpCount > 0 && fn >= dumpSkip && fn < dumpSkip + dumpCount) {
            try { dumpNV12(im, fn - dumpSkip); } catch (Throwable e) { Log.e(TAG, "dump err", e); }
          }
        } finally { im.close(); }
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
                    if (intent.getStringExtra("aeoff") != null) {
                      rb.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
                      rb.set(CaptureRequest.SENSOR_EXPOSURE_TIME, intent.getLongExtra("exp", 10000000L));
                      rb.set(CaptureRequest.SENSOR_SENSITIVITY, intent.getIntExtra("iso", 400));
                    }
                    s.setRepeatingRequest(rb.build(), null, h);
                    Log.e(TAG, "STREAMING(svc) phys=" + phys);
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
    return START_NOT_STICKY;
  }

  void dumpNV12(android.media.Image im, int idx) throws IOException {
    int w = im.getWidth(), h = im.getHeight();
    android.media.Image.Plane[] p = im.getPlanes();
    ByteBuffer yb = p[0].getBuffer(), ub = p[1].getBuffer(), vb = p[2].getBuffer();
    int yrs = p[0].getRowStride(), yps = p[0].getPixelStride();
    int urs = p[1].getRowStride(), ups = p[1].getPixelStride();
    int vrs = p[2].getRowStride(), vps = p[2].getPixelStride();
    Log.e(TAG, "PLANES " + w + "x" + h + " Y[rs=" + yrs + ",ps=" + yps + "] U[rs=" + urs + ",ps=" + ups
        + "] V[rs=" + vrs + ",ps=" + vps + "] Ucap=" + ub.capacity() + " Vcap=" + vb.capacity()
        + " semiplanar=" + (ups == 2 && vps == 2));
    File f = new File(outDir, "nv12_" + w + "x" + h + "_" + idx + ".yuv");
    BufferedOutputStream os = new BufferedOutputStream(new FileOutputStream(f), 1 << 20);
    byte[] row = new byte[w];
    for (int y = 0; y < h; y++) {
      int off = y * yrs;
      if (yps == 1) { yb.position(off); yb.get(row, 0, w); }
      else { for (int x = 0; x < w; x++) row[x] = yb.get(off + x * yps); }
      os.write(row, 0, w);
    }
    byte[] uv = new byte[w];
    for (int y = 0; y < h / 2; y++) {
      int uo = y * urs, vo = y * vrs, k = 0;
      for (int x = 0; x < w / 2; x++) { uv[k++] = ub.get(uo + x * ups); uv[k++] = vb.get(vo + x * vps); }
      os.write(uv, 0, w);
    }
    os.flush(); os.close();
    Log.e(TAG, "DUMPED " + f.getAbsolutePath() + " size=" + f.length() + " expect=" + (w * h * 3 / 2));
  }
}
