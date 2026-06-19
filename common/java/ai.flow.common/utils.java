package ai.flow.common;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.IOException;
import java.util.LinkedList;
import java.util.Queue;

public class utils {

    public enum USE_MODEL_RUNNER {
        ONNX, // DOESNT WORK
        SNPE, // Nicki Minaj Model
        TNN, // DOESNT WORK
        THNEED, // Latest
        EXTERNAL_TINYGRAD // DOESNT WORK
    }
    public static boolean F2 = false, NLPModel = false, LAModel = true;
    // TESTING_MODE: frontend-only model testing. Forces always-onroad and runs
    // without the backend (no panda / controlsd / plannerd / modelparsed). The
    // app runs the vision model on camera frames and parses modelRaw->modelV2
    // in Java so the UI can render model outputs with no backend. Lives in
    // :common so both :ui and :modeld can read it.
    public static boolean TESTING_MODE = true;
    // THNEED (Adreno OpenCL/GPU) — same runner the working alpha5 build uses.
    // The earlier "GPU crash" was actually a stale/wrong supercombo.thneed on
    // /sdcard (md5 86357aad, no-nav) mismatching the nav-aware app code; the
    // correct model (93e42c6e, matches alpha5) must be on /sdcard.
    public static USE_MODEL_RUNNER Runner = USE_MODEL_RUNNER.THNEED;
    public static boolean getBoolEnvVar(String key) {
        String val = System.getenv(key);
        boolean ret = false;
        if (val != null) {
            if (val.equals("1"))
                ret = true;
        }
        return ret;
    }

    public static double secSinceBoot() {
        return System.nanoTime() / 1e9;
    }

    public static double milliSinceBoot() {
        return System.nanoTime() / 1e6;
    }

    public static double nanoSinceBoot() {
        return System.nanoTime();
    }

    public static double numElements(int[] shape){
        double ret = 1;
        for (int i:shape)
            ret *= i;
        return ret;
    }

    public static String readFile(String fileName)
    {
        BufferedReader br = null;
        try
        {
            br = new BufferedReader(new FileReader(fileName));
            StringBuilder sb = new StringBuilder();
            String line = null;
            while (true)
            {
                line = br.readLine();
                if (line == null)
                {
                    break;
                }
                sb.append(line+"\n");
            }
            return sb.toString();
        }
        catch (IOException e)
        {
            e.printStackTrace();
            return "";
        }
        finally
        {
            if (br != null)
            {
                try
                {
                    br.close();
                }
                catch (IOException ex)
                {
                    ex.printStackTrace();
                }
            }
        }
    }
}
