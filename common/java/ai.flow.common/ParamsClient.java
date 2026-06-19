package ai.flow.common;

import messaging.Utils;
import org.zeromq.ZMQ;
import org.zeromq.ZMQException;
import org.zeromq.ZMsg;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class ParamsClient extends ParamsInterface {

    // EINTR (errno 4): a blocking ZMQ syscall was interrupted by a signal.
    // This happens intermittently during app/backend startup and used to crash
    // AndroidLauncher.onCreate (ZMQException: Interrupted system call). ZMQ
    // documents EINTR as retriable, so we retry transparently instead of throwing.
    private static final int EINTR = 4;

    private static byte[] recvRetry(ZMQ.Socket sock){
        while (true) {
            try {
                return sock.recv();
            } catch (ZMQException e) {
                if (e.getErrorCode() == EINTR) continue;
                throw e;
            }
        }
    }

    private static byte[] recvRetry(ZMQ.Socket sock, int flags){
        while (true) {
            try {
                return sock.recv(flags);
            } catch (ZMQException e) {
                if (e.getErrorCode() == EINTR) continue;
                throw e;
            }
        }
    }

    public static final ZMQ.Context context = ZMQ.context(4);

    public ZMQ.Socket sockGet;
    public ZMQ.Socket sockPut;
    public ZMQ.Socket sockDel;
    public ZMQ.Poller poller = context.poller();

    public ParamsClient(){
        sockGet = context.socket(ZMQ.REQ);
        sockPut = context.socket(ZMQ.REQ);
        sockDel = context.socket(ZMQ.REQ);
        sockGet.connect(Utils.getSocketPath("6001")); // get socket
        sockPut.connect(Utils.getSocketPath("6002")); // put socket
        sockDel.connect(Utils.getSocketPath("6003")); // delete socket

        sockGet.setLinger(50);
        poller.register(sockGet, ZMQ.Poller.POLLIN);
    }

    public ZMsg makeFrame(byte[]... bytes){
        ZMsg msg = new ZMsg();
        for (byte[] b : bytes)
            msg.add(b);
        return msg;
    }

    public void putInt(String key, int value){
        ZMsg msg = makeFrame(key.getBytes(), ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(value).array());
        msg.send(sockPut);
        recvRetry(sockPut);
    }

    public void putFloat(String key, float value){
        ZMsg msg = makeFrame(key.getBytes(), ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putFloat(value).array());
        msg.send(sockPut);
        recvRetry(sockPut);
    }

    public void putShort(String key, short value){
        ZMsg msg = makeFrame(key.getBytes(), ByteBuffer.allocate(2).order(ByteOrder.LITTLE_ENDIAN).putShort(value).array());
        msg.send(sockPut);
        recvRetry(sockPut);
    }

    public void putLong(String key, long value){
        ZMsg msg = makeFrame(key.getBytes(), ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(value).array());
        msg.send(sockPut);
        recvRetry(sockPut);
    }

    public void putBool(String key, boolean value){
        ZMsg msg = makeFrame(key.getBytes(), value?"1".getBytes():"0".getBytes());
        msg.send(sockPut);
        recvRetry(sockPut);
    }

    public void putDouble(String key, double value){
        ZMsg msg = makeFrame(key.getBytes(), ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putDouble(value).array());
        msg.send(sockPut);
        recvRetry(sockPut);
    }

    public void put(String key, byte[] value){
        ZMsg msg = makeFrame(key.getBytes(), value);
        msg.send(sockPut);
        recvRetry(sockPut);
    }

    public void put(String key, String value){
        ZMsg msg = makeFrame(key.getBytes(), value.getBytes());
        msg.send(sockPut);
        recvRetry(sockPut);
    }

    public byte[] getBytes(String key){
        sockGet.send(key.getBytes(), 0);
        return recvRetry(sockGet);
    }

    public int getInt(String key){
        sockGet.send(key.getBytes(), 0);
        byte[] data = recvRetry(sockGet);
        return ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getInt();
    }

    public float getFloat(String key){
        sockGet.send(key.getBytes(), 0);
        byte[] data = recvRetry(sockGet);
        return ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getFloat();
    }

    public short getShort(String key){
        sockGet.send(key.getBytes(), 0);
        byte[] data = recvRetry(sockGet);
        return ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getShort();
    }

    public long getLong(String key){
        sockGet.send(key.getBytes(), 0);
        byte[] data = recvRetry(sockGet);
        return ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getLong();
    }

    public double getDouble(String key){
        sockGet.send(key.getBytes(), 0);
        byte[] data = recvRetry(sockGet);
        return ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getDouble();
    }

    public String getString(String key){
        sockGet.send(key.getBytes(), 0);
        byte[] data = recvRetry(sockGet);
        return new String(data);
    }

    public boolean getBool(String key){
        sockGet.send(key.getBytes(), 0);
        byte[] data = recvRetry(sockGet);
        return new String(data).equals("1");
    }

    public void deleteKey(String key){
        sockDel.send(key.getBytes(), 0);
        recvRetry(sockDel);
    }

    public boolean exists(String key){
        sockGet.send(key.getBytes(), 0);
        byte[] data = recvRetry(sockGet);
        return data.length != 0;
    }

    public void blockTillExists(String key) throws InterruptedException {
        while (!exists(key))
            Thread.sleep(50);
    }

    public boolean existsAndCompare(String key, boolean value){
        if (exists(key)) {
            return getBool(key) == value;
        }
        return false;
    }

    public boolean initialized(){
        sockGet.send("Passive".getBytes(), 0);
        poller.poll(50);
        return poller.pollin(0);
    }

    public void dispose(){
        sockGet.close();
        sockDel.close();
        sockPut.close();
    }
}

