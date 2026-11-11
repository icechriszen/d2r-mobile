package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.lang.ref.WeakReference;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/** Native platform services only. Snapshot and conflict decisions live in dm/sync. */
public final class CloudSaves {
    static native String status();
    static native int connect(String json);
    static native String connectionFile();
    static native int importConnection();
    static native void command(String action, String argument);
    static native void progress(long received, long total);
    private static WeakReference<Activity> activity = new WeakReference<>(null);
    private static Context context;
    private static volatile HttpURLConnection active;
    private static AlertDialog progress;
    private static final String ALIAS = "d2r-personal-save-sync";
    private static final int MAX = 16 * 1024 * 1024;
    static void init(Activity a) { activity = new WeakReference<>(a); context = a.getApplicationContext(); }
    public static byte[] pathKey(byte[] bytes) {
        try {
            String value=StandardCharsets.UTF_8.newDecoder().onMalformedInput(java.nio.charset.CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();
            return java.text.Normalizer.normalize(value,java.text.Normalizer.Form.NFC).toLowerCase(java.util.Locale.ROOT).getBytes(StandardCharsets.UTF_8);
        }catch(Exception e){return null;}
    }
    private static SecretKey key() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore"); store.load(null);
        if (store.containsAlias(ALIAS)) return (SecretKey) store.getKey(ALIAS, null);
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());
        return generator.generateKey();
    }
    public static synchronized boolean credentialsStore(String json) {
        if (context == null) return false;
        SharedPreferences p = context.getSharedPreferences(ALIAS, Context.MODE_PRIVATE);
        if (json == null) return p.edit().clear().commit();
        try {
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding"); cipher.init(Cipher.ENCRYPT_MODE, key());
            byte[] data = cipher.doFinal(json.getBytes(StandardCharsets.UTF_8));
            return p.edit().putString("data", Base64.encodeToString(data, Base64.NO_WRAP))
                .putString("iv", Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP)).commit();
        } catch (Exception e) { return false; }
    }
    public static synchronized String credentialsLoad() {
        if (context == null) return null;
        SharedPreferences p = context.getSharedPreferences(ALIAS, Context.MODE_PRIVATE);
        String value = p.getString("data", null); if (value == null) return null;
        try {
            Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
            c.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(128, Base64.decode(p.getString("iv", ""), Base64.NO_WRAP)));
            return new String(c.doFinal(Base64.decode(value, Base64.NO_WRAP)), StandardCharsets.UTF_8);
        } catch (Exception e) { return null; }
    }
    public static void cancelRequest() { HttpURLConnection c = active; if (c != null) c.disconnect(); }
    public static byte[] request(String method, String address, String token, byte[] body, int timeout) {
        java.util.concurrent.atomic.AtomicReference<HttpURLConnection> connection=new java.util.concurrent.atomic.AtomicReference<>();
        java.util.concurrent.FutureTask<byte[]> task=new java.util.concurrent.FutureTask<>(()->requestBody(method,address,token,body,timeout,connection));
        Thread worker=new Thread(task,"save-https");worker.setDaemon(true);worker.start();
        try{return task.get(timeout,java.util.concurrent.TimeUnit.MILLISECONDS);}
        catch(Exception e){HttpURLConnection c=connection.get();if(c!=null)c.disconnect();task.cancel(true);return ByteBuffer.allocate(4).putInt(-1).array();}
    }
    private static byte[] requestBody(String method,String address,String token,byte[] body,int timeout,java.util.concurrent.atomic.AtomicReference<HttpURLConnection> connection) {
        HttpURLConnection c = null;
        try {
            URL url = new URL(address);
            if (!url.getProtocol().equals("https") || url.getUserInfo() != null) throw new IllegalArgumentException();
            c = (HttpURLConnection) url.openConnection(); connection.set(c); active = c;
            c.setInstanceFollowRedirects(false); c.setUseCaches(false); c.setConnectTimeout(timeout); c.setReadTimeout(timeout);
            c.setRequestMethod(method); c.setRequestProperty("Authorization", "Bearer " + token);
            c.setRequestProperty("Content-Type", address.contains("/snapshots/") ? "application/zip" : "application/json");
            if (body.length > 0) { c.setDoOutput(true); c.setFixedLengthStreamingMode(body.length); try(java.io.OutputStream out = c.getOutputStream()) { out.write(body); } }
            int code = c.getResponseCode();
            if (c.getContentLengthLong() > MAX) throw new IllegalArgumentException();
            ByteArrayOutputStream bytes = new ByteArrayOutputStream(); bytes.write(ByteBuffer.allocate(4).putInt(code).array());
            InputStream stream = code >= 400 ? c.getErrorStream() : c.getInputStream();
            if (stream != null) try (InputStream in = stream) {
                byte[] buf = new byte[8192]; int n,last=-1;long total=Math.max(0,c.getContentLengthLong());
                while ((n = in.read(buf)) != -1) {
                    if (bytes.size() + n > MAX + 4) throw new IllegalArgumentException(); bytes.write(buf, 0, n);
                    int percent=total>0?(int)(100L*(bytes.size()-4)/total):(bytes.size()-4)/16384;
                    if(percent!=last){last=percent;progress(bytes.size()-4,total);}
                }
            }
            return bytes.toByteArray();
        } catch (Exception e) { return ByteBuffer.allocate(4).putInt(-1).array(); }
        finally { if (c != null) c.disconnect(); if (active == c) active = null; }
    }
    private static native String text(String key);
    private static JSONObject state() { try { return new JSONObject(status()); } catch (Exception e) { return new JSONObject(); } }

    // 主动粘贴才读剪贴板，连接确认与结果由共用游戏页面显示。
    // Read clipboard only for explicit Paste; the shared game page handles confirmation/results.
    private static native void pasted(String code);
    private static native boolean gameUIReady();
    public static void paste() {
        Activity a=activity.get();if(a==null){pasted(null);return;}
        a.runOnUiThread(()->{String value=null;try{
            android.content.ClipboardManager cb=(android.content.ClipboardManager)a.getSystemService(Context.CLIPBOARD_SERVICE);
            android.content.ClipData clip=cb.getPrimaryClip();
            if(clip!=null&&clip.getItemCount()>0){CharSequence text=clip.getItemAt(0).getText();if(text!=null&&text.length()<=4096)value=text.toString();}
        }catch(RuntimeException ignored){}pasted(value);});
    }
    public static void show(){command("show","");}
    public static void changed(){
        Activity a=activity.get();if(a==null)return;a.runOnUiThread(()->{
            if(a.isFinishing())return;JSONObject s=state();
            boolean downloading=!gameUIReady()&&s.optString("status").equals("downloading");
            if(!downloading){if(progress!=null)progress.dismiss();progress=null;return;}
            if(progress==null){progress=new AlertDialog.Builder(a).setTitle(text("dmSyncDownloadTitle"))
                .setView(new android.widget.ProgressBar(a)).setNegativeButton(text("dmSyncCancelOffline"),(d,w)->command("cancel",""))
                .setCancelable(false).create();progress.show();}
            long total=s.optLong("total"),received=s.optLong("received");
            progress.setMessage(total>0?String.format(text("dmSyncDownloadPercentAndroid"),Math.min(100,100*received/total)):String.format(text("dmSyncDownloadKBAndroid"),received/1024));
        });
    }
}
