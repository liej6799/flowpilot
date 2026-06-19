package ai.flow.modeld;

import ai.flow.common.SizeMapArrayPool;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

import static ai.flow.modeld.CommonModelF3.*;

// F3 (LA-model) parser: converts the raw THNEED/SNPE model output into
// ParsedOutputs (-> modelV2), so the F3 path can publish modelV2 in-app
// without the backend modelparsed daemon. Mirrors Parser.java (the F2 parser)
// but uses the F3 output layout from CommonModelF3 / driving.h:
//   - NO StopLines block (leads -> meta directly)
//   - LA-model tail (wide_from_device_euler, temporal_pose, road_transform,
//     action) sits between pose and the feature buffer; we don't need those
//     for display, so we skip straight to the feature buffer at OUTPUT_SIZE.
public class ParserF3 {
    public ParsedOutputs parsed = new ParsedOutputs();
    public ArrayList<LeadDataV3> leads = parsed.leads;
    public MetaData metaDataOutput = parsed.metaData;

    public float[][] meta = parsed.meta;
    public float[][] pose = parsed.pose;
    public float[][] state = parsed.state;

    public float[] trans = parsed.trans;
    public float[] transStd = parsed.transStd;
    public float[] rot = parsed.rot;
    public float[] rotStd = parsed.rotStd;
    public float[] laneLineProbs = parsed.laneLineProbs;
    public float[] laneLineStds = parsed.laneLineStds;
    public ArrayList<ArrayList<float[]>> laneLines = parsed.laneLines;
    public ArrayList<ArrayList<float[]>> roadEdges = parsed.roadEdges;
    public float[] roadEdgeStds = parsed.roadEdgeStds;

    public Map<String, float[]> net_outputs = new HashMap<String, float[]>();
    public float[] plan_t_arr = new float[TRAJECTORY_SIZE];
    public float[] best_plan = new float[PLAN_MHP_GROUP_SIZE];
    public float stopSignProb;  // F3 has no stoplines; always 0

    public SizeMapArrayPool pool = new SizeMapArrayPool();

    public ParserF3(){
        net_outputs.put("lead", new float[SIZE_ModelOutputLeads]);
        net_outputs.put("meta", new float[SIZE_ModelOutputMeta]);
        net_outputs.put("pose", new float[SIZE_ModelOutputPose]);
        net_outputs.put("state", new float[TEMPORAL_SIZE]);
    }

    public void copyOfRange(float[] src, float[] dst, int start, int end){
        for (int i=start; i<end; i++){
            dst[i-start] = src[i];
        }
    }

    public float getMax(float[] x){
         float max = x[0];
         for (int i=1; i<x.length; i++){
            max = Math.max(max, x[i]);
         }
         return max;
    }

    public void softmax(float[] x, float[] output){
        float max = getMax(x);
        float sum = 0;
        for (int i=0; i<x.length; i++)
            sum += Math.exp(x[i] - max);
        for (int i=0; i<x.length; i++)
            output[i] = (float)Math.exp(x[i]) / sum;
    }

    public float sigmoid(float x){
        return 1.0f / (float)(1 + Math.exp(-x));
    }

    public void getBestPlan(float[] x, float[] output, int start) {
        int plan_mhp_max_idx = 0;
        for(int i=1; i < PLAN_MHP_N; i++)
            if (x[start + (i + 1)*(PLAN_MHP_GROUP_SIZE) - 1] > x[start + (plan_mhp_max_idx + 1)*(PLAN_MHP_GROUP_SIZE) - 1])
                plan_mhp_max_idx = i;
        copyOfRange(x, output, start + plan_mhp_max_idx*(PLAN_MHP_GROUP_SIZE), start + (plan_mhp_max_idx+1)*(PLAN_MHP_GROUP_SIZE));
    }

    public void fillXYZT(ArrayList<float[]>xyzt, float[] data, int columns, int column_offset, float[] plan_t_arr, boolean fill_std)
    {
        float[] x_arr = xyzt.get(0);
        float[] y_arr = xyzt.get(1);
        float[] z_arr = xyzt.get(2);
        float[] t_arr = xyzt.get(3);
        float[] xStd_arr = xyzt.get(4);
        float[] yStd_arr = xyzt.get(5);
        float[] zStd_arr = xyzt.get(6);

        for(int i = 0; i < TRAJECTORY_SIZE; i++)
        {
            if (column_offset >= 0)
            {
                t_arr[i] = T_IDXS[i];
                x_arr[i] = data[i * columns + column_offset];
                if (fill_std)
                    xStd_arr[i] = data[columns * (TRAJECTORY_SIZE + i) + column_offset];
            }
            else
            {
                t_arr[i] = plan_t_arr[i];
                x_arr[i] = X_IDXS[i];
                xStd_arr[i] = Float.NaN;
            }
            y_arr[i] = data[i*columns + 1 + column_offset];
            z_arr[i] = data[i*columns + 2 + column_offset];

            if (fill_std)
            {
                yStd_arr[i] = data[columns * (TRAJECTORY_SIZE + i) + 1 + column_offset];
                zStd_arr[i] = data[columns * (TRAJECTORY_SIZE + i) + 2 + column_offset];
            }
        }
    }

    public void fillXYZT(ArrayList<float[]>xyzt, float[] data, int start, int columns, int column_offset, float[] plan_t_arr, boolean fill_std)
    {
        float[] x_arr = xyzt.get(0);
        float[] y_arr = xyzt.get(1);
        float[] z_arr = xyzt.get(2);
        float[] t_arr = xyzt.get(3);
        float[] xStd_arr = xyzt.get(4);
        float[] yStd_arr = xyzt.get(5);
        float[] zStd_arr = xyzt.get(6);

        for(int i = 0; i < TRAJECTORY_SIZE; i++)
        {
            if (column_offset >= 0)
            {
                t_arr[i] = T_IDXS[i];
                x_arr[i] = data[start + i * columns + column_offset];
                if (fill_std)
                    xStd_arr[i] = data[start + columns * (TRAJECTORY_SIZE + i) + column_offset];
            }
            else
            {
                t_arr[i] = plan_t_arr[i];
                x_arr[i] = X_IDXS[i];
                xStd_arr[i] = Float.NaN;
            }
            y_arr[i] = data[start + i*columns + 1 + column_offset];
            z_arr[i] = data[start + i*columns + 2 + column_offset];

            if (fill_std)
            {
                yStd_arr[i] = data[start + columns * (TRAJECTORY_SIZE + i) + 1 + column_offset];
                zStd_arr[i] = data[start + columns * (TRAJECTORY_SIZE + i) + 2 + column_offset];
            }
        }
    }

    public float[] get_best_data(float[] data, int size, int group_size, int offset)
    {
        int max_idx = 0;
        for(int i=1; i < size; i++)
            if (data[(i + 1) * group_size + offset] > data[(max_idx + 1) * group_size + offset])
                max_idx = i;
        return Arrays.copyOfRange(data, max_idx * group_size, (max_idx+1) * group_size);
    }

    public float[] get_lead_data(float[] lead, int t_offset)
    {
        return get_best_data(lead, LEAD_MHP_N, LEAD_MHP_GROUP_SIZE, t_offset - LEAD_MHP_SELECTION);
    }

    public void fill_lead_v3(LeadDataV3 lead, float[] lead_data, float[] alldata, int t_offset, float prob_t)
    {
        float[] data = get_lead_data(lead_data, t_offset);
        lead.prob = sigmoid(alldata[t_offset + LEAD_PROB_IDX]);
        lead.probTime = prob_t;
        float[] x_arr = lead.x;
        float[] y_arr = lead.y;
        float[] v_arr = lead.v;
        float[] a_arr = lead.a;
        float[] x_stds_arr = lead.XStd;
        float[] y_stds_arr = lead.YStd;
        float[] v_stds_arr = lead.VStd;
        float[] a_stds_arr = lead.AStd;

        for(int i = 0; i < LEAD_TRAJ_LEN; i++)
        {
            x_arr[i] = data[i * LEAD_PRED_DIM + 0];
            y_arr[i] = data[i * LEAD_PRED_DIM + 1];
            v_arr[i] = data[i * LEAD_PRED_DIM + 2];
            a_arr[i] = data[i * LEAD_PRED_DIM + 3];
            x_stds_arr[i] = (float) Math.exp(data[LEAD_MHP_VALS + i * LEAD_PRED_DIM + 0]);
            y_stds_arr[i] = (float) Math.exp(data[LEAD_MHP_VALS + i * LEAD_PRED_DIM + 1]);
            v_stds_arr[i] = (float) Math.exp(data[LEAD_MHP_VALS + i * LEAD_PRED_DIM + 2]);
            a_stds_arr[i] = (float) Math.exp(data[LEAD_MHP_VALS + i * LEAD_PRED_DIM + 3]);
        }
    }

    public void fill_sigmoid(float[] input, float[] output, int offset, int len, int stride)
    {
        for (int i=0; i<len; i++)
            output[i] = sigmoid(input[DESIRE_LEN+offset+i*stride]);
    }

    public void fillMeta(float[] metaData) {
        float[] desire_state_softmax = metaDataOutput.desireState;
        float[] desire_pred_softmax = metaDataOutput.desirePrediction;
        float[] gas_disengage_sigmoid = metaDataOutput.disengagePredictions.gasDesengageProbs;
        float[] brake_disengage_sigmoid = metaDataOutput.disengagePredictions.brakeDisengageProbs;
        float[] steer_override_sigmoid = metaDataOutput.disengagePredictions.steerOverrideProbs;
        float[] brake_3ms2_sigmoid = metaDataOutput.disengagePredictions.brake3MetersPerSecondSquaredProbs;
        float[] brake_4ms2_sigmoid = metaDataOutput.disengagePredictions.brake4MetersPerSecondSquaredProbs;
        float[] brake_5ms2_sigmoid = metaDataOutput.disengagePredictions.brake5MetersPerSecondSquaredProbs;

        System.arraycopy(metaData, 0, desire_state_softmax, 0, DESIRE_LEN);
        softmax(desire_state_softmax, desire_state_softmax);
        for (int i=0; i<4; i++)
        {
            System.arraycopy(metaData, DESIRE_LEN+OTHER_META_SIZE+i*DESIRE_LEN, desire_pred_softmax, 0, DESIRE_LEN);
            softmax(desire_pred_softmax, desire_pred_softmax);
        }

        fill_sigmoid(metaData, gas_disengage_sigmoid, 1, NUM_META_INTERVALS, META_STRIDE);
        fill_sigmoid(metaData, brake_disengage_sigmoid, 2, NUM_META_INTERVALS, META_STRIDE);
        fill_sigmoid(metaData, steer_override_sigmoid, 3, NUM_META_INTERVALS, META_STRIDE);
        fill_sigmoid(metaData, brake_3ms2_sigmoid, 4, NUM_META_INTERVALS, META_STRIDE);
        fill_sigmoid(metaData, brake_4ms2_sigmoid, 5, NUM_META_INTERVALS, META_STRIDE);
        fill_sigmoid(metaData, brake_5ms2_sigmoid, 6, NUM_META_INTERVALS, META_STRIDE);

        System.arraycopy(prev_brake_5ms2_probs, 1, prev_brake_5ms2_probs, 0, 4);
        System.arraycopy(prev_brake_3ms2_probs, 1, prev_brake_3ms2_probs, 0, 2);
        prev_brake_5ms2_probs[4] = brake_5ms2_sigmoid[0];
        prev_brake_3ms2_probs[2] = brake_3ms2_sigmoid[0];

        boolean above_fcw_threshold = true;
        for(int i = 0; i < 5; i++)
        {
            float threshold = i < 2 ? FCW_THRESHOLD_5MS2_LOW : FCW_THRESHOLD_5MS2_HIGH;
            above_fcw_threshold = above_fcw_threshold && prev_brake_5ms2_probs[i] > threshold;
        }
        for(int i = 0; i < 3; i++)
            above_fcw_threshold = above_fcw_threshold && prev_brake_3ms2_probs[i] > FCW_THRESHOLD_3MS2;

        metaDataOutput.engagedProb = sigmoid(metaData[DESIRE_LEN]);
        metaDataOutput.hardBrakePredicted = above_fcw_threshold;
    }

    public ParsedOutputs parser(float[] outs){
        stopSignProb = 0;  // F3 has no stoplines block

        copyOfRange(outs, net_outputs.get("lead"), LEAD_IDX, LEAD_IDX + SIZE_ModelOutputLeads);
        copyOfRange(outs, net_outputs.get("meta"), META_IDX, META_IDX + SIZE_ModelOutputMeta);
        copyOfRange(outs, net_outputs.get("pose"), POSE_IDX, POSE_IDX + SIZE_ModelOutputPose);

        getBestPlan(outs, best_plan, PLAN_IDX);

        plan_t_arr[0] = 0.0f;
        for (int xidx=1, tidx=0; xidx<TRAJECTORY_SIZE; xidx++) {
            while (tidx < TRAJECTORY_SIZE-2 && best_plan[(tidx+1)*PLAN_MHP_COLUMNS] < X_IDXS[xidx]) {
                tidx++;
            }
            float current_x_val = best_plan[tidx*PLAN_MHP_COLUMNS];
            float next_x_val = best_plan[(tidx+1)*PLAN_MHP_COLUMNS];
            if (next_x_val < X_IDXS[xidx]) {
                plan_t_arr[xidx] = T_IDXS[TRAJECTORY_SIZE-1];
                break;
            } else {
                float p = (X_IDXS[xidx] - current_x_val) / (next_x_val - current_x_val);
                plan_t_arr[xidx] = p * T_IDXS[tidx+1] + (1 - p) * T_IDXS[tidx];
            }
        }

        fillXYZT(parsed.position, best_plan, PLAN_MHP_COLUMNS, 0, plan_t_arr, true);
        fillXYZT(parsed.velocity, best_plan, PLAN_MHP_COLUMNS, 3, plan_t_arr, false);
        fillXYZT(parsed.orientation, best_plan, PLAN_MHP_COLUMNS, 9, plan_t_arr, false);
        fillXYZT(parsed.orientationRate, best_plan, PLAN_MHP_COLUMNS, 12, plan_t_arr, false);

        for(int i=0; i < 4; i++)
        {
            fillXYZT(laneLines.get(i), outs, LL_IDX + i*TRAJECTORY_SIZE*2, 2, -1, plan_t_arr, false);
            laneLineProbs[i] = sigmoid(outs[LL_PROB_IDX + i*2+1]);
            laneLineStds[i] = (float) Math.exp(outs[LL_IDX + 2*TRAJECTORY_SIZE*(4+i)]);
        }

        for(int i=0; i<2; i++)
        {
            fillXYZT(roadEdges.get(i), outs, RE_IDX + i*TRAJECTORY_SIZE*2, 2, -1, plan_t_arr, false);
            roadEdgeStds[i] = (float) Math.exp(outs[RE_IDX + 2*TRAJECTORY_SIZE*(2+i)]);
        }

        for(int t_offset=0; t_offset < LEAD_MHP_N; t_offset++)
            fill_lead_v3(leads.get(t_offset), net_outputs.get("lead"), outs, t_offset, t_offsets[t_offset]);

        copyOfRange(net_outputs.get("meta"), meta[0], 0, meta[0].length);
        copyOfRange(net_outputs.get("pose"), pose[0], 0, pose[0].length);
        // feature buffer for temporal feedback sits right after OUTPUT_SIZE
        copyOfRange(outs, state[0], OUTPUT_SIZE, OUTPUT_SIZE+TEMPORAL_SIZE);

        fillMeta(meta[0]);

        copyOfRange(pose[0], trans, 0, 3);
        copyOfRange(pose[0], transStd, 6, 9);
        copyOfRange(pose[0], rot, 3, 6);
        copyOfRange(pose[0], rotStd, 9, 12);

        for (int i =0; i < 3; i ++)
        {
            transStd[i] = (float) Math.exp(transStd[i]);
            rotStd[i] = (float) Math.exp(rotStd[i]);
        }

        for(int j=0; j < rotStd.length; j++){
            rotStd[j] *= Math.PI / 180.0;
            rot[j] *= Math.PI / 180.0;
        }

        return parsed;
    }
}
