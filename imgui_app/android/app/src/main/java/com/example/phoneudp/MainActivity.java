package com.example.phoneudp;

import android.app.Activity;
import android.content.ClipboardManager;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.method.ScrollingMovementMethod;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;

public class MainActivity extends Activity {

    static {
        System.loadLibrary("phone_udp");
    }

    private EditText etIp, etPort, etPid, etAddr, etLen;
    private CheckBox cbRoot;
    private TextView tvLog;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final StringBuilder logBuf = new StringBuilder();
    private volatile boolean sending = false;
    private Thread sendThread;

    // ---- JNI（由 C++ 实现，见 common/ + jni_bridge）----
    // 通过 root 读取指定进程内存(直接使用 su 执行 dd 从 /proc/PID/mem 读取)
    private native byte[] nativeRootReadMem(int pid, long addr, long len);
    // 执行任意 root 命令，返回输出
    private native String nativeRootExec(String cmd);
    // 启动可靠UDP发送（后台线程驱动 pump）
    private native boolean nativeSendStart(String ip, int port, byte[] data);
    // 轮询发送进度（返回是否完成）；在后台线程调用
    private native boolean nativeSendPump();
    private native long nativeSendAcked();
    private native String nativeSendStats();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        etIp = findViewById(R.id.etIp);
        etPort = findViewById(R.id.etPort);
        etPid = findViewById(R.id.etPid);
        etAddr = findViewById(R.id.etAddr);
        etLen = findViewById(R.id.etLen);
        cbRoot = findViewById(R.id.cbRoot);
        tvLog = findViewById(R.id.tvLog);
        tvLog.setMovementMethod(new ScrollingMovementMethod());
        tvLog.setTextColor(Color.BLACK);

        Button btnRead = findViewById(R.id.btnReadAndSend);
        Button btnSample = findViewById(R.id.btnSendSample);
        Button btnCmd = findViewById(R.id.btnRootCmd);

        btnRead.setOnClickListener(v -> readAndSend());
        btnSample.setOnClickListener(v -> sendSample());
        btnCmd.setOnClickListener(v -> runRootCmd());

        appendLog("已就绪。输入电脑 IP/端口，勾选 root 后读取进程内存并发送。");
    }

    private void readAndSend() {
        if (sending) { toast("正在发送，请稍候"); return; }
        final int pid = parseInt(etPid.getText().toString());
        final long addr = parseHex(etAddr.getText().toString());
        final long len = parseLong(etLen.getText().toString());
        if (pid <= 0 || addr < 0 || len <= 0) {
            appendLog("需填写有效的 PID / 内存地址 / 长度。");
            return;
        }
        appendLog("root 读取内存 pid=%d addr=0x%x len=%d ..." .formatted(pid, addr, len));
        new Thread(() -> {
            try {
                byte[] data = nativeRootReadMem(pid, addr, len);
                if (data == null || data.length == 0) {
                    ui.post(() -> appendLog("读取内存失败（无 root 或地址无效）。"));
                    return;
                }
                appendLog("读取成功 " + data.length + " 字节，开始发送...");
                doSend(data);
            } catch (Throwable t) {
                ui.post(() -> appendLog("异常：" + t));
            }
        }).start();
    }

    private void sendSample() {
        if (sending) { toast("正在发送"); return; }
        byte[] blob = new byte[]{0x00,0x1c,0x00,0x91, 0x00,0x7c,0x00,0x9b,
                0x21,0x00,0xa0,0xd2, 0x20,0x00,0x00,0xf9, 0xc0,0x03,0x5f,0xd6};
        appendLog("发送示例 blob %d 字节" .formatted(blob.length));
        new Thread(() -> doSend(blob)).start();
    }

    private void runRootCmd() {
        String cmd = etPid.getText().toString().trim();
        if (cmd.isEmpty()) { appendLog("在 PID 输入框里填要执行的 root 命令，例如：id"); return; }
        appendLog("root 执行：" + cmd);
        new Thread(() -> {
            try {
                String out = nativeRootExec(cmd);
                ui.post(() -> appendLog("输出：\n" + out));
            } catch (Throwable t) {
                ui.post(() -> appendLog("异常：" + t));
            }
        }).start();
    }

    private void doSend(byte[] data) {
        sending = true;
        String ip = etIp.getText().toString().trim();
        int port = parseInt(etPort.getText().toString());
        if (port <= 0) port = 8010;
        boolean ok = nativeSendStart(ip, port, data);
        if (!ok) { sending = false; ui.post(() -> appendLog("发送初始化失败。")); return; }
        sendThread = new Thread(() -> {
            long lastT = System.currentTimeMillis();
            try {
                while (!Thread.currentThread().isInterrupted()) {
                    long t = System.currentTimeMillis();
                    if (t - lastT > 250) {
                        ui.post(() -> appendLogProgress("已确认 " + nativeSendAcked() + " / " + data.length + " 字节"));
                        lastT = t;
                    }
                    boolean done = nativeSendPump();
                    if (done) { ui.post(() -> appendLogProgress("发送完成。")); break; }
                    Thread.sleep(20);
                }
            } catch (InterruptedException ignored) {
            } finally {
                sending = false;
            }
        });
        sendThread.start();
    }

    private void appendLog(String s) {
        logBuf.append(s).append("\n");
        if (logBuf.length() > 8000) logBuf.delete(0, logBuf.length() - 6000);
        tvLog.setText(logBuf.toString());
        ui.post(() -> { if (tvLog.getLineCount() > 0) tvLog.scrollTo(0, tvLog.getLayout() != null ? tvLog.getLayout().getHeight() : 0); });
    }

    private void appendLogProgress(String s) {
        logBuf.append(s).append("\n");
        if (logBuf.length() > 8000) logBuf.delete(0, logBuf.length() - 6000);
        tvLog.setText(logBuf.toString());
    }

    private void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_SHORT).show(); }
    private static int parseInt(String s) {
        try { if (s == null) return 0; return Integer.parseInt(s.trim()); } catch (Exception e) { return 0; }
    }
    private static long parseLong(String s) {
        try { if (s == null) return 0; return Long.parseLong(s.trim()); } catch (Exception e) { return 0; }
    }
    private static long parseHex(String s) {
        try { if (s == null) return -1; String t = s.trim(); return Long.parseLong(t.replace("0x", "").replace("0X", ""), 16); } catch (Exception e) { return -1; }
    }
}